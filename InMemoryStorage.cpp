#include "InMemoryStorage.h"
#include "RespParser.h"
#include <cassert>
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

// The hash table is sized at construction for up to this many keys: 4,355,707
// buckets with libstdc++, about 35 MB at 8 bytes each. See the constructor.
constexpr size_t kPresizedKeys = size_t{1} << 22;

}  // namespace

int64_t InMemoryStorage::unixTimeMs() {
    // system_clock counts from the Unix epoch (required from C++20, and already
    // true of every C++17 standard library)
    return std::chrono::duration_cast<milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

InMemoryStorage::InMemoryStorage(size_t capacity, AOFLogger* aof, EvictionPolicy policy)
    : policy_(policy), capacity_(capacity), aof_(aof) {
    // Eviction makes room by removing a key, so at least one must fit. With
    // capacity 0, the first SET would evict from an empty LRU list.
    if(capacity_ == 0) throw std::invalid_argument("InMemoryStorage capacity must be at least 1");

    // Size the hash table for the capacity now, so it never rehashes while
    // serving. std::unordered_map grows all at once, moving every key: the
    // SET that added the 85,230th key took about 20 ms (bench/insert_latency),
    // and the single-threaded server makes every client wait that long. The
    // cost is the bucket array, 8 bytes per key of capacity, allocated up
    // front. It is capped, so a huge --maxkeys meant as "unlimited" doesn't
    // allocate gigabytes; past the cap the table grows by rehashing, as
    // before. (Redis instead rehashes incrementally, a few buckets at a time.)
    data_.reserve(std::min(capacity_, kPresizedKeys));
}

// AOF logging. Each record is a RESP array (binary-safe: values with spaces or
// newlines are length-prefixed), serialized straight into the AOF's buffer.
// This used to build a vector<string> of copies (twice for the key, through an
// initializer_list) and then a string of the record, before even checking for
// an AOF: 3 allocations per SET with the AOF off, 6 with it on.
void InMemoryStorage::logAOF(std::initializer_list<std::string_view> parts) {
    if(!aof_ || loading_) return;
    aof_->logWith([&](std::string& out) { RespParser::appendArray(out, parts); });
}

void InMemoryStorage::logAOF(std::initializer_list<std::string_view> head,
                             const std::vector<std::string>& tail) {
    if(!aof_ || loading_) return;
    aof_->logWith([&](std::string& out) {
        RespParser::appendLength(out, '*', head.size() + tail.size());
        for(std::string_view part : head) RespParser::appendBulk(out, part);
        for(const auto& part : tail) RespParser::appendBulk(out, part);
    });
}

void InMemoryStorage::logAOF(std::initializer_list<std::string_view> head,
                             const std::vector<std::pair<std::string, std::string>>& pairs) {
    if(!aof_ || loading_) return;
    aof_->logWith([&](std::string& out) {
        RespParser::appendLength(out, '*', head.size() + 2 * pairs.size());
        for(std::string_view part : head) RespParser::appendBulk(out, part);
        for(const auto& [first, second] : pairs) {
            RespParser::appendBulk(out, first);
            RespParser::appendBulk(out, second);
        }
    });
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
            markUsed(it->second);
            return &it->second; // existing entry of correct type
        }
    }
    // Create new entry
    makeRoomForNewKey();
    CacheEntry entry;
    switch(expected) {
        case DataType::STRING: entry.value = std::string(); break;
        case DataType::LIST:   entry.value = std::deque<std::string>(); break;
        case DataType::SET:    entry.value = std::unordered_set<std::string>(); break;
        case DataType::HASH:   entry.value = std::unordered_map<std::string,std::string>(); break;
    }
    entry.has_expiry = false;
    return &insertEntry(key, std::move(entry))->second;
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

// A copy of the value, through readString() (which the server uses directly)
Result InMemoryStorage::get(const std::string& key){
    std::string value;
    Result r = readString(key, [&value](const std::string& stored) { value = stored; });
    if(r.success) r.value = std::move(value);
    return r;
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
            markUsed(it->second);
            return Result::ok();
        }
    }
    makeRoomForNewKey();
    CacheEntry entry;
    entry.value = value;    // stores as std::string in the variant
    entry.has_expiry = false;
    setExpiry(insertEntry(key, std::move(entry)), expiry);
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
    logAOF({"LPUSH", key}, values);
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
    logAOF({"RPUSH", key}, values);
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
    else markUsed(*entry);
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
    else markUsed(*entry);
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
    logAOF({"SADD", key}, members);
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
    else markUsed(*entry);
    logAOF({"SREM", key}, members);
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
    for(const auto& [field, val] : fields) {
        auto [it, inserted] = hash.insert_or_assign(field, val);
        if(inserted) added++;
    }
    logAOF({"HSET", key}, fields);
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
    else markUsed(*entry);
    logAOF({"HDEL", key}, fields);
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
    newest_ = oldest_ = hand_ = nullptr;
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
        if(key.empty()) continue;
        // A key listed twice: the later value wins. This used to replace the
        // entry and leave the first queue node behind, keyless; evicting that
        // node later erased the live key's entry, then data_.end().
        auto it = data_.find(key);
        if(it != data_.end()) {
            it->second.value = value;
        } else {
            CacheEntry entry;
            entry.value = value;
            entry.has_expiry = false;
            insertEntry(key, std::move(entry), /*newest=*/false);   // file order: first line newest
        }
    }
    in.close();
    while(data_.size() > capacity_) evictOne();
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

