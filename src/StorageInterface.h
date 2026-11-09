#pragma once
#include <string>
#include <vector>
#include "Result.h"

class StorageInterface {
    public:
    // String operations
    virtual Result set(const std::string& key, const std::string& value)=0;
    virtual Result set(const std::string& key, const std::string& value, int ttlSeconds)=0;
    virtual Result get(const std::string& key)=0;

    // Key operations
    virtual Result del(const std::string& key)=0;
    virtual Result exists(const std::string& key)=0;
    virtual Result type(const std::string& key)=0;
    virtual Result expire(const std::string& key, int ttlSeconds)=0;
    virtual Result ttl(const std::string& key)=0;

    // List operations
    virtual Result lpush(const std::string& key, const std::vector<std::string>& values)=0;
    virtual Result rpush(const std::string& key, const std::vector<std::string>& values)=0;
    virtual Result lpop(const std::string& key)=0;
    virtual Result rpop(const std::string& key)=0;
    virtual Result lrange(const std::string& key, int start, int stop)=0;
    virtual Result llen(const std::string& key)=0;

    // Set operations
    virtual Result sadd(const std::string& key, const std::vector<std::string>& members)=0;
    virtual Result srem(const std::string& key, const std::vector<std::string>& members)=0;
    virtual Result smembers(const std::string& key)=0;
    virtual Result sismember(const std::string& key, const std::string& member)=0;
    virtual Result scard(const std::string& key)=0;

    // Hash operations
    virtual Result hset(const std::string& key, const std::vector<std::pair<std::string,std::string>>& fields)=0;
    virtual Result hget(const std::string& key, const std::string& field)=0;
    virtual Result hdel(const std::string& key, const std::vector<std::string>& fields)=0;
    virtual Result hgetall(const std::string& key)=0;
    virtual Result hlen(const std::string& key)=0;

    // Persistence
    virtual Result saveToFile(const std::string& filename)=0;
    virtual Result loadFromFile(const std::string& filename)=0;

    virtual ~StorageInterface() = default;
};