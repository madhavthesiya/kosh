#pragma once
#include "InMemoryStorage.h"

class RedisLite {
    public:
        RedisLite(InMemoryStorage& storage) : storage_(storage) {}
        
        // String operations
        Result set(const std::string& key, const std::string& value);
        Result setex(const std::string& key, int ttl, const std::string& value);
        Result get(const std::string& key);

        // Key operations
        Result del(const std::string& key);
        Result exists(const std::string& key);
        Result type(const std::string& key);
        Result expire(const std::string& key, int ttl);
        Result ttl(const std::string& key);
        
        // List operations
        Result lpush(const std::string& key, const std::vector<std::string>& values);
        Result rpush(const std::string& key, const std::vector<std::string>& values);
        Result lpop(const std::string& key);
        Result rpop(const std::string& key);
        Result lrange(const std::string& key, int start, int stop);
        Result llen(const std::string& key);

        // Set operations
        Result sadd(const std::string& key, const std::vector<std::string>& members);
        Result srem(const std::string& key, const std::vector<std::string>& members);
        Result smembers(const std::string& key);
        Result sismember(const std::string& key, const std::string& member);
        Result scard(const std::string& key);

        // Hash operations
        Result hset(const std::string& key, const std::vector<std::pair<std::string,std::string>>& fields);
        Result hget(const std::string& key, const std::string& field);
        Result hdel(const std::string& key, const std::vector<std::string>& fields);
        Result hgetall(const std::string& key);
        Result hlen(const std::string& key);

        // Persistence
        Result save(const std::string& filename);
        Result load(const std::string& filename);

    private:
        InMemoryStorage& storage_;
        Result validateKey(const std::string& key);
};