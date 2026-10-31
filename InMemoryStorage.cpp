#include "InMemoryStorage.h"
#include "RespParser.h"
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace {

using std::chrono::milliseconds;

ExpiryTime steadyNowMs() {
    return std::chrono::time_point_cast<milliseconds>(std::chrono::steady_clock::now());
}

// a + b, clamped to the range of int64_t. Signed overflow is undefined
// behaviour, and clients choose these numbers: PXAT takes any 64-bit value.
int64_t addSaturating(int64_t a, int64_t b) {
    if(b > 0 && a > INT64_MAX - b) return INT64_MAX;
    if(b < 0 && a < INT64_MIN - b) return INT64_MIN;
    return a + b;
}

// The monotonic time at which the wall clock will read `unixTimeMs`
ExpiryTime toExpiryTime(int64_t unixTimeMs) {
    int64_t remaining = addSaturating(unixTimeMs, -InMemoryStorage::unixTimeMs());
    return ExpiryTime(milliseconds(addSaturating(steadyNowMs().time_since_epoch().count(), remaining)));
}

// The Unix time, in milliseconds, at which `expiry` comes
int64_t toUnixTimeMs(ExpiryTime expiry) {
    int64_t remaining = addSaturating(expiry.time_since_epoch().count(),
                                      -steadyNowMs().time_since_epoch().count());
    return addSaturating(InMemoryStorage::unixTimeMs(), remaining);
}

}  // namespace

