#include "InMemoryStorage.h"
#include "RespParser.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <new>
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

// The list, set or hash in a value already known to hold one (see ValueType)
template<typename T>
T& boxed(ValueType& value) { return *std::get<std::unique_ptr<T>>(value); }
template<typename T>
const T& boxed(const ValueType& value) { return *std::get<std::unique_ptr<T>>(value); }

}  // namespace

int64_t InMemoryStorage::unixTimeMs() {
    // system_clock counts from the Unix epoch (required from C++20, and already
    // true of every C++17 standard library)
    return std::chrono::duration_cast<milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

InMemoryStorage::InMemoryStorage(size_t capacity, AOFLogger* aof, EvictionPolicy policy)
    : data_(KeyedHash{SipKey::random()}), policy_(policy), capacity_(capacity), aof_(aof) {
    // Eviction makes room by removing a key, so at least one must fit. With
    // capacity 0, the first SET would evict from an empty LRU list.
    if(capacity_ == 0) throw std::invalid_argument("InMemoryStorage capacity must be at least 1");
    // Nothing is sized up front: KeyTable grows a bucket at a time, so it
    // never stalls the way std::unordered_map's all-at-once rehash did (about
    // 20 ms at 85,230 keys, bench/insert_latency), and memory follows the
    // keys actually stored, not --maxkeys.
}

InMemoryStorage::~InMemoryStorage() {
    freeAllEntries();
}

// One block: the entry, then the key's bytes, so a key costs one allocation
// (std::unordered_map took two for a 16-byte key: its node, and a heap copy
// of the key, one byte too long for std::string's inline buffer).
InMemoryStorage::CacheEntry* InMemoryStorage::newEntry(std::string_view key){
    if(key.size() > UINT32_MAX) throw std::length_error("key too long");
    void* block = ::operator new(sizeof(CacheEntry) + key.size());
    auto* entry = new (block) CacheEntry{};
    entry->key_len = static_cast<uint32_t>(key.size());
    // The key's bytes go into the raw storage after the entry, not into it
    std::memcpy(static_cast<char*>(block) + sizeof(CacheEntry), key.data(), key.size());
    return entry;
}

void InMemoryStorage::deleteEntry(CacheEntry* entry){
    entry->~CacheEntry();
    ::operator delete(entry);
}

// Every entry is in the queue, so walking it frees them all
void InMemoryStorage::freeAllEntries(){
    for(CacheEntry* e = newest_; e; ) {
        CacheEntry* older = e->older;
        deleteEntry(e);
        e = older;
    }
    newest_ = oldest_ = hand_ = nullptr;
    data_.clear();
    expiry_index_.clear();
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
    if(std::holds_alternative<std::unique_ptr<ListValue>>(entry.value)) return DataType::LIST;
    if(std::holds_alternative<std::unique_ptr<SetValue>>(entry.value)) return DataType::SET;
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
    CacheEntry* e = data_.find(key);
    if(!e) return nullptr;
    if(isExpired(*e)) {
        expireEntry(e);
        return nullptr;
    }
    if(getDataType(*e) != expected) return nullptr;
    return e;
}

// Gets or creates an entry of the expected type.
// If key exists with a different type, returns nullptr (type mismatch).
// If key exists but is expired, removes it and creates a new one.
// Caller must hold unique_lock.
InMemoryStorage::CacheEntry* InMemoryStorage::getOrCreateEntry(const std::string& key, DataType expected) {
    if(CacheEntry* e = data_.find(key)) {
        if(isExpired(*e)) {
            expireEntry(e);
        } else if(getDataType(*e) != expected) {
            return nullptr;     // type mismatch
        } else {
            markUsed(*e);
            return e;           // existing entry of correct type
        }
    }
    // Create new entry
    makeRoomForNewKey();
    ValueType value;
    switch(expected) {
        case DataType::STRING: value = std::string(); break;
        case DataType::LIST:   value = std::make_unique<ListValue>(); break;
        case DataType::SET:    value = std::make_unique<SetValue>(); break;
        case DataType::HASH:   value = std::make_unique<HashValue>(); break;
    }
    return insertEntry(key, std::move(value));
}

// ==================== Key Operations ====================

Result InMemoryStorage::exists(const std::string& key){
    std::unique_lock lock(mutex_);
    CacheEntry* e = data_.find(key);
    if(!e) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(*e)) {
        expireEntry(e);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    return Result::ok();
}

Result InMemoryStorage::del(const std::string& key){
    std::unique_lock lock(mutex_);
    CacheEntry* e = data_.find(key);
    if(!e) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    // An expired key no longer exists: DEL returns 0, as in Redis. Its removal
    // is still logged, as an expiration.
    if(isExpired(*e)) {
        expireEntry(e);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    eraseEntry(e);
    logAOF({"DEL", key});
    return Result::ok();
}

Result InMemoryStorage::type(const std::string& key){
    std::shared_lock lock(mutex_);
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::ok("none");
    return Result::ok(dataTypeToString(getDataType(*e)));
}

Result InMemoryStorage::expire(const std::string& key, int ttlSeconds){
    return expireAt(key, addSaturating(unixTimeMs(), ttlSeconds * int64_t(1000)));
}

// Logged as PEXPIREAT with the absolute time, so replay expires the key at the
// same moment, however much later the restart is
Result InMemoryStorage::expireAt(const std::string& key, int64_t unixTimeMs){
    std::unique_lock lock(mutex_);
    CacheEntry* e = data_.find(key);
    if(!e) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(*e)) {
        expireEntry(e);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    setExpiry(e, toExpiryTime(unixTimeMs));
    logAOF({"PEXPIREAT", key, std::to_string(unixTimeMs)});
    return Result::ok();
}

Result InMemoryStorage::ttl(const std::string& key){
    std::shared_lock lock(mutex_);
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(!e->has_expiry) return Result::ok("-1");
    int64_t remaining_ms = addSaturating(e->expiry_time.time_since_epoch().count(),
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
    if(CacheEntry* e = data_.find(key)) {
        if(isExpired(*e)) {
            expireEntry(e);
        } else {
            // Overwrite: replace value with string regardless of current type
            e->value = value;
            setExpiry(e, expiry);
            markUsed(*e);
            return Result::ok();
        }
    }
    makeRoomForNewKey();
    setExpiry(insertEntry(key, ValueType(value)), expiry);
    return Result::ok();
}

// ==================== List Operations ====================

Result InMemoryStorage::lpush(const std::string& key, const std::vector<std::string>& values){
    std::unique_lock lock(mutex_);
    auto* entry = getOrCreateEntry(key, DataType::LIST);
    if(!entry) return Result::fail(ErrorCode::WRONG_TYPE);
    auto& list = boxed<ListValue>(entry->value);
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
    auto& list = boxed<ListValue>(entry->value);
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
        const CacheEntry* e = data_.find(key);
        if(e && !isExpired(*e))
            return Result::fail(ErrorCode::WRONG_TYPE);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    auto& list = boxed<ListValue>(entry->value);
    if(list.empty()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    std::string val = list.front();
    list.pop_front();
    // Auto-delete empty lists (same as real Redis)
    if(list.empty()) eraseEntry(entry);
    else markUsed(*entry);
    logAOF({"LPOP", key});
    return Result::ok(val);
}

Result InMemoryStorage::rpop(const std::string& key){
    std::unique_lock lock(mutex_);
    auto* entry = getValidEntry(key, DataType::LIST);
    if(!entry) {
        const CacheEntry* e = data_.find(key);
        if(e && !isExpired(*e))
            return Result::fail(ErrorCode::WRONG_TYPE);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    auto& list = boxed<ListValue>(entry->value);
    if(list.empty()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    std::string val = list.back();
    list.pop_back();
    if(list.empty()) eraseEntry(entry);
    else markUsed(*entry);
    logAOF({"RPOP", key});
    return Result::ok(val);
}

Result InMemoryStorage::lrange(const std::string& key, int start, int stop){
    std::shared_lock lock(mutex_);
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::ok(std::vector<std::string>{});
    if(getDataType(*e) != DataType::LIST) return Result::fail(ErrorCode::WRONG_TYPE);
    
    const auto& list = boxed<ListValue>(e->value);
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
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::ok(0);
    if(getDataType(*e) != DataType::LIST) return Result::fail(ErrorCode::WRONG_TYPE);
    return Result::ok(static_cast<int>(boxed<ListValue>(e->value).size()));
}

// ==================== Set Operations ====================

Result InMemoryStorage::sadd(const std::string& key, const std::vector<std::string>& members){
    std::unique_lock lock(mutex_);
    auto* entry = getOrCreateEntry(key, DataType::SET);
    if(!entry) return Result::fail(ErrorCode::WRONG_TYPE);
    auto& set = boxed<SetValue>(entry->value);
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
        const CacheEntry* e = data_.find(key);
        if(e && !isExpired(*e))
            return Result::fail(ErrorCode::WRONG_TYPE);
        return Result::ok(0);
    }
    auto& set = boxed<SetValue>(entry->value);
    int removed = 0;
    for(const auto& m : members) {
        removed += static_cast<int>(set.erase(m));
    }
    if(set.empty()) eraseEntry(entry);
    else markUsed(*entry);
    logAOF({"SREM", key}, members);
    return Result::ok(removed);
}

Result InMemoryStorage::smembers(const std::string& key){
    std::shared_lock lock(mutex_);
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::ok(std::vector<std::string>{});
    if(getDataType(*e) != DataType::SET) return Result::fail(ErrorCode::WRONG_TYPE);
    const auto& set = boxed<SetValue>(e->value);
    return Result::ok(std::vector<std::string>(set.begin(), set.end()));
}

Result InMemoryStorage::sismember(const std::string& key, const std::string& member){
    std::shared_lock lock(mutex_);
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::ok(0);
    if(getDataType(*e) != DataType::SET) return Result::fail(ErrorCode::WRONG_TYPE);
    const auto& set = boxed<SetValue>(e->value);
    return Result::ok(set.count(member) > 0 ? 1 : 0);
}

Result InMemoryStorage::scard(const std::string& key){
    std::shared_lock lock(mutex_);
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::ok(0);
    if(getDataType(*e) != DataType::SET) return Result::fail(ErrorCode::WRONG_TYPE);
    return Result::ok(static_cast<int>(boxed<SetValue>(e->value).size()));
}

// ==================== Hash Operations ====================

Result InMemoryStorage::hset(const std::string& key, const std::vector<std::pair<std::string,std::string>>& fields){
    std::unique_lock lock(mutex_);
    auto* entry = getOrCreateEntry(key, DataType::HASH);
    if(!entry) return Result::fail(ErrorCode::WRONG_TYPE);
    auto& hash = boxed<HashValue>(entry->value);
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
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(getDataType(*e) != DataType::HASH) return Result::fail(ErrorCode::WRONG_TYPE);
    const auto& hash = boxed<HashValue>(e->value);
    auto fit = hash.find(field);
    if(fit == hash.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    return Result::ok(fit->second);
}

Result InMemoryStorage::hdel(const std::string& key, const std::vector<std::string>& fields){
    std::unique_lock lock(mutex_);
    auto* entry = getValidEntry(key, DataType::HASH);
    if(!entry) {
        const CacheEntry* e = data_.find(key);
        if(e && !isExpired(*e))
            return Result::fail(ErrorCode::WRONG_TYPE);
        return Result::ok(0);
    }
    auto& hash = boxed<HashValue>(entry->value);
    int removed = 0;
    for(const auto& f : fields) {
        removed += static_cast<int>(hash.erase(f));
    }
    if(hash.empty()) eraseEntry(entry);
    else markUsed(*entry);
    logAOF({"HDEL", key}, fields);
    return Result::ok(removed);
}

Result InMemoryStorage::hgetall(const std::string& key){
    std::shared_lock lock(mutex_);
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::ok(std::vector<std::string>{});
    if(getDataType(*e) != DataType::HASH) return Result::fail(ErrorCode::WRONG_TYPE);
    const auto& hash = boxed<HashValue>(e->value);
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
    const CacheEntry* e = data_.find(key);
    if(!e || isExpired(*e)) return Result::ok(0);
    if(getDataType(*e) != DataType::HASH) return Result::fail(ErrorCode::WRONG_TYPE);
    return Result::ok(static_cast<int>(boxed<HashValue>(e->value).size()));
}

// ==================== Persistence ====================

Result InMemoryStorage::saveToFile(const std::string& filename){
    std::shared_lock lock(mutex_);
    std::ofstream out(filename);
    if(!out.is_open()) return Result::fail(ErrorCode::FILE_ERROR);
    for(const CacheEntry* e = oldest_; e; e = e->newer) {
        if(isExpired(*e)) continue;
        if(getDataType(*e) == DataType::STRING) {
            out << "S|" << e->key() << "=" << std::get<std::string>(e->value) << "\n";
        }
    }
    out.close();
    return Result::ok();
}

Result InMemoryStorage::loadFromFile(const std::string& filename){
    std::unique_lock lock(mutex_);
    std::ifstream in(filename);
    if(!in.is_open()) return Result::fail(ErrorCode::FILE_ERROR);
    freeAllEntries();

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
        if(CacheEntry* e = data_.find(key)) {
            e->value = value;
        } else {
            insertEntry(key, ValueType(value), /*newest=*/false);   // file order: first line newest
        }
    }
    in.close();
    while(data_.size() > capacity_) evictOne();
    return Result::ok();
}

// ==================== AOF Operations ====================

// Serialize current in-memory state as the minimum set of RESP-formatted commands
// needed to reproduce it. Used by AOFREWRITE to compact the AOF file. Written
// oldest key first, so loading the file rebuilds the same eviction order.
std::vector<std::string> InMemoryStorage::dumpState() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> commands;
    for(const CacheEntry* e = oldest_; e; e = e->newer) {
        const CacheEntry& entry = *e;
        const std::string key(entry.key());
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
                const auto& list = boxed<ListValue>(entry.value);
                if(!list.empty()) {
                    std::vector<std::string> parts = {"RPUSH", key};
                    parts.insert(parts.end(), list.begin(), list.end());
                    commands.push_back(RespParser::array(parts));
                }
                break;
            }
            case DataType::SET: {
                const auto& s = boxed<SetValue>(entry.value);
                if(!s.empty()) {
                    std::vector<std::string> parts = {"SADD", key};
                    parts.insert(parts.end(), s.begin(), s.end());
                    commands.push_back(RespParser::array(parts));
                }
                break;
            }
            case DataType::HASH: {
                const auto& hash = boxed<HashValue>(entry.value);
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

// Allocating the entry and adding it to data_ are the only steps that can
// throw (out of memory). If adding throws, the unique_ptr frees the entry;
// linking can't throw. So a failed insert leaves everything as it was.
InMemoryStorage::CacheEntry* InMemoryStorage::insertEntry(std::string_view key, ValueType value,
                                                          bool newest){
    assert(!data_.find(key) && "insertEntry: the key is already stored");
    auto release = [](CacheEntry* e) { deleteEntry(e); };
    std::unique_ptr<CacheEntry, decltype(release)> entry(newEntry(key), release);
    entry->value = std::move(value);
    data_.insert(entry.get());
    CacheEntry* e = entry.release();
    if(newest) linkNewest(*e);
    else linkOldest(*e);
    return e;
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
    std::string key(victim->key());    // a copy: erasing frees the original
    eraseEntry(victim);
    logAOF({"DEL", key});
}

void InMemoryStorage::expireEntry(CacheEntry* entry){
    std::string key(entry->key());     // a copy, as above
    eraseEntry(entry);
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
        expireEntry(const_cast<CacheEntry*>(expiry_index_.begin()->second));
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
    return data_.bucketCount();
}

bool InMemoryStorage::isExpired(const CacheEntry& entry) const{
    if(!entry.has_expiry || loading_) return false;   // see setLoading()
    return steadyNowMs() >= entry.expiry_time;
}

void InMemoryStorage::setExpiry(CacheEntry* entry, std::optional<ExpiryTime> expiry){
    if(entry->has_expiry) expiry_index_.erase({entry->expiry_time, entry});
    entry->has_expiry = expiry.has_value();
    if(expiry) {
        entry->expiry_time = *expiry;
        expiry_index_.insert({*expiry, entry});
    }
}

void InMemoryStorage::eraseEntry(CacheEntry* entry){
    // Out of the index first: it points at the entry this frees
    if(entry->has_expiry) expiry_index_.erase({entry->expiry_time, entry});
    // Every removal comes through here (eviction, DEL, expiry, an emptied
    // list), so this is the one place SIEVE's hand needs fixing: off the
    // removed key, to its neighbour towards the newest, or back to the start
    // (the oldest) if it was the newest. After an eviction that is exactly
    // where the paper leaves the hand.
    if(entry == hand_) hand_ = entry->newer;    // nullptr if it was the newest
    unlink(*entry);
    data_.erase(entry);     // finds it by its key, so before the key is freed
    deleteEntry(entry);
}