// ==================== Eviction ====================

// Adding to data_ is the only step that can throw (out of memory); linking
// can't, so a failed insert leaves everything as it was.
InMemoryStorage::DataMap::iterator InMemoryStorage::insertEntry(const std::string& key,
                                                               CacheEntry entry, bool newest){
    auto [it, inserted] = data_.emplace(key, std::move(entry));
    assert(inserted && "insertEntry: the key is already stored");
    (void)inserted;
    it->second.key = &it->first;
    if(newest) linkNewest(it->second);
    else linkOldest(it->second);
    return it;
}

void InMemoryStorage::linkNewest(CacheEntry& entry){
    entry.newer = nullptr;
    entry.older = newest_;
    if(newest_) newest_->newer = &entry;
    else oldest_ = &entry;
    newest_ = &entry;
}

void InMemoryStorage::linkOldest(CacheEntry& entry){
    entry.older = nullptr;
    entry.newer = oldest_;
    if(oldest_) oldest_->older = &entry;
    else newest_ = &entry;
    oldest_ = &entry;
}

void InMemoryStorage::unlink(CacheEntry& entry){
    if(entry.newer) entry.newer->older = entry.older;
    else newest_ = entry.older;
    if(entry.older) entry.older->newer = entry.newer;
    else oldest_ = entry.newer;
    entry.newer = entry.older = nullptr;
}

// A hit.
// - LRU: relinks the entry at the front. O(1) and no allocation, but it
//   writes the entry's two neighbours and the old front, which a big table
//   rarely has in the cache: the profile's 10.8% of GET at pipeline 16.
// - SIEVE: sets the entry's visited bit, in the entry the lookup has just
//   read, so a hit touches no other memory.
void InMemoryStorage::markUsed(CacheEntry& entry){
    if(policy_ == EvictionPolicy::SIEVE) {
        entry.visited = true;
    } else if(&entry != newest_) {
        unlink(entry);
        linkNewest(entry);
    }
}

// SIEVE's choice (Zhang et al., NSDI 2024). The hand walks from the oldest
// key towards the newest, wrapping round, and clears every visited bit it
// passes; the first key whose bit is already clear is evicted. The keys it
// passes keep their places (CLOCK would move them to the front), so new keys
// that aren't used again leave soon, and popular ones settle near the oldest
// end. Each step clears a bit that a hit set, so a walk is short on average.
// The hand is left on the victim; eraseEntry() moves it on.
InMemoryStorage::CacheEntry* InMemoryStorage::sieveVictim(){
    CacheEntry* obj = hand_ ? hand_ : oldest_;
    while(obj->visited) {
        obj->visited = false;
        obj = obj->newer ? obj->newer : oldest_;
    }
    hand_ = obj;
    return obj;
}

// Evictions and expirations are removals the server decides on its own, so
// each is logged as a DEL, as Redis does. Replay can't repeat these decisions:
// GETs aren't logged, so its eviction order differs, and it expires nothing
// until loading is done (see setLoading). Without the DELs, a restart could
// bring back keys the server had dropped and lose others it still had.

void InMemoryStorage::evictOne(){
    CacheEntry* victim = policy_ == EvictionPolicy::SIEVE ? sieveVictim() : oldest_;
    std::string key = *victim->key;    // a copy: erasing removes the original
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
    // Expired keys go before any live key, wherever they are in the queue.
    // Only as many as are needed: each insert does a bounded amount of work.
    ExpiryTime now = steadyNowMs();
    while(data_.size() >= capacity_ && !expiry_index_.empty() &&
          expiry_index_.begin()->first <= now) {
        std::string key = *expiry_index_.begin()->second;   // a copy, as above
        expireKey(key);
    }
    while(data_.size() >= capacity_) evictOne();
}

void InMemoryStorage::setLoading(bool loading){
    std::unique_lock lock(mutex_);
    loading_ = loading;
    // The AOF may hold more keys than the capacity allows now (a smaller
    // --maxkeys than when it was written). Evicting during replay could drop a
    // key and then rebuild it from its later records alone, a wrong value
    // rather than a missing one, so evict only once everything is loaded.
    if(!loading_) {
        while(data_.size() > capacity_) evictOne();
    }
}

size_t InMemoryStorage::bucketCount() const{
    std::shared_lock lock(mutex_);
    return data_.bucket_count();
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
    // Every removal comes through here (eviction, DEL, expiry, an emptied
    // list), so this is the one place SIEVE's hand needs fixing: off the
    // removed key, to its neighbour towards the newest, or back to the start
    // (the oldest) if it was the newest. After an eviction that is exactly
    // where the paper leaves the hand.
    CacheEntry& entry = it->second;
    if(&entry == hand_) hand_ = entry.newer;    // nullptr if it was the newest
    unlink(entry);
    data_.erase(it);
}