int64_t InMemoryStorage::unixTimeMs() {
    // system_clock counts from the Unix epoch (required from C++20, and already
    // true of every C++17 standard library)
    return std::chrono::duration_cast<milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

InMemoryStorage::InMemoryStorage(size_t capacity, AOFLogger* aof)
    : capacity_(capacity), aof_(aof) {
    // Eviction makes room by removing a key, so at least one must fit. With
    // capacity 0, the first SET would evict from an empty LRU list.
    if(capacity_ == 0) throw std::invalid_argument("InMemoryStorage capacity must be at least 1");
}

// Centralized AOF logging — serializes command parts as RESP and writes to AOF.
// RESP format is binary-safe: values with spaces, newlines etc. are length-prefixed.
void InMemoryStorage::logAOF(const std::vector<std::string>& parts) {
    if(aof_ && !loading_) {
        aof_->log(RespParser::array(parts));
    }
}

// ==================== Type Helpers ====================

DataType InMemoryStorage::getDataType(const CacheEntry& entry) const {
    if(std::holds_alternative<std::string>(entry.value)) return DataType::STRING;
    if(std::holds_alternative<std::deque<std::string>>(entry.value)) return DataType::LIST;
    if(std::holds_alternative<std::unordered_set<std::string>>(entry.value)) return DataType::SET;
    return DataType::HASH;
}

std::string InMemoryStorage::dataTypeToString(DataType dt) const {
    switch(dt) {
        case DataType::STRING: return "string";
        case DataType::LIST:   return "list";
        case DataType::SET:    return "set";
        case DataType::HASH:   return "hash";
    }
    return "none";
}

// Returns a valid, non-expired entry of the expected type.
// Returns nullptr if key doesn't exist, is expired, or has wrong type.
// Caller must hold appropriate lock.
InMemoryStorage::CacheEntry* InMemoryStorage::getValidEntry(const std::string& key, DataType expected) {
    auto it = data_.find(key);
    if(it == data_.end()) return nullptr;
    if(isExpired(it->second)) {
        expireKey(key);
        return nullptr;
    }
    if(getDataType(it->second) != expected) return nullptr;
    return &it->second;
}

// Gets or creates an entry of the expected type.
// If key exists with a different type, returns nullptr (type mismatch).
// If key exists but is expired, removes it and creates a new one.
// Caller must hold unique_lock.
InMemoryStorage::CacheEntry* InMemoryStorage::getOrCreateEntry(const std::string& key, DataType expected) {
    auto it = data_.find(key);
    if(it != data_.end()) {
        if(isExpired(it->second)) {
            expireKey(key);
        } else if(getDataType(it->second) != expected) {
            return nullptr;     // type mismatch
        } else {
            touchLRU(key);
            return &it->second; // existing entry of correct type
        }
    }
    // Create new entry
    makeRoomForNewKey();
    lru_list_.push_front(key);
    CacheEntry entry;
    switch(expected) {
        case DataType::STRING: entry.value = std::string(); break;
        case DataType::LIST:   entry.value = std::deque<std::string>(); break;
        case DataType::SET:    entry.value = std::unordered_set<std::string>(); break;
        case DataType::HASH:   entry.value = std::unordered_map<std::string,std::string>(); break;
    }
    entry.has_expiry = false;
    entry.lru_it = lru_list_.begin();
    return &data_.emplace(key, std::move(entry)).first->second;
}

// ==================== Key Operations ====================

Result InMemoryStorage::exists(const std::string& key){
    std::unique_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(it->second)) {
        expireKey(key);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    return Result::ok();
}

Result InMemoryStorage::del(const std::string& key){
    std::unique_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    // An expired key no longer exists: DEL returns 0, as in Redis. Its removal
    // is still logged, as an expiration.
    if(isExpired(it->second)) {
        expireKey(key);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    eraseEntry(it);
    logAOF({"DEL", key});
    return Result::ok();
}

Result InMemoryStorage::type(const std::string& key){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::ok("none");
    if(isExpired(it->second)) return Result::ok("none");
    return Result::ok(dataTypeToString(getDataType(it->second)));
}

Result InMemoryStorage::expire(const std::string& key, int ttlSeconds){
    return expireAt(key, addSaturating(unixTimeMs(), ttlSeconds * int64_t(1000)));
}

// Logged as PEXPIREAT with the absolute time, so replay expires the key at the
// same moment, however much later the restart is
Result InMemoryStorage::expireAt(const std::string& key, int64_t unixTimeMs){
    std::unique_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(it->second)) {
        expireKey(key);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    setExpiry(it, toExpiryTime(unixTimeMs));
    logAOF({"PEXPIREAT", key, std::to_string(unixTimeMs)});
    return Result::ok();
}

Result InMemoryStorage::ttl(const std::string& key){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(it->second)) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(!it->second.has_expiry) return Result::ok("-1");
    int64_t remaining_ms = addSaturating(it->second.expiry_time.time_since_epoch().count(),
                                         -steadyNowMs().time_since_epoch().count());
    // Rounded to the nearest second, as Redis does: right after "SET k v EX 10"
    // this is 10, not 9
    return Result::ok(std::to_string(addSaturating(remaining_ms, 500) / 1000));
}

// ==================== String Operations ====================

Result InMemoryStorage::get(const std::string& key){
    std::unique_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(it->second)) {
        expireKey(key);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    // Type check: GET only works on strings
    if(getDataType(it->second) != DataType::STRING) {
        return Result::fail(ErrorCode::WRONG_TYPE);
    }
    touchLRU(key);
    return Result::ok(std::get<std::string>(it->second.value));
}

Result InMemoryStorage::set(const std::string& key, const std::string& value){
    std::unique_lock lock(mutex_);
    auto r = setInternal(key, value, std::nullopt);
    if(r.success) logAOF({"SET", key, value});
    return r;
}

// A TTL of 0 or less stores the key without one
Result InMemoryStorage::set(const std::string& key, const std::string& value, int ttlSeconds){
    if(ttlSeconds <= 0) return set(key, value);
    return setExpiringAt(key, value, addSaturating(unixTimeMs(), ttlSeconds * int64_t(1000)));
}

// One record, SET with PXAT, so a crash can't leave the value logged without
// its expiry (as SET followed by PEXPIREAT could)
Result InMemoryStorage::setExpiringAt(const std::string& key, const std::string& value, int64_t unixTimeMs){
    std::unique_lock lock(mutex_);
    auto r = setInternal(key, value, toExpiryTime(unixTimeMs));
    if(r.success) logAOF({"SET", key, value, "PXAT", std::to_string(unixTimeMs)});
    return r;
}

// SET always overwrites regardless of existing type (same as real Redis)
Result InMemoryStorage::setInternal(const std::string& key, const std::string& value,
                                    std::optional<ExpiryTime> expiry){
    auto it = data_.find(key);
    if(it != data_.end()) {
        if(isExpired(it->second)) {
            expireKey(key);
        } else {
            // Overwrite: replace value with string regardless of current type
            it->second.value = value;
            setExpiry(it, expiry);
            touchLRU(key);
            return Result::ok();
        }
    }
    makeRoomForNewKey();
    lru_list_.push_front(key);
    CacheEntry entry;
    entry.value = value;    // stores as std::string in the variant
    entry.has_expiry = false;
    entry.lru_it = lru_list_.begin();
    setExpiry(data_.emplace(key, std::move(entry)).first, expiry);
    return Result::ok();
}

// ==================== List Operations ====================

Result InMemoryStorage::lpush(const std::string& key, const std::vector<std::string>& values){
    std::unique_lock lock(mutex_);
    auto* entry = getOrCreateEntry(key, DataType::LIST);
    if(!entry) return Result::fail(ErrorCode::WRONG_TYPE);
    auto& list = std::get<std::deque<std::string>>(entry->value);
    for(const auto& v : values) {
        list.push_front(v);
    }
    std::vector<std::string> parts = {"LPUSH", key};
    parts.insert(parts.end(), values.begin(), values.end());
    logAOF(parts);
    return Result::ok(static_cast<int>(list.size()));
}

Result InMemoryStorage::rpush(const std::string& key, const std::vector<std::string>& values){
    std::unique_lock lock(mutex_);
    auto* entry = getOrCreateEntry(key, DataType::LIST);
    if(!entry) return Result::fail(ErrorCode::WRONG_TYPE);
    auto& list = std::get<std::deque<std::string>>(entry->value);
    for(const auto& v : values) {
        list.push_back(v);
    }
    std::vector<std::string> parts = {"RPUSH", key};
    parts.insert(parts.end(), values.begin(), values.end());
    logAOF(parts);
    return Result::ok(static_cast<int>(list.size()));
}

Result InMemoryStorage::lpop(const std::string& key){
    std::unique_lock lock(mutex_);
    auto* entry = getValidEntry(key, DataType::LIST);
    if(!entry) {
        auto it = data_.find(key);
        if(it != data_.end() && !isExpired(it->second))
            return Result::fail(ErrorCode::WRONG_TYPE);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    auto& list = std::get<std::deque<std::string>>(entry->value);
    if(list.empty()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    std::string val = list.front();
    list.pop_front();
    // Auto-delete empty lists (same as real Redis)
    if(list.empty()) removeKey(key);
    else touchLRU(key);
    logAOF({"LPOP", key});
    return Result::ok(val);
}

Result InMemoryStorage::rpop(const std::string& key){
    std::unique_lock lock(mutex_);
    auto* entry = getValidEntry(key, DataType::LIST);
    if(!entry) {
        auto it = data_.find(key);
        if(it != data_.end() && !isExpired(it->second))
            return Result::fail(ErrorCode::WRONG_TYPE);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    auto& list = std::get<std::deque<std::string>>(entry->value);
    if(list.empty()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    std::string val = list.back();
    list.pop_back();
    if(list.empty()) removeKey(key);
    else touchLRU(key);
    logAOF({"RPOP", key});
    return Result::ok(val);
}

Result InMemoryStorage::lrange(const std::string& key, int start, int stop){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::ok(std::vector<std::string>{});
    if(isExpired(it->second)) return Result::ok(std::vector<std::string>{});
    if(getDataType(it->second) != DataType::LIST) return Result::fail(ErrorCode::WRONG_TYPE);
    
    const auto& list = std::get<std::deque<std::string>>(it->second.value);
    int len = static_cast<int>(list.size());
    
    // Handle negative indices (like real Redis: -1 = last element)
    if(start < 0) start = std::max(0, len + start);
    if(stop < 0) stop = len + stop;
    if(start >= len || start > stop) return Result::ok(std::vector<std::string>{});
    stop = std::min(stop, len - 1);
    
    std::vector<std::string> result;
    for(int i = start; i <= stop; i++) {
        result.push_back(list[i]);
    }
    return Result::ok(result);
}

Result InMemoryStorage::llen(const std::string& key){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::ok(0);
    if(isExpired(it->second)) return Result::ok(0);
    if(getDataType(it->second) != DataType::LIST) return Result::fail(ErrorCode::WRONG_TYPE);
    return Result::ok(static_cast<int>(std::get<std::deque<std::string>>(it->second.value).size()));
}

// ==================== Set Operations ====================

Result InMemoryStorage::sadd(const std::string& key, const std::vector<std::string>& members){
    std::unique_lock lock(mutex_);
    auto* entry = getOrCreateEntry(key, DataType::SET);
    if(!entry) return Result::fail(ErrorCode::WRONG_TYPE);
    auto& set = std::get<std::unordered_set<std::string>>(entry->value);
    int added = 0;
    for(const auto& m : members) {
        if(set.insert(m).second) added++;
    }
    std::vector<std::string> parts = {"SADD", key};
    parts.insert(parts.end(), members.begin(), members.end());
    logAOF(parts);
    return Result::ok(added);
}

Result InMemoryStorage::srem(const std::string& key, const std::vector<std::string>& members){
    std::unique_lock lock(mutex_);
    auto* entry = getValidEntry(key, DataType::SET);
    if(!entry) {
        auto it = data_.find(key);
        if(it != data_.end() && !isExpired(it->second))
            return Result::fail(ErrorCode::WRONG_TYPE);
        return Result::ok(0);
    }
    auto& set = std::get<std::unordered_set<std::string>>(entry->value);
    int removed = 0;
    for(const auto& m : members) {
        removed += static_cast<int>(set.erase(m));
    }
    if(set.empty()) removeKey(key);
    else touchLRU(key);
    std::vector<std::string> parts = {"SREM", key};
    parts.insert(parts.end(), members.begin(), members.end());
    logAOF(parts);
    return Result::ok(removed);
}

Result InMemoryStorage::smembers(const std::string& key){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::ok(std::vector<std::string>{});
    if(isExpired(it->second)) return Result::ok(std::vector<std::string>{});
    if(getDataType(it->second) != DataType::SET) return Result::fail(ErrorCode::WRONG_TYPE);
    const auto& set = std::get<std::unordered_set<std::string>>(it->second.value);
    return Result::ok(std::vector<std::string>(set.begin(), set.end()));
}

Result InMemoryStorage::sismember(const std::string& key, const std::string& member){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::ok(0);
    if(isExpired(it->second)) return Result::ok(0);
    if(getDataType(it->second) != DataType::SET) return Result::fail(ErrorCode::WRONG_TYPE);
    const auto& set = std::get<std::unordered_set<std::string>>(it->second.value);
    return Result::ok(set.count(member) > 0 ? 1 : 0);
}

Result InMemoryStorage::scard(const std::string& key){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::ok(0);
    if(isExpired(it->second)) return Result::ok(0);
    if(getDataType(it->second) != DataType::SET) return Result::fail(ErrorCode::WRONG_TYPE);
    return Result::ok(static_cast<int>(std::get<std::unordered_set<std::string>>(it->second.value).size()));
}

// ==================== Hash Operations ====================

Result InMemoryStorage::hset(const std::string& key, const std::vector<std::pair<std::string,std::string>>& fields){
    std::unique_lock lock(mutex_);
    auto* entry = getOrCreateEntry(key, DataType::HASH);
    if(!entry) return Result::fail(ErrorCode::WRONG_TYPE);
    auto& hash = std::get<std::unordered_map<std::string,std::string>>(entry->value);
    int added = 0;
    std::vector<std::string> parts = {"HSET", key};
    for(const auto& [field, val] : fields) {
        auto [it, inserted] = hash.insert_or_assign(field, val);
        if(inserted) added++;
        parts.push_back(field);
        parts.push_back(val);
    }
    logAOF(parts);
    return Result::ok(added);
}

Result InMemoryStorage::hget(const std::string& key, const std::string& field){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(it->second)) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(getDataType(it->second) != DataType::HASH) return Result::fail(ErrorCode::WRONG_TYPE);
    const auto& hash = std::get<std::unordered_map<std::string,std::string>>(it->second.value);
    auto fit = hash.find(field);
    if(fit == hash.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    return Result::ok(fit->second);
}

Result InMemoryStorage::hdel(const std::string& key, const std::vector<std::string>& fields){
    std::unique_lock lock(mutex_);
    auto* entry = getValidEntry(key, DataType::HASH);
    if(!entry) {
        auto it = data_.find(key);
        if(it != data_.end() && !isExpired(it->second))
            return Result::fail(ErrorCode::WRONG_TYPE);
        return Result::ok(0);
    }
    auto& hash = std::get<std::unordered_map<std::string,std::string>>(entry->value);
    int removed = 0;
    for(const auto& f : fields) {
        removed += static_cast<int>(hash.erase(f));
    }
    if(hash.empty()) removeKey(key);
    else touchLRU(key);
    std::vector<std::string> parts = {"HDEL", key};
    parts.insert(parts.end(), fields.begin(), fields.end());
    logAOF(parts);
    return Result::ok(removed);
}

Result InMemoryStorage::hgetall(const std::string& key){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::ok(std::vector<std::string>{});
    if(isExpired(it->second)) return Result::ok(std::vector<std::string>{});
    if(getDataType(it->second) != DataType::HASH) return Result::fail(ErrorCode::WRONG_TYPE);
    const auto& hash = std::get<std::unordered_map<std::string,std::string>>(it->second.value);
    // Return as alternating field, value pairs (same as real Redis)
    std::vector<std::string> result;
    for(const auto& [field, val] : hash) {
        result.push_back(field);
        result.push_back(val);
    }
    return Result::ok(result);
}

Result InMemoryStorage::hlen(const std::string& key){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::ok(0);
    if(isExpired(it->second)) return Result::ok(0);
    if(getDataType(it->second) != DataType::HASH) return Result::fail(ErrorCode::WRONG_TYPE);
    return Result::ok(static_cast<int>(std::get<std::unordered_map<std::string,std::string>>(it->second.value).size()));
}

// ==================== Persistence ====================

Result InMemoryStorage::saveToFile(const std::string& filename){
    std::shared_lock lock(mutex_);
    std::ofstream out(filename);
    if(!out.is_open()) return Result::fail(ErrorCode::FILE_ERROR);
    for(const auto& pair : data_) {
        if(isExpired(pair.second)) continue;
        if(getDataType(pair.second) == DataType::STRING) {
            out << "S|" << pair.first << "=" << std::get<std::string>(pair.second.value) << "\n";
        }
    }
    out.close();
    return Result::ok();
}

Result InMemoryStorage::loadFromFile(const std::string& filename){
    std::unique_lock lock(mutex_);
    std::ifstream in(filename);
    if(!in.is_open()) return Result::fail(ErrorCode::FILE_ERROR);
    data_.clear();
    lru_list_.clear();
    expiry_index_.clear();

    std::string line;
    while(std::getline(in, line)) {
        std::string key, value;
        if(line.size() > 2 && line[1] == '|') {
            auto pos = line.find('=', 2);
            if(pos != std::string::npos) {
                key = line.substr(2, pos - 2);
                value = line.substr(pos + 1);
            }
        } else {
            auto pos = line.find('=');
            if(pos != std::string::npos) {
                key = line.substr(0, pos);
                value = line.substr(pos + 1);
            }
        }
        if(!key.empty()) {
            lru_list_.push_back(key);
            CacheEntry entry;
            entry.value = value;
            entry.has_expiry = false;
            entry.lru_it = std::prev(lru_list_.end());
            data_[key] = std::move(entry);
        }
    }
    in.close();
    while(data_.size() > capacity_) evictLRU();
    return Result::ok();
}

// ==================== AOF Operations ====================

// Serialize current in-memory state as the minimum set of RESP-formatted commands
// needed to reproduce it. Used by AOFREWRITE to compact the AOF file.
std::vector<std::string> InMemoryStorage::dumpState() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> commands;
    for(const auto& [key, entry] : data_) {
        if(isExpired(entry)) continue;
        DataType dt = getDataType(entry);
        switch(dt) {
            case DataType::STRING: {
                std::vector<std::string> parts = {"SET", key, std::get<std::string>(entry.value)};
                if(entry.has_expiry) {
                    parts.push_back("PXAT");
                    parts.push_back(std::to_string(toUnixTimeMs(entry.expiry_time)));
                }
                commands.push_back(RespParser::array(parts));
                break;
            }
            case DataType::LIST: {
                const auto& list = std::get<std::deque<std::string>>(entry.value);
                if(!list.empty()) {
                    std::vector<std::string> parts = {"RPUSH", key};
                    parts.insert(parts.end(), list.begin(), list.end());
                    commands.push_back(RespParser::array(parts));
                }
                break;
            }
            case DataType::SET: {
                const auto& s = std::get<std::unordered_set<std::string>>(entry.value);
                if(!s.empty()) {
                    std::vector<std::string> parts = {"SADD", key};
                    parts.insert(parts.end(), s.begin(), s.end());
                    commands.push_back(RespParser::array(parts));
                }
                break;
            }
            case DataType::HASH: {
                const auto& hash = std::get<std::unordered_map<std::string,std::string>>(entry.value);
                if(!hash.empty()) {
                    std::vector<std::string> parts = {"HSET", key};
                    for(const auto& [f, v] : hash) {
                        parts.push_back(f);
                        parts.push_back(v);
                    }
                    commands.push_back(RespParser::array(parts));
                }
                break;
            }
        }
        // Lists, sets and hashes can't carry their expiry in the record that
        // creates them. (Strings carry it as SET ... PXAT, above.)
        if(entry.has_expiry && dt != DataType::STRING) {
            commands.push_back(RespParser::array(
                {"PEXPIREAT", key, std::to_string(toUnixTimeMs(entry.expiry_time))}));
        }
    }
    return commands;
}

// ==================== LRU Helpers ====================

void InMemoryStorage::touchLRU(const std::string& key){
    auto& entry = data_[key]; 
    lru_list_.erase(entry.lru_it);
    lru_list_.push_front(key);
    entry.lru_it = lru_list_.begin();
}

// Evictions and expirations are removals the server decides on its own, so
// each is logged as a DEL, as Redis does. Replay can't repeat these decisions:
// GETs aren't logged, so its LRU order differs, and it expires nothing until
// loading is done (see setLoading). Without the DELs, a restart could bring
// back keys the server had dropped and lose others it still had.

void InMemoryStorage::evictLRU(){
    std::string key = lru_list_.back();     // a copy: erasing removes the original
    eraseEntry(data_.find(key));
    logAOF({"DEL", key});
}

void InMemoryStorage::expireKey(const std::string& key){
    removeKey(key);
    logAOF({"DEL", key});
}

// Called before adding a key. Not while loading: replay repeats the live
// server's evictions from their DEL records instead of choosing its own.
void InMemoryStorage::makeRoomForNewKey(){
    if(loading_ || data_.size() < capacity_) return;    // no clock read on the common path
    // Expired keys go before any live key, wherever they are in the LRU order.
    // Only as many as are needed: each insert does a bounded amount of work.
    ExpiryTime now = steadyNowMs();
    while(data_.size() >= capacity_ && !expiry_index_.empty() &&
          expiry_index_.begin()->first <= now) {
        std::string key = *expiry_index_.begin()->second;   // a copy, as above
        expireKey(key);
    }
    while(data_.size() >= capacity_) evictLRU();
}

void InMemoryStorage::setLoading(bool loading){
    std::unique_lock lock(mutex_);
    loading_ = loading;
    // The AOF may hold more keys than the capacity allows now (a smaller
    // --maxkeys than when it was written). Evicting during replay could drop a
    // key and then rebuild it from its later records alone, a wrong value
    // rather than a missing one, so evict only once everything is loaded.
    if(!loading_) {
        while(data_.size() > capacity_) evictLRU();
    }
}

bool InMemoryStorage::isExpired(const CacheEntry& entry) const{
    if(!entry.has_expiry || loading_) return false;   // see setLoading()
    return steadyNowMs() >= entry.expiry_time;
}

void InMemoryStorage::removeKey(const std::string& key){
    auto it = data_.find(key);
    if(it != data_.end()) eraseEntry(it);
}

void InMemoryStorage::setExpiry(DataMap::iterator it, std::optional<ExpiryTime> expiry){
    CacheEntry& entry = it->second;
    if(entry.has_expiry) expiry_index_.erase({entry.expiry_time, &it->first});
    entry.has_expiry = expiry.has_value();
    if(expiry) {
        entry.expiry_time = *expiry;
        expiry_index_.insert({*expiry, &it->first});
    }
}

void InMemoryStorage::eraseEntry(DataMap::iterator it){
    // Out of the index first: it points at the key that erase() destroys
    if(it->second.has_expiry) expiry_index_.erase({it->second.expiry_time, &it->first});
    lru_list_.erase(it->second.lru_it);
    data_.erase(it);
}