#pragma once
#include "StorageInterface.h"
#include "AOFLogger.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <string_view>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <list>
#include <set>
#include <deque>
#include <optional>
#include <variant>
#include <shared_mutex>

// Type-safe tagged union for storing different Redis data types
// std::variant enforces compile-time type checking — no void* or std::any needed
using ValueType = std::variant<
    std::string,                                    // String
    std::deque<std::string>,                        // List (deque for O(1) push/pop at both ends)
    std::unordered_set<std::string>,                // Set
    std::unordered_map<std::string, std::string>    // Hash
>;

enum class DataType { STRING, LIST, SET, HASH };

// When a key expires. In memory it is a time on the monotonic clock, in
// milliseconds, so a change to the wall clock while the server runs can't make
// keys expire early or late. The AOF records it as Unix time in milliseconds,
// which still means the same after a restart; the monotonic clock doesn't.
using ExpiryTime = std::chrono::time_point<std::chrono::steady_clock, std::chrono::milliseconds>;

class InMemoryStorage : public StorageInterface {
    public:
        // Throws std::invalid_argument if capacity is 0
        explicit InMemoryStorage(size_t capacity=100, AOFLogger* aof=nullptr);

        // String operations
        Result set(const std::string& key, const std::string& value) override;
        Result set(const std::string& key, const std::string& value, int ttlSeconds) override;
        Result get(const std::string& key) override;

        // Key operations
        Result del(const std::string& key) override;
        Result exists(const std::string& key) override;
        Result type(const std::string& key) override;
        Result expire(const std::string& key, int ttlSeconds) override;
        Result ttl(const std::string& key) override;

        // List operations
        Result lpush(const std::string& key, const std::vector<std::string>& values) override;
        Result rpush(const std::string& key, const std::vector<std::string>& values) override;
        Result lpop(const std::string& key) override;
        Result rpop(const std::string& key) override;
        Result lrange(const std::string& key, int start, int stop) override;
        Result llen(const std::string& key) override;

        // Set operations
        Result sadd(const std::string& key, const std::vector<std::string>& members) override;
        Result srem(const std::string& key, const std::vector<std::string>& members) override;
        Result smembers(const std::string& key) override;
        Result sismember(const std::string& key, const std::string& member) override;
        Result scard(const std::string& key) override;

        // Hash operations
        Result hset(const std::string& key, const std::vector<std::pair<std::string,std::string>>& fields) override;
        Result hget(const std::string& key, const std::string& field) override;
        Result hdel(const std::string& key, const std::vector<std::string>& fields) override;
        Result hgetall(const std::string& key) override;
        Result hlen(const std::string& key) override;

        // Persistence
        Result saveToFile(const std::string& filename) override;
        Result loadFromFile(const std::string& filename) override;

        // Expiry at an absolute time, given as Unix time in milliseconds (what
        // the AOF records: SET ... PXAT and PEXPIREAT)
        Result setExpiringAt(const std::string& key, const std::string& value, int64_t unixTimeMs);
        Result expireAt(const std::string& key, int64_t unixTimeMs);

        // The wall clock now, as Unix time in milliseconds
        static int64_t unixTimeMs();

        // AOF operations
        std::vector<std::string> dumpState() const;   // serialize current state as commands

        // True while the AOF is being replayed (see replayAOF() in Commands.h).
        // Then the replayed commands are not logged again, and:
        // - no key expires: a key whose TTL has passed may be given a later one
        //   by a later record, so expiry is checked once loading is done (Redis
        //   does the same);
        // - nothing is evicted: the live server's evictions are in the AOF as
        //   DELs. Once loading is done, keys beyond the capacity are evicted.
        void setLoading(bool loading);

        // Buckets in the key table. It is sized for the capacity at
        // construction, so this doesn't change as keys are added (tests check it).
        size_t bucketCount() const;

    private:
        // bench/memory_layout.cpp prints sizeof(CacheEntry), so it needs access.
        friend struct MemoryLayoutReport;

        struct CacheEntry {
            ValueType value;        // std::variant — holds String, List, Set, or Hash
            bool has_expiry;
            ExpiryTime expiry_time;
            std::list<std::string>::iterator lru_it;
        };
        using DataMap = std::unordered_map<std::string, CacheEntry>;

        // Every change to an entry's expiry and every removal goes through
        // these, so the expiry index stays in step with data_
        void setExpiry(DataMap::iterator it, std::optional<ExpiryTime> expiry);
        void eraseEntry(DataMap::iterator it);

        // LRU helpers
        void touchLRU(CacheEntry& entry);               // marks it most recently used
        void evictLRU();                                // removes and logs the LRU key
        void makeRoomForNewKey();                       // expired keys first, then LRU

        // Expiry helpers
        bool isExpired(const CacheEntry& entry) const;
        void expireKey(const std::string& key);         // removes an expired key, logs DEL
        void removeKey(const std::string& key);         // removes without logging

        // Type helpers
        DataType getDataType(const CacheEntry& entry) const;
        std::string dataTypeToString(DataType dt) const;

        // Internal set (called by every set variant); no expiry if `expiry` is empty
        Result setInternal(const std::string& key, const std::string& value,
                           std::optional<ExpiryTime> expiry);

        // Ensures key exists, is not expired, and holds the expected type
        // Returns nullptr if valid, or a Result with the appropriate error
        CacheEntry* getValidEntry(const std::string& key, DataType expected);

        // Creates a new key with the given value type if it doesn't exist
        // If key exists with wrong type, returns error. If expired, removes and creates new.
        CacheEntry* getOrCreateEntry(const std::string& key, DataType expected);

        // Log one command to the AOF as a RESP array: `parts`, or `head` followed
        // by `tail` (values, or field/value pairs). Built straight into the AOF's
        // buffer, and not at all without an AOF or while loading.
        void logAOF(std::initializer_list<std::string_view> parts);
        void logAOF(std::initializer_list<std::string_view> head,
                    const std::vector<std::string>& tail);
        void logAOF(std::initializer_list<std::string_view> head,
                    const std::vector<std::pair<std::string, std::string>>& pairs);

        DataMap data_;
        std::list<std::string> lru_list_;

        // Keys with a TTL, soonest expiry first, so expired keys are found
        // without a scan when room is needed. The pointer is to the key inside
        // data_: unordered_map keeps references to its elements valid until they
        // are erased, even when it rehashes.
        using ExpiryItem = std::pair<ExpiryTime, const std::string*>;
        struct ExpiryOrder {
            bool operator()(const ExpiryItem& a, const ExpiryItem& b) const {
                if(a.first != b.first) return a.first < b.first;
                // `<` on pointers into different objects is unspecified;
                // std::less is guaranteed to be a total order
                return std::less<const std::string*>()(a.second, b.second);
            }
        };
        std::set<ExpiryItem, ExpiryOrder> expiry_index_;

        size_t capacity_;
        mutable std::shared_mutex mutex_;
        AOFLogger* aof_ = nullptr;          // optional — nullptr means no AOF
        bool loading_ = false;              // replaying the AOF (see setLoading)
};