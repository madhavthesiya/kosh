#include "InMemoryStorage.h"
#include "RespParser.h"
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <sstream>

InMemoryStorage::InMemoryStorage(size_t capacity, AOFLogger* aof){
    capacity_=capacity;
    aof_=aof;
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
        removeKey(key);
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
            removeKey(key);
        } else if(getDataType(it->second) != expected) {
            return nullptr;     // type mismatch
        } else {
            touchLRU(key);
            return &it->second; // existing entry of correct type
        }
    }
    // Create new entry
    if(data_.size() == capacity_) {
        evictLRU();
    }
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
    data_[key] = std::move(entry);
    return &data_[key];
}

// ==================== Key Operations ====================

Result InMemoryStorage::exists(const std::string& key){
    std::unique_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(it->second)) {
        removeKey(key);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    return Result::ok();
}

Result InMemoryStorage::del(const std::string& key){
    std::unique_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    // Expired keys are treated as non-existent — don't log DEL for them
    if(isExpired(it->second)) {
        removeKey(key);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    lru_list_.erase(it->second.lru_it);
    data_.erase(it);
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
    std::unique_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(it->second)) {
        removeKey(key);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    it->second.has_expiry = true;
    it->second.expiry_time = std::chrono::steady_clock::now() + std::chrono::seconds(ttlSeconds);
    logAOF({"EXPIRE", key, std::to_string(ttlSeconds)});
    return Result::ok();
}

Result InMemoryStorage::ttl(const std::string& key){
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(it->second)) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(!it->second.has_expiry) return Result::ok("-1");
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        it->second.expiry_time - std::chrono::steady_clock::now()
    ).count();
    return Result::ok(std::to_string(remaining));
}

// ==================== String Operations ====================

Result InMemoryStorage::get(const std::string& key){
    std::unique_lock lock(mutex_);
    auto it = data_.find(key);
    if(it == data_.end()) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(it->second)) {
        removeKey(key);
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
    auto r = setInternal(key, value, 0);
    if(r.success) logAOF({"SET", key, value});
    return r;
}

Result InMemoryStorage::set(const std::string& key, const std::string& value, int ttlSeconds){
    std::unique_lock lock(mutex_);
    auto r = setInternal(key, value, ttlSeconds);
    if(r.success) logAOF({"SETEX", key, std::to_string(ttlSeconds), value});
    return r;
}

// SET always overwrites regardless of existing type (same as real Redis)
Result InMemoryStorage::setInternal(const std::string& key, const std::string& value, int ttlSeconds){
    auto it = data_.find(key);
    if(it != data_.end()) {
        if(isExpired(it->second)) {
            removeKey(key);
        } else {
            // Overwrite: replace value with string regardless of current type
            it->second.value = value;
            if(ttlSeconds > 0) {
                it->second.has_expiry = true;
                it->second.expiry_time = std::chrono::steady_clock::now() + std::chrono::seconds(ttlSeconds);
            } else {
                it->second.has_expiry = false;
            }
            touchLRU(key);
            return Result::ok();
        }
    }
    // Sweep expired keys before checking capacity
    while(!lru_list_.empty()) {
        auto backKey = lru_list_.back();
        auto bit = data_.find(backKey);
        if(bit != data_.end() && isExpired(bit->second)) {
            removeKey(backKey);
        } else {
            break;
        }
    }
    if(data_.size() == capacity_) {
        evictLRU();
    }
    lru_list_.push_front(key);
    CacheEntry entry;
    entry.value = value;    // stores as std::string in the variant
    if(ttlSeconds > 0) {
        entry.has_expiry = true;
        entry.expiry_time = std::chrono::steady_clock::now() + std::chrono::seconds(ttlSeconds);
    } else {
        entry.has_expiry = false;
    }
    entry.lru_it = lru_list_.begin();
    data_[key] = std::move(entry);
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
                commands.push_back(RespParser::array({"SET", key, std::get<std::string>(entry.value)}));
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
        // Preserve TTL: emit EXPIRE for keys with remaining TTL
        if(entry.has_expiry) {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                entry.expiry_time - std::chrono::steady_clock::now()
            ).count();
            if(remaining > 0) {
                commands.push_back(RespParser::array({"EXPIRE", key, std::to_string(remaining)}));
            }
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

void InMemoryStorage::evictLRU(){
    std::string key = lru_list_.back();
    lru_list_.pop_back();
    data_.erase(key);
}

bool InMemoryStorage::isExpired(const CacheEntry& entry) const{
    if(!entry.has_expiry) return false;   
    return std::chrono::steady_clock::now() >= entry.expiry_time;
}

void InMemoryStorage::removeKey(const std::string& key){
    auto it = data_.find(key);
    if(it != data_.end()) {
        lru_list_.erase(it->second.lru_it);
        data_.erase(it);
    }
}