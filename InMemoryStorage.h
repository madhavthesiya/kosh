#pragma once
#include "StorageInterface.h"
#include "AOFLogger.h"
#include "KeyTable.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <string_view>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <deque>
#include <optional>
#include <variant>
#include <shared_mutex>

using ListValue = std::deque<std::string>;                      // O(1) push and pop at both ends
using SetValue = std::unordered_set<std::string>;
using HashValue = std::unordered_map<std::string, std::string>;

// A key's value, as a type-safe tagged union. Strings, the common case, are
// stored inline; lists, sets and hashes behind a pointer. A variant is as
// large as its largest alternative, and a std::deque is 80 bytes, so storing
// them inline made every key, strings included, pay 88 bytes for its value;
// boxed, it is 40.
using ValueType = std::variant<
    std::string,
    std::unique_ptr<ListValue>,
    std::unique_ptr<SetValue>,
    std::unique_ptr<HashValue>
>;

enum class DataType { STRING, LIST, SET, HASH };

// How a full engine picks the key to remove (--eviction)
enum class EvictionPolicy {
    LRU,    // the least recently used key: a hit moves the key to the front of the queue
    SIEVE,  // SIEVE (Zhang et al., NSDI 2024): a hit only sets a bit; see sieveVictim()
};

// When a key expires. In memory it is a time on the monotonic clock, in
// milliseconds, so a change to the wall clock while the server runs can't make
// keys expire early or late. The AOF records it as Unix time in milliseconds,
// which still means the same after a restart; the monotonic clock doesn't.
using ExpiryTime = std::chrono::time_point<std::chrono::steady_clock, std::chrono::milliseconds>;

// final: nothing derives from it, so a call through an InMemoryStorage& (as
// every command makes) is a direct call instead of one through the vtable.
// It still can't be inlined, as the methods are in InMemoryStorage.cpp.
class InMemoryStorage final : public StorageInterface {
    public:
        // Throws std::invalid_argument if capacity is 0
        explicit InMemoryStorage(size_t capacity=100, AOFLogger* aof=nullptr,
                                 EvictionPolicy policy=EvictionPolicy::LRU);

        // String operations
        Result set(const std::string& key, const std::string& value) override;
        Result set(const std::string& key, const std::string& value, int ttlSeconds) override;
        Result get(const std::string& key) override;

        // GET without copying the value: calls use(value) with the stored string
        // while holding the lock (so `use` must not call back into this object),
        // and marks the key used, as get() does. The server writes the reply
        // straight from the stored value into the client's output buffer.
        template<typename Use>
        Result readString(const std::string& key, Use&& use);

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

        // Buckets in the key table: none until the first key, then growing
        // with the keys (tests check it)
        size_t bucketCount() const;

        ~InMemoryStorage() override;
        InMemoryStorage(const InMemoryStorage&) = delete;
        InMemoryStorage& operator=(const InMemoryStorage&) = delete;

    private:
        // bench/memory_layout.cpp prints sizeof(CacheEntry), so it needs access.
        friend struct MemoryLayoutReport;

        // One key: a single allocation, this struct followed by the key's
        // bytes (see newEntry). The eviction queue runs through the entries
        // themselves (an intrusive list), newest at the front. Entries never
        // move, so the queue's links and the expiry index's pointers stay valid.
        struct CacheEntry {
            ValueType value;        // std::variant — holds String, List, Set, or Hash
            ExpiryTime expiry_time;
            CacheEntry* newer = nullptr;    // towards the front of the queue
            CacheEntry* older = nullptr;    // towards the back
            uint32_t key_len = 0;
            bool has_expiry = false;
            bool visited = false;           // SIEVE: used since the hand last passed it
            std::string_view key() const { return {reinterpret_cast<const char*>(this + 1), key_len}; }
        };
        struct EntryKey {
            static std::string_view key(const CacheEntry& e) { return e.key(); }
        };
        using DataMap = KeyTable<CacheEntry, EntryKey>;

        // The entry and its key's bytes in one block, and its release
        static CacheEntry* newEntry(std::string_view key);
        static void deleteEntry(CacheEntry* entry);

        // Adds a key that isn't in data_ yet, with this value, and links it
        // into the queue: at the front (the newest), or at the back when
        // `newest` is false
        CacheEntry* insertEntry(std::string_view key, ValueType value, bool newest = true);

        // The queue's links
        void linkNewest(CacheEntry& entry);
        void linkOldest(CacheEntry& entry);
        void unlink(CacheEntry& entry);

        // Every change to an entry's expiry and every removal goes through
        // these, so the expiry index stays in step with data_
        void setExpiry(CacheEntry* entry, std::optional<ExpiryTime> expiry);
        void eraseEntry(CacheEntry* entry);         // and frees it

        // Eviction helpers
        void markUsed(CacheEntry& entry);               // a hit, for the eviction policy
        void evictOne();                                // removes and logs the policy's choice
        CacheEntry* sieveVictim();                      // SIEVE's choice; leaves the hand on it
        void makeRoomForNewKey();                       // expired keys first, then evictOne()

        // Expiry helpers
        bool isExpired(const CacheEntry& entry) const;
        void expireEntry(CacheEntry* entry);            // removes an expired key, logs DEL
        void freeAllEntries();                          // empties the table and the queue

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
        CacheEntry* newest_ = nullptr;      // the front of the queue
        CacheEntry* oldest_ = nullptr;      // the back
        EvictionPolicy policy_;
        // SIEVE's hand: the next entry it looks at, or nullptr to start from
        // the oldest. Never points at a removed key (see eraseEntry).
        CacheEntry* hand_ = nullptr;

        // Keys with a TTL, soonest expiry first, so expired keys are found
        // without a scan when room is needed. Entries never move, so the
        // pointers stay valid until the entry is erased (eraseEntry takes it
        // out of the index first).
        using ExpiryItem = std::pair<ExpiryTime, const CacheEntry*>;
        struct ExpiryOrder {
            bool operator()(const ExpiryItem& a, const ExpiryItem& b) const {
                if(a.first != b.first) return a.first < b.first;
                // `<` on pointers into different objects is unspecified;
                // std::less is guaranteed to be a total order
                return std::less<const CacheEntry*>()(a.second, b.second);
            }
        };
        std::set<ExpiryItem, ExpiryOrder> expiry_index_;

        size_t capacity_;
        mutable std::shared_mutex mutex_;
        AOFLogger* aof_ = nullptr;          // optional — nullptr means no AOF
        bool loading_ = false;              // replaying the AOF (see setLoading)
};

template<typename Use>
Result InMemoryStorage::readString(const std::string& key, Use&& use) {
    std::unique_lock lock(mutex_);      // unique: an expired key is removed, and the hit is recorded
    CacheEntry* e = data_.find(key);
    if(!e) return Result::fail(ErrorCode::KEY_NOT_FOUND);
    if(isExpired(*e)) {
        expireEntry(e);
        return Result::fail(ErrorCode::KEY_NOT_FOUND);
    }
    if(getDataType(*e) != DataType::STRING) return Result::fail(ErrorCode::WRONG_TYPE);
    markUsed(*e);
    use(std::get<std::string>(e->value));
    return Result::ok();
}