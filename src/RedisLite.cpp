#include "RedisLite.h"

Result RedisLite::validateKey(const std::string& key) {
    if(key.empty()) return Result::fail(ErrorCode::INVALID_KEY);
    return Result::ok();
}

// ==================== String Operations ====================

Result RedisLite::set(const std::string& key, const std::string& value) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.set(key, value);
}

Result RedisLite::setex(const std::string& key, int ttl, const std::string& value) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.set(key, value, ttl);
}

Result RedisLite::get(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.get(key);
}

// ==================== Key Operations ====================

Result RedisLite::del(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.del(key);
}

Result RedisLite::exists(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.exists(key);
}

Result RedisLite::type(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.type(key);
}

Result RedisLite::expire(const std::string& key, int ttl) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.expire(key, ttl);
}

Result RedisLite::ttl(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.ttl(key);
}

// ==================== List Operations ====================

Result RedisLite::lpush(const std::string& key, const std::vector<std::string>& values) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.lpush(key, values);
}

Result RedisLite::rpush(const std::string& key, const std::vector<std::string>& values) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.rpush(key, values);
}

Result RedisLite::lpop(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.lpop(key);
}

Result RedisLite::rpop(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.rpop(key);
}

Result RedisLite::lrange(const std::string& key, int start, int stop) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.lrange(key, start, stop);
}

Result RedisLite::llen(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.llen(key);
}

// ==================== Set Operations ====================

Result RedisLite::sadd(const std::string& key, const std::vector<std::string>& members) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.sadd(key, members);
}

Result RedisLite::srem(const std::string& key, const std::vector<std::string>& members) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.srem(key, members);
}

Result RedisLite::smembers(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.smembers(key);
}

Result RedisLite::sismember(const std::string& key, const std::string& member) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.sismember(key, member);
}

Result RedisLite::scard(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.scard(key);
}

// ==================== Hash Operations ====================

Result RedisLite::hset(const std::string& key, const std::vector<std::pair<std::string,std::string>>& fields) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.hset(key, fields);
}

Result RedisLite::hget(const std::string& key, const std::string& field) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.hget(key, field);
}

Result RedisLite::hdel(const std::string& key, const std::vector<std::string>& fields) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.hdel(key, fields);
}

Result RedisLite::hgetall(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.hgetall(key);
}

Result RedisLite::hlen(const std::string& key) {
    auto v = validateKey(key); if(!v.success) return v;
    return storage_.hlen(key);
}

// ==================== Persistence ====================

Result RedisLite::save(const std::string& filename) {
    return storage_.saveToFile(filename);
}

Result RedisLite::load(const std::string& filename) {
    return storage_.loadFromFile(filename);
}