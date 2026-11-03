#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "../Poller.h"
#include "../KeyTable.h"
#include "../InMemoryStorage.h"
#include "../RedisLite.h"
#include "../RespParser.h"
#include "../Commands.h"

int passed = 0;
int failed = 0;

#define TEST(name) void name()
#define RUN_TEST(name) \
    std::cout << "  Running " << #name << "..."; \
    try { name(); passed++; std::cout << " PASSED" << std::endl; } \
    catch(const std::exception& e) { failed++; std::cout << " FAILED: " << e.what() << std::endl; }

#define ASSERT_TRUE(expr) if(!(expr)) throw std::runtime_error("Assertion failed: " #expr)
#define ASSERT_FALSE(expr) if(expr) throw std::runtime_error("Assertion failed: NOT " #expr)
#define ASSERT_EQ(a, b) if((a) != (b)) throw std::runtime_error("Assertion failed: " #a " != " #b)

// ==================== Basic Operations ====================

TEST(test_set_and_get) {
    InMemoryStorage s(10);
    s.set("name", "madhav");
    Result r = s.get("name");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "madhav");
}

TEST(test_get_nonexistent_key) {
    InMemoryStorage s(10);
    Result r = s.get("missing");
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.error, ErrorCode::KEY_NOT_FOUND);
}

TEST(test_overwrite_existing_key) {
    InMemoryStorage s(10);
    s.set("key", "v1");
    s.set("key", "v2");
    Result r = s.get("key");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "v2");
}

TEST(test_delete_key) {
    InMemoryStorage s(10);
    s.set("key", "val");
    Result r = s.del("key");
    ASSERT_TRUE(r.success);
    r = s.get("key");
    ASSERT_FALSE(r.success);
}

TEST(test_delete_nonexistent_key) {
    InMemoryStorage s(10);
    Result r = s.del("ghost");
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.error, ErrorCode::KEY_NOT_FOUND);
}

TEST(test_exists_found) {
    InMemoryStorage s(10);
    s.set("key", "val");
    Result r = s.exists("key");
    ASSERT_TRUE(r.success);
}

TEST(test_exists_not_found) {
    InMemoryStorage s(10);
    Result r = s.exists("nope");
    ASSERT_FALSE(r.success);
}

// ==================== LRU Eviction ====================

TEST(test_lru_eviction_basic) {
    InMemoryStorage s(2);
    s.set("A", "1");
    s.set("B", "2");
    s.set("C", "3");

    Result r = s.get("A");
    ASSERT_FALSE(r.success);
    r = s.get("B");
    ASSERT_TRUE(r.success);
    r = s.get("C");
    ASSERT_TRUE(r.success);
}

TEST(test_lru_refresh_with_get) {
    InMemoryStorage s(2);
    s.set("A", "1");
    s.set("B", "2");
    s.get("A");
    s.set("C", "3");

    Result r = s.get("A");
    ASSERT_TRUE(r.success);
    r = s.get("B");
    ASSERT_FALSE(r.success);
}

TEST(test_exists_does_not_refresh_lru) {
    InMemoryStorage s(2);
    s.set("A", "1");
    s.set("B", "2");
    s.exists("A");
    s.set("C", "3");

    Result r = s.get("A");
    ASSERT_FALSE(r.success);
}

TEST(test_lru_capacity_one) {
    InMemoryStorage s(1);
    s.set("A", "1");
    s.set("B", "2");

    Result r = s.get("A");
    ASSERT_FALSE(r.success);
    r = s.get("B");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "2");
}

TEST(test_capacity_zero_is_rejected) {
    // With room for no keys, the first SET used to evict from an empty LRU list:
    // back() of an empty std::list, which is undefined behaviour
    bool threw = false;
    try {
        InMemoryStorage s(0);
    } catch(const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

TEST(test_table_sized_for_capacity) {
    // The key table is sized for the capacity up front, so filling it never
    // rehashes. A rehash moves every key at once, and the single-threaded
    // server stalls meanwhile: about 20 ms at 85,000 keys (bench/insert_latency).
    InMemoryStorage s(5000);
    size_t buckets = s.bucketCount();
    ASSERT_TRUE(buckets >= 5000);
    for(int i = 0; i < 5000; i++) s.set("key:" + std::to_string(i), "v");
    ASSERT_EQ(s.bucketCount(), buckets);
}

TEST(test_table_presize_is_capped) {
    // A capacity meant as "unlimited" must not allocate a table for it
    InMemoryStorage s(SIZE_MAX);
    ASSERT_TRUE(s.bucketCount() < (size_t{1} << 23));
    s.set("k", "v");
    ASSERT_EQ(s.get("k").value, "v");
}

// ==================== Key Table ====================

struct TableEntry {
    std::string key;
};
struct TableKeyOf {
    static std::string_view key(const TableEntry& e) { return e.key; }
};
// Every key hashes alike: one bucket, a chain of children, and tags that
// always match, so every lookup has to compare keys
struct SameHash {
    size_t operator()(std::string_view) const { return 42; }
};

TEST(test_keytable_basics) {
    KeyTable<TableEntry, TableKeyOf> t;
    ASSERT_TRUE(t.find("a") == nullptr);
    TableEntry a{"a"}, b{"b"};
    ASSERT_FALSE(t.erase(&a));                  // an empty table: nothing to erase
    t.insert(&a);
    t.insert(&b);
    ASSERT_EQ(t.size(), size_t{2});
    ASSERT_TRUE(t.find("a") == &a);
    ASSERT_TRUE(t.find("b") == &b);
    ASSERT_TRUE(t.find("c") == nullptr);
    ASSERT_TRUE(t.erase(&a));
    ASSERT_FALSE(t.erase(&a));
    ASSERT_TRUE(t.find("a") == nullptr);
    ASSERT_EQ(t.size(), size_t{1});
}

TEST(test_keytable_one_long_chain) {
    KeyTable<TableEntry, TableKeyOf, SameHash> t;
    std::vector<TableEntry> entries(100);
    for(int i = 0; i < 100; i++) {
        entries[i].key = "k" + std::to_string(i);
        t.insert(&entries[i]);
    }
    ASSERT_TRUE(t.childBuckets() > 0);
    for(int i = 0; i < 100; i++) ASSERT_TRUE(t.find(entries[i].key) == &entries[i]);
    ASSERT_TRUE(t.find("missing") == nullptr);
    // Every other key: empties child buckets in the middle of the chain
    for(int i = 0; i < 100; i += 2) ASSERT_TRUE(t.erase(&entries[i]));
    for(int i = 0; i < 100; i++) ASSERT_EQ(t.find(entries[i].key) == &entries[i], i % 2 == 1);
    for(int i = 1; i < 100; i += 2) ASSERT_TRUE(t.erase(&entries[i]));
    ASSERT_EQ(t.size(), size_t{0});
    ASSERT_EQ(t.childBuckets(), size_t{0});     // every emptied child bucket was freed
}

TEST(test_keytable_matches_unordered_map) {
    // Random inserts, erases and lookups over 50,000 keys, through many
    // rehashes, checked against std::unordered_map after every operation.
    // forEach must visit every entry exactly once, mid-rehash included.
    KeyTable<TableEntry, TableKeyOf> t;
    std::unordered_map<std::string, TableEntry*> ref;
    std::vector<std::unique_ptr<TableEntry>> pool;     // owns the entries
    std::mt19937 rng(7);
    int checked_mid_rehash = 0;
    for(int op = 0; op < 200000; op++) {
        std::string k = "key:" + std::to_string(rng() % 50000);
        unsigned dice = rng() % 100;
        auto it = ref.find(k);
        if(dice < 50) {
            if(it == ref.end()) {
                pool.push_back(std::make_unique<TableEntry>(TableEntry{k}));
                t.insert(pool.back().get());
                ref[k] = pool.back().get();
            }
        } else if(dice < 75) {
            if(it != ref.end()) {
                ASSERT_TRUE(t.erase(it->second));
                ref.erase(it);
            }
        } else {
            ASSERT_TRUE(t.find(k) == (it == ref.end() ? nullptr : it->second));
        }
        ASSERT_EQ(t.size(), ref.size());
        if(op % 5000 == 0 || (t.rehashing() && checked_mid_rehash < 20)) {
            if(t.rehashing()) checked_mid_rehash++;
            std::unordered_set<const TableEntry*> seen;
            bool all_known = true;
            t.forEach([&](TableEntry& e) {
                auto r = ref.find(e.key);
                all_known = all_known && r != ref.end() && r->second == &e;
                ASSERT_TRUE(seen.insert(&e).second);        // never twice
            });
            ASSERT_TRUE(all_known);
            ASSERT_EQ(seen.size(), ref.size());
        }
    }
    ASSERT_TRUE(checked_mid_rehash > 0);
    for(const auto& [k, e] : ref) ASSERT_TRUE(t.find(k) == e);
}

TEST(test_keytable_rehash_is_incremental) {
    // Once a rehash starts, each insert moves at most one bucket chain (or
    // passes up to 10 empty buckets), so the rehash ends within as many
    // inserts as the old table had buckets: no insert moves them all.
    KeyTable<TableEntry, TableKeyOf> t;
    std::vector<std::unique_ptr<TableEntry>> pool;
    int n = 0;
    auto add = [&] {
        pool.push_back(std::make_unique<TableEntry>(TableEntry{"k" + std::to_string(n++)}));
        t.insert(pool.back().get());
    };
    for(int growths = 0; growths < 5; growths++) {
        while(!t.rehashing()) add();
        size_t old_buckets = t.bucketCount() / 3;    // the old table, and the new one twice its size
        size_t inserts = 0;
        while(t.rehashing()) {
            add();
            inserts++;
            ASSERT_TRUE(inserts <= old_buckets);
        }
    }
    for(const auto& e : pool) ASSERT_TRUE(t.find(e->key) == e.get());
}

// ==================== SIEVE Eviction ====================

TEST(test_sieve_evicts_first_unvisited_key) {
    // A B C, then A is read; D needs room. The hand starts at the oldest (A),
    // finds it visited, clears the bit and moves on to B, which goes.
    InMemoryStorage s(3, nullptr, EvictionPolicy::SIEVE);
    s.set("A", "1");
    s.set("B", "2");
    s.set("C", "3");
    s.get("A");
    s.set("D", "4");
    ASSERT_TRUE(s.exists("A").success);
    ASSERT_FALSE(s.exists("B").success);
    ASSERT_TRUE(s.exists("C").success);
    ASSERT_TRUE(s.exists("D").success);
}

TEST(test_sieve_differs_from_lru) {
    // Both keys read, B then A. LRU evicts B, the less recently used. SIEVE's
    // hand clears A (the oldest), then B, wraps round and evicts A: a hit
    // never moves a key, so the oldest goes although it was read last.
    for(EvictionPolicy policy : {EvictionPolicy::LRU, EvictionPolicy::SIEVE}) {
        InMemoryStorage s(2, nullptr, policy);
        s.set("A", "1");
        s.set("B", "2");
        s.get("B");
        s.get("A");
        s.set("C", "3");
        bool sieve = policy == EvictionPolicy::SIEVE;
        ASSERT_EQ(s.exists("A").success, !sieve);
        ASSERT_EQ(s.exists("B").success, sieve);
        ASSERT_TRUE(s.exists("C").success);
    }
}

TEST(test_sieve_hand_moves_off_a_deleted_key) {
    // A B C, a read of A, then D: the hand clears A, evicts B and stops on C.
    // Deleting C must move the hand on, to D: left on the erased node it
    // would dangle (ASan reports that), and sent back to the oldest it would
    // evict A next instead of D.
    InMemoryStorage s(3, nullptr, EvictionPolicy::SIEVE);
    s.set("A", "1");
    s.set("B", "2");
    s.set("C", "3");
    s.get("A");
    s.set("D", "4");            // evicts B; the hand is on C
    ASSERT_FALSE(s.exists("B").success);
    s.del("C");                 // the hand moves to D
    s.set("E", "5");            // there's room: A, D, E
    s.set("F", "6");            // full: the hand is on D, unvisited, so D goes
    ASSERT_FALSE(s.exists("D").success);
    ASSERT_TRUE(s.exists("A").success);
    ASSERT_TRUE(s.exists("E").success);
    ASSERT_TRUE(s.exists("F").success);
}

// Which keys a cache holds under each policy, written differently from the
// engine on purpose: a vector with the oldest key first and SIEVE's hand as
// an index, where the engine uses list iterators. Slow (every step is a
// linear search), and fine for a few dozen keys.
struct EvictionModel {
    EvictionPolicy policy;
    size_t capacity;
    std::vector<std::string> queue;         // oldest first, newest last
    std::map<std::string, bool> visited;    // the keys held, and their bits
    long hand = -1;                         // index into queue; -1: start from the oldest

    bool has(const std::string& k) const { return visited.count(k) > 0; }
    size_t indexOf(const std::string& k) const {
        return static_cast<size_t>(std::find(queue.begin(), queue.end(), k) - queue.begin());
    }
    void hit(const std::string& k) {
        if(policy == EvictionPolicy::SIEVE) {
            visited[k] = true;
        } else {                                // LRU: to the newest end
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(indexOf(k)));
            queue.push_back(k);
        }
    }
    // Later entries shift down one. A hand on a later key follows it; a hand
    // on the removed key stays at i, which now holds the next newer key, or
    // nothing if the newest was removed.
    void remove(size_t i) {
        if(hand > static_cast<long>(i)) hand--;
        visited.erase(queue[i]);
        queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
        if(hand == static_cast<long>(queue.size())) hand = -1;
    }
    void evict() {
        size_t i = 0;                           // LRU: the oldest
        if(policy == EvictionPolicy::SIEVE) {
            i = hand >= 0 ? static_cast<size_t>(hand) : 0;
            while(visited[queue[i]]) {
                visited[queue[i]] = false;
                i = (i + 1) % queue.size();     // towards the newest, wrapping round
            }
            hand = static_cast<long>(i);
        }
        remove(i);
    }
    void set(const std::string& k) {
        if(has(k)) return hit(k);
        if(queue.size() >= capacity) evict();
        queue.push_back(k);
        visited[k] = false;
    }
    bool get(const std::string& k) {
        if(!has(k)) return false;
        hit(k);
        return true;
    }
    void del(const std::string& k) {
        if(has(k)) remove(indexOf(k));
    }
};

TEST(test_eviction_matches_a_model) {
    // Random GETs, SETs and DELs of 30 keys in room for 8, under each policy.
    // After every operation, every key must be present in the engine exactly
    // when it is in the model. EXISTS doesn't count as a use, so checking
    // doesn't change what is evicted.
    for(EvictionPolicy policy : {EvictionPolicy::LRU, EvictionPolicy::SIEVE}) {
        InMemoryStorage s(8, nullptr, policy);
        EvictionModel model{policy, 8, {}, {}, -1};
        std::mt19937 rng(policy == EvictionPolicy::SIEVE ? 2 : 1);
        for(int op = 0; op < 50000; op++) {
            std::string k = "k" + std::to_string(rng() % 30);
            unsigned dice = rng() % 100;
            if(dice < 45) {
                ASSERT_EQ(s.get(k).success, model.get(k));
            } else if(dice < 90) {
                s.set(k, "v");
                model.set(k);
            } else {
                s.del(k);
                model.del(k);
            }
            for(int j = 0; j < 30; j++) {
                std::string key = "k" + std::to_string(j);
                if(s.exists(key).success != model.has(key)) {
                    throw std::runtime_error(std::string(policy == EvictionPolicy::SIEVE ? "SIEVE" : "LRU") +
                                             ", operation " + std::to_string(op) + ": " + key +
                                             " is held by one of engine and model only");
                }
            }
        }
    }
}

TEST(test_load_file_with_a_key_twice) {
    // A key listed twice used to leave its first queue node behind with no
    // entry; evicting that node erased data_.end(), undefined behaviour.
    const std::string filename = "test_key_twice.dat";
    {
        std::ofstream out(filename);
        out << "S|a=1\nS|a=2\nS|b=3\n";
    }
    InMemoryStorage s(2);
    ASSERT_TRUE(s.loadFromFile(filename).success);
    ASSERT_EQ(s.get("a").value, "2");
    s.set("c", "4");            // evicts, through every node in the queue
    s.set("d", "5");
    s.set("e", "6");
    ASSERT_TRUE(s.exists("e").success);
    std::remove(filename.c_str());
}

// ==================== TTL Expiration ====================

TEST(test_ttl_expiration) {
    InMemoryStorage s(10);
    s.set("temp", "data", 1);
    
    Result r = s.get("temp");
    ASSERT_TRUE(r.success);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    r = s.get("temp");
    ASSERT_FALSE(r.success);
}

TEST(test_ttl_check_remaining) {
    InMemoryStorage s(10);
    s.set("key", "val", 10);

    Result r = s.ttl("key");
    ASSERT_TRUE(r.success);
    int remaining = std::stoi(r.value);
    ASSERT_TRUE(remaining >= 8 && remaining <= 10);
}

TEST(test_ttl_no_expiry) {
    InMemoryStorage s(10);
    s.set("key", "val");

    Result r = s.ttl("key");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "-1");
}

TEST(test_expire_existing_key) {
    InMemoryStorage s(10);
    s.set("key", "val");

    s.expire("key", 1);
    Result r = s.get("key");
    ASSERT_TRUE(r.success);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    r = s.get("key");
    ASSERT_FALSE(r.success);
}

TEST(test_exists_removes_expired_key) {
    InMemoryStorage s(10);
    s.set("key", "val", 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    Result r = s.exists("key");
    ASSERT_FALSE(r.success);
}

// ==================== TTL > LRU Priority ====================

TEST(test_ttl_evicted_before_lru) {
    InMemoryStorage s(2);
    s.set("A", "1", 1);
    s.set("B", "2");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    s.set("C", "3");

    Result r = s.get("A");
    ASSERT_FALSE(r.success);
    r = s.get("B");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "2");
    r = s.get("C");
    ASSERT_TRUE(r.success);
}

TEST(test_expired_keys_evicted_before_live_ones) {
    // Capacity 2. A expires, but was used after B, so B is at the least
    // recently used end. SET only looked for expired keys at that end, and
    // LPUSH, SADD and HSET didn't look at all: both evicted live B and kept
    // expired A.
    for(bool use_lpush : {false, true}) {
        InMemoryStorage s(2);
        s.set("B", "2");
        s.setExpiringAt("A", "1", InMemoryStorage::unixTimeMs() + 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(use_lpush) s.lpush("C", {"x"});
        else s.set("C", "3");
        ASSERT_TRUE(s.exists("B").success);
        ASSERT_TRUE(s.exists("C").success);
        ASSERT_FALSE(s.exists("A").success);
    }
}

// ==================== Persistence ====================

TEST(test_save_and_load) {
    const std::string filename = "test_save_load.dat";
    {
        InMemoryStorage s(10);
        s.set("name", "madhav");
        s.set("lang", "cpp");
        s.saveToFile(filename);
    }
    {
        InMemoryStorage s(10);
        Result r = s.loadFromFile(filename);
        ASSERT_TRUE(r.success);

        r = s.get("name");
        ASSERT_TRUE(r.success);
        ASSERT_EQ(r.value, "madhav");

        r = s.get("lang");
        ASSERT_TRUE(r.success);
        ASSERT_EQ(r.value, "cpp");
    }
    std::remove(filename.c_str());
}

TEST(test_load_enforces_capacity) {
    const std::string filename = "test_capacity.dat";
    {
        InMemoryStorage s(10);
        s.set("A", "1");
        s.set("B", "2");
        s.set("C", "3");
        s.set("D", "4");
        s.set("E", "5");
        s.saveToFile(filename);
    }
    {
        InMemoryStorage s(2);
        s.loadFromFile(filename);

        int count = 0;
        if(s.get("A").success) count++;
        if(s.get("B").success) count++;
        if(s.get("C").success) count++;
        if(s.get("D").success) count++;
        if(s.get("E").success) count++;
        ASSERT_EQ(count, 2);
    }
    std::remove(filename.c_str());
}

TEST(test_save_skips_expired_keys) {
    const std::string filename = "test_expired.dat";
    {
        InMemoryStorage s(10);
        s.set("alive", "yes");
        s.set("dead", "no", 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        s.saveToFile(filename);
    }
    {
        InMemoryStorage s(10);
        s.loadFromFile(filename);

        Result r = s.get("alive");
        ASSERT_TRUE(r.success);
        ASSERT_EQ(r.value, "yes");

        r = s.get("dead");
        ASSERT_FALSE(r.success);
    }
    std::remove(filename.c_str());
}

TEST(test_load_nonexistent_file) {
    InMemoryStorage s(10);
    Result r = s.loadFromFile("does_not_exist.dat");
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.error, ErrorCode::FILE_ERROR);
}

// ==================== Controller Validation ====================

TEST(test_controller_empty_key) {
    InMemoryStorage s(10);
    RedisLite redis(s);

    Result r = redis.get("");
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.error, ErrorCode::INVALID_KEY);

    r = redis.set("", "val");
    ASSERT_FALSE(r.success);

    r = redis.del("");
    ASSERT_FALSE(r.success);

    r = redis.exists("");
    ASSERT_FALSE(r.success);
}

// ==================== List Operations ====================

TEST(test_lpush_and_lrange) {
    InMemoryStorage s(10);
    Result r = s.lpush("mylist", {"c", "b", "a"});
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "3");  // length after push

    r = s.lrange("mylist", 0, -1);  // get all elements
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.values.size(), (size_t)3);
    // LPUSH pushes each in order, so last pushed is first: a, b, c
    ASSERT_EQ(r.values[0], "a");
    ASSERT_EQ(r.values[1], "b");
    ASSERT_EQ(r.values[2], "c");
}

TEST(test_rpush_and_lrange) {
    InMemoryStorage s(10);
    s.rpush("mylist", {"a", "b", "c"});

    Result r = s.lrange("mylist", 0, -1);
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.values.size(), (size_t)3);
    ASSERT_EQ(r.values[0], "a");
    ASSERT_EQ(r.values[1], "b");
    ASSERT_EQ(r.values[2], "c");
}

TEST(test_lpop_and_rpop) {
    InMemoryStorage s(10);
    s.rpush("mylist", {"a", "b", "c"});

    Result r = s.lpop("mylist");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "a");

    r = s.rpop("mylist");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "c");

    r = s.llen("mylist");
    ASSERT_EQ(r.value, "1");  // only "b" remains
}

TEST(test_lpop_empty_list_deletes_key) {
    InMemoryStorage s(10);
    s.rpush("mylist", {"only"});
    s.lpop("mylist");

    // Key should be auto-deleted when list becomes empty (Redis behavior)
    Result r = s.exists("mylist");
    ASSERT_FALSE(r.success);
}

TEST(test_lrange_negative_indices) {
    InMemoryStorage s(10);
    s.rpush("mylist", {"a", "b", "c", "d", "e"});

    // -2 means second-to-last, -1 means last
    Result r = s.lrange("mylist", -3, -1);
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.values.size(), (size_t)3);
    ASSERT_EQ(r.values[0], "c");
    ASSERT_EQ(r.values[1], "d");
    ASSERT_EQ(r.values[2], "e");
}

TEST(test_llen) {
    InMemoryStorage s(10);
    Result r = s.llen("empty");
    ASSERT_EQ(r.value, "0");

    s.rpush("mylist", {"a", "b"});
    r = s.llen("mylist");
    ASSERT_EQ(r.value, "2");
}

// ==================== Set Operations ====================

TEST(test_sadd_and_smembers) {
    InMemoryStorage s(10);
    Result r = s.sadd("myset", {"a", "b", "c"});
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "3");  // all 3 added

    r = s.smembers("myset");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.values.size(), (size_t)3);
}

TEST(test_sadd_duplicates) {
    InMemoryStorage s(10);
    s.sadd("myset", {"a", "b"});
    Result r = s.sadd("myset", {"b", "c"});  // "b" already exists
    ASSERT_EQ(r.value, "1");  // only "c" was newly added
}

TEST(test_srem) {
    InMemoryStorage s(10);
    s.sadd("myset", {"a", "b", "c"});
    Result r = s.srem("myset", {"b", "x"});  // "x" doesn't exist
    ASSERT_EQ(r.value, "1");  // only "b" was removed

    r = s.scard("myset");
    ASSERT_EQ(r.value, "2");
}

TEST(test_sismember) {
    InMemoryStorage s(10);
    s.sadd("myset", {"a", "b"});

    Result r = s.sismember("myset", "a");
    ASSERT_EQ(r.value, "1");  // exists

    r = s.sismember("myset", "z");
    ASSERT_EQ(r.value, "0");  // doesn't exist
}

TEST(test_srem_empty_set_deletes_key) {
    InMemoryStorage s(10);
    s.sadd("myset", {"only"});
    s.srem("myset", {"only"});

    Result r = s.exists("myset");
    ASSERT_FALSE(r.success);  // key auto-deleted
}

// ==================== Hash Operations ====================

TEST(test_hset_and_hget) {
    InMemoryStorage s(10);
    Result r = s.hset("user", {{"name", "madhav"}, {"age", "21"}});
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "2");  // 2 new fields added

    r = s.hget("user", "name");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "madhav");

    r = s.hget("user", "age");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "21");
}

TEST(test_hset_overwrite_field) {
    InMemoryStorage s(10);
    s.hset("user", {{"name", "old"}});
    Result r = s.hset("user", {{"name", "new"}});
    ASSERT_EQ(r.value, "0");  // 0 new fields (overwritten existing)

    r = s.hget("user", "name");
    ASSERT_EQ(r.value, "new");
}

TEST(test_hdel) {
    InMemoryStorage s(10);
    s.hset("user", {{"name", "madhav"}, {"age", "21"}, {"lang", "cpp"}});
    Result r = s.hdel("user", {"age", "ghost"});
    ASSERT_EQ(r.value, "1");  // only "age" was deleted

    r = s.hlen("user");
    ASSERT_EQ(r.value, "2");
}

TEST(test_hgetall) {
    InMemoryStorage s(10);
    s.hset("user", {{"name", "madhav"}, {"lang", "cpp"}});

    Result r = s.hgetall("user");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.values.size(), (size_t)4);  // 2 field-value pairs = 4 elements
}

TEST(test_hget_missing_field) {
    InMemoryStorage s(10);
    s.hset("user", {{"name", "madhav"}});

    Result r = s.hget("user", "phone");
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.error, ErrorCode::KEY_NOT_FOUND);
}

// ==================== Type Safety ====================

TEST(test_type_command) {
    InMemoryStorage s(10);
    s.set("str", "hello");
    s.lpush("lst", {"a"});
    s.sadd("st", {"x"});
    s.hset("hs", {{"k", "v"}});

    ASSERT_EQ(s.type("str").value, "string");
    ASSERT_EQ(s.type("lst").value, "list");
    ASSERT_EQ(s.type("st").value, "set");
    ASSERT_EQ(s.type("hs").value, "hash");
    ASSERT_EQ(s.type("nope").value, "none");
}

TEST(test_wrong_type_get_on_list) {
    InMemoryStorage s(10);
    s.lpush("mylist", {"a"});

    Result r = s.get("mylist");  // GET is a string op — can't use on list
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.error, ErrorCode::WRONG_TYPE);
}

TEST(test_wrong_type_lpush_on_string) {
    InMemoryStorage s(10);
    s.set("name", "madhav");

    Result r = s.lpush("name", {"oops"});  // LPUSH on a string key
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.error, ErrorCode::WRONG_TYPE);
}

TEST(test_set_overwrites_any_type) {
    InMemoryStorage s(10);
    s.lpush("key", {"a", "b"});

    // SET should overwrite regardless of existing type (Redis behavior)
    s.set("key", "string_now");
    Result r = s.get("key");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.value, "string_now");
    ASSERT_EQ(s.type("key").value, "string");
}

// ==================== LRU with Mixed Types ====================

TEST(test_lru_evicts_across_types) {
    InMemoryStorage s(2);
    s.set("str", "hello");
    s.sadd("myset", {"a"});
    // Cache is full (2 keys). Adding a 3rd should evict "str" (LRU)
    s.lpush("mylist", {"x"});

    Result r = s.get("str");
    ASSERT_FALSE(r.success);  // evicted

    r = s.scard("myset");
    ASSERT_EQ(r.value, "1");  // still alive

    r = s.llen("mylist");
    ASSERT_EQ(r.value, "1");  // still alive
}

// ==================== AOF Persistence ====================

// Helper: count RESP commands in raw AOF data
size_t countRespCommands(const std::string& raw) {
    std::string buf = raw;
    size_t count = 0;
    while(!buf.empty()) {
        auto parsed = RespParser::parse(buf);
        if(parsed.status != RespParser::ParseStatus::Ok) break;
        buf.erase(0, parsed.consumed);
        if(!parsed.args.empty()) count++;
    }
    return count;
}

TEST(test_aof_basic_replay) {
    const std::string aof_file = "test_aof_basic.aof";
    std::remove(aof_file.c_str());
    // Phase 1: Write with AOF logging
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        s.set("name", "madhav");
        s.set("lang", "cpp");
        s.del("lang");
    }
    // Phase 2: Create fresh storage, replay AOF — state should be restored
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);

        Result r = s.get("name");
        ASSERT_TRUE(r.success);
        ASSERT_EQ(r.value, "madhav");

        r = s.get("lang");
        ASSERT_FALSE(r.success);  // was deleted
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_all_types) {
    const std::string aof_file = "test_aof_types.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        s.set("str", "hello");
        s.rpush("lst", {"a", "b", "c"});
        s.sadd("st", {"x", "y"});
        s.hset("hs", {{"name", "madhav"}});
        s.lpop("lst");  // removes "a"
    }
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);

        ASSERT_EQ(s.get("str").value, "hello");
        ASSERT_EQ(s.llen("lst").value, "2");       // "b" and "c" remain
        ASSERT_EQ(s.scard("st").value, "2");
        ASSERT_EQ(s.hget("hs", "name").value, "madhav");
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_rewrite_compaction) {
    const std::string aof_file = "test_aof_rewrite.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        // Write 10 redundant commands
        s.set("key", "v1");
        s.set("key", "v2");
        s.set("key", "v3");
        s.set("key", "v4");
        s.set("key", "final");

        // Before rewrite: 5 commands in AOF
        ASSERT_EQ(countRespCommands(aof.readRaw()), (size_t)5);

        // Rewrite compacts to minimal state
        auto commands = s.dumpState();
        ASSERT_TRUE(aof.rewrite(commands));

        ASSERT_EQ(countRespCommands(aof.readRaw()), (size_t)1);  // only 1 SET needed
    }
    // Verify compacted AOF still restores correctly
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);
        ASSERT_EQ(s.get("key").value, "final");
    }
    std::remove(aof_file.c_str());
}

#ifndef _WIN32
// While it exists, files are limited to `bytes`: writes past that fail with
// EFBIG, as on a full disk. (SIGXFSZ, which would kill the process, is
// ignored.) The destructor restores both, even when an assertion throws.
//
// The limit applies to every file this process writes, including its own
// output when that is redirected to a file ("./tests > log"). A failed write
// leaves std::cout or std::cerr failed for good, and every later line is
// lost; std::cerr is tied to std::cout, so the logger's expected error
// message alone flushes std::cout. So output is flushed before the limit is
// set, and the streams are cleared after it is lifted. Only what is printed
// meanwhile (the logger's message) can be lost.
struct FileSizeLimit {
    explicit FileSizeLimit(rlim_t bytes) {
        std::cout.flush();
        std::cerr.flush();
        old_handler = std::signal(SIGXFSZ, SIG_IGN);
        getrlimit(RLIMIT_FSIZE, &old_limit);
        rlimit limit = old_limit;
        limit.rlim_cur = bytes;
        setrlimit(RLIMIT_FSIZE, &limit);
    }
    ~FileSizeLimit() {
        setrlimit(RLIMIT_FSIZE, &old_limit);
        std::signal(SIGXFSZ, old_handler);
        std::cout.clear();
        std::cerr.clear();
    }
    void (*old_handler)(int);
    rlimit old_limit;
};
#endif

TEST(test_aof_rewrite_failure_keeps_old_file) {
#ifndef _WIN32
    // A rewrite that fails halfway must leave the AOF as it was. The old
    // rewrite truncated the AOF and then wrote into it, so a full disk or a
    // crash halfway lost data, and the client was still told OK.
    const std::string aof_file = "test_aof_rewrite_fail.aof";
    std::remove(aof_file.c_str());
    AOFLogger aof(aof_file);
    InMemoryStorage s(1000, &aof);
    CommandContext ctx{s, &aof};
    for(int i = 0; i < 200; i++) s.set("key" + std::to_string(i), std::string(100, 'v'));
    const std::string before = aof.readRaw();      // about 26 KB

    std::string reply;
    {
        FileSizeLimit limit(4096);
        reply = executeCommand(ctx, {"BGREWRITEAOF"});
    }
    ASSERT_EQ(reply, "-ERR AOF rewrite failed: cannot write " + aof_file + ".tmp: File too large\r\n");
    ASSERT_TRUE(aof.readRaw() == before);
    ASSERT_FALSE(std::ifstream(aof_file + ".tmp").good());    // the partial file is gone

    s.set("after", "1");            // still logging to the same file
    InMemoryStorage replayed(1000, &aof);
    replayAOF(replayed, aof);
    ASSERT_EQ(replayed.dumpState().size(), (size_t)201);
    std::remove(aof_file.c_str());
#endif
}

TEST(test_aof_failed_writes_wait_and_are_retried) {
#ifndef _WIN32
    // A failed write used to be ignored: its record was missing from the AOF
    // for good. Now what it didn't write waits, later records queue behind it
    // in order, and all of it reaches the file once there is room.
    const std::string aof_file = "test_aof_write_error.aof";
    std::remove(aof_file.c_str());
    AOFLogger aof(aof_file);
    const std::string record = RespParser::array({"SET", "k", std::string(100, 'v')});
    std::string expected;
    {
        FileSizeLimit limit(1000);
        while(aof.log(record)) expected += record;
        expected += record;                         // partly written, the rest waiting
        ASSERT_EQ(aof.readRaw().size(), (size_t)1000);
        ASSERT_FALSE(aof.flushPending());
        ASSERT_EQ(aof.lastWriteError(), "File too large");
        ASSERT_FALSE(aof.log(record));              // waits behind it
        expected += record;
    }
    ASSERT_TRUE(aof.flushPending());                // room again: everything is written
    ASSERT_EQ(aof.lastWriteError(), "");
    ASSERT_TRUE(aof.readRaw() == expected);
    ASSERT_TRUE(aof.log(record));
    std::remove(aof_file.c_str());
#endif
}

TEST(test_aof_rewrite_replaces_failed_writes) {
#ifndef _WIN32
    // What a failed write left waiting belongs at the end of the old file. A
    // rewrite replaces that file with one holding all the data, so it must be
    // dropped: written after the new file's last record it would be garbage.
    const std::string aof_file = "test_aof_rewrite_pending.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(100, &aof);
        s.set("a", "1");
        {
            FileSizeLimit limit(aof.readRaw().size() + 10);
            s.set("b", std::string(100, 'v'));      // 10 bytes written, the rest waiting
            ASSERT_FALSE(aof.flushPending());
        }
        ASSERT_TRUE(aof.rewrite(s.dumpState()));
        ASSERT_TRUE(aof.flushPending());            // nothing waits any more
        s.set("c", "3");
    }
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(100, &aof);
        auto loaded = replayAOF(s, aof);
        ASSERT_TRUE(loaded.ok);
        ASSERT_EQ(loaded.truncated_bytes, (size_t)0);
        ASSERT_EQ(s.dumpState().size(), (size_t)3);
    }
    std::remove(aof_file.c_str());
#endif
}

TEST(test_aof_deferred_writes_wait_for_flush) {
    // Group commit: in deferred mode (the server's), records are only buffered,
    // and flush() writes them all, in order, with one write()
    const std::string aof_file = "test_aof_deferred.aof";
    std::remove(aof_file.c_str());
    const std::string a = RespParser::array({"SET", "a", "1"});
    const std::string b = RespParser::array({"SET", "b", "2"});
    {
        AOFLogger aof(aof_file);
        aof.setDeferred(true);
        ASSERT_TRUE(aof.log(a));
        ASSERT_TRUE(aof.log(b));
        ASSERT_TRUE(aof.readRaw().empty());         // nothing written yet
        ASSERT_TRUE(aof.flush());
        ASSERT_TRUE(aof.readRaw() == a + b);
        ASSERT_TRUE(aof.flush());                   // nothing left to write
        ASSERT_TRUE(aof.log(a));
        aof.setDeferred(false);                     // writes what is buffered
        ASSERT_TRUE(aof.readRaw() == a + b + a);
        ASSERT_TRUE(aof.log(b));                    // written at once again
        ASSERT_TRUE(aof.readRaw() == a + b + a + b);
        aof.setDeferred(true);
        ASSERT_TRUE(aof.log(a));
    }                                               // the destructor writes it
    AOFLogger aof(aof_file);
    ASSERT_TRUE(aof.readRaw() == a + b + a + b + a);
    std::remove(aof_file.c_str());
}

TEST(test_aof_deferred_write_errors) {
#ifndef _WIN32
    // A flush that fails (a full disk) keeps what it didn't write, in order,
    // as an immediate write does, and writes it all once there is room
    const std::string aof_file = "test_aof_deferred_error.aof";
    std::remove(aof_file.c_str());
    AOFLogger aof(aof_file);
    aof.setDeferred(true);
    const std::string record = RespParser::array({"SET", "k", std::string(100, 'v')});
    std::string expected;
    for(int i = 0; i < 20; i++) {                   // 128 bytes each
        ASSERT_TRUE(aof.log(record));
        expected += record;
    }
    {
        FileSizeLimit limit(1000);
        ASSERT_FALSE(aof.flush());
        ASSERT_EQ(aof.readRaw().size(), (size_t)1000);
        ASSERT_FALSE(aof.flushPending());
        ASSERT_EQ(aof.lastWriteError(), "File too large");
        ASSERT_FALSE(aof.log(record));              // buffered behind the rest
        expected += record;
    }
    ASSERT_TRUE(aof.flushPending());                // room again: everything is written
    ASSERT_TRUE(aof.readRaw() == expected);
    ASSERT_EQ(aof.lastWriteError(), "");
    std::remove(aof_file.c_str());
#endif
}

TEST(test_aof_rewrite_drops_buffered_records) {
    // A rewrite writes a snapshot that already includes the effect of every
    // record still buffered. Those records must not be written after it:
    // replay would apply them twice, and an RPUSH twice is a wrong value.
    const std::string aof_file = "test_aof_rewrite_buffered.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        aof.setDeferred(true);
        InMemoryStorage s(100, &aof);
        s.rpush("list", {"x"});                     // buffered, and in the snapshot below
        ASSERT_TRUE(aof.rewrite(s.dumpState()));
        s.rpush("list", {"y"});                     // after the rewrite: must be kept
        ASSERT_TRUE(aof.flush());
    }
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(100, &aof);
        ASSERT_TRUE(replayAOF(s, aof).ok);
        ASSERT_EQ(s.llen("list").value, "2");       // x, y; not x, x, y
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_fsync_always_syncs_each_write_or_group) {
    // appendfsync always: a sync after every write, so before any reply. With
    // group commit the server writes once per pass, so one sync covers every
    // command of the pass: that is what makes "always" affordable.
    const std::string aof_file = "test_aof_fsync_always.aof";
    std::remove(aof_file.c_str());
    const std::string record = RespParser::array({"SET", "k", "v"});
    {
        AOFLogger aof(aof_file);
        aof.setFsyncPolicy(AOFLogger::FsyncPolicy::Always);
        ASSERT_TRUE(aof.log(record));
        ASSERT_TRUE(aof.log(record));
        ASSERT_EQ(aof.syncs(), (size_t)2);          // one per write
        aof.setDeferred(true);
        for(int i = 0; i < 3; i++) ASSERT_TRUE(aof.log(record));
        ASSERT_EQ(aof.syncs(), (size_t)2);          // buffered: nothing to sync yet
        ASSERT_TRUE(aof.flush());
        ASSERT_EQ(aof.syncs(), (size_t)3);          // one write, one sync, for all three
        ASSERT_TRUE(aof.flush());
        ASSERT_EQ(aof.syncs(), (size_t)3);          // nothing new: no sync
        ASSERT_FALSE(aof.failedForGood());
    }
    {
        AOFLogger aof(aof_file);                    // no: the OS decides
        aof.setFsyncPolicy(AOFLogger::FsyncPolicy::No);
        ASSERT_TRUE(aof.log(record));
        ASSERT_EQ(aof.syncs(), (size_t)0);
        ASSERT_EQ(aof.readRaw().size(), 6 * record.size());
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_fsync_everysec_syncs_in_the_background) {
    // appendfsync everysec: a background thread syncs about once a second,
    // and only if something was written since the last sync
    const std::string aof_file = "test_aof_fsync_everysec.aof";
    std::remove(aof_file.c_str());
    AOFLogger aof(aof_file);
    aof.setFsyncPolicy(AOFLogger::FsyncPolicy::EverySecond);
    ASSERT_TRUE(aof.log(RespParser::array({"SET", "k", "v"})));
    ASSERT_EQ(aof.syncs(), (size_t)0);              // not by the write itself
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while(aof.syncs() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_EQ(aof.syncs(), (size_t)1);              // within about a second
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT_EQ(aof.syncs(), (size_t)1);              // nothing new: no sync
    std::remove(aof_file.c_str());
}

TEST(test_aof_fsync_always_write_error_is_fatal) {
#ifndef _WIN32
    // With "always", a reply must mean the write is on the disk. A write that
    // fails breaks that promise for good, so the server must stop (Redis exits)
    const std::string aof_file = "test_aof_fsync_always_error.aof";
    std::remove(aof_file.c_str());
    AOFLogger aof(aof_file);
    aof.setFsyncPolicy(AOFLogger::FsyncPolicy::Always);
    {
        FileSizeLimit limit(100);
        ASSERT_FALSE(aof.log(RespParser::array({"SET", "k", std::string(200, 'v')})));
    }
    ASSERT_TRUE(aof.failedForGood());
    ASSERT_EQ(aof.lastWriteError(), "File too large");
    std::remove(aof_file.c_str());
#endif
}

TEST(test_aof_no_log_on_failed_ops) {
    const std::string aof_file = "test_aof_nolog.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        s.set("key", "val");
        s.del("nonexistent");       // fails — should NOT be logged
        s.get("nonexistent");       // read — should NOT be logged
        s.lpush("key", {"oops"});   // WRONG_TYPE — should NOT be logged

        ASSERT_EQ(countRespCommands(aof.readRaw()), (size_t)1);  // only the successful SET
    }
    std::remove(aof_file.c_str());
}

// Every record in an AOF, parsed
std::vector<std::vector<std::string>> aofRecords(const std::string& raw) {
    std::vector<std::vector<std::string>> records;
    size_t pos = 0;
    while(pos < raw.size()) {
        auto parsed = RespParser::parse(std::string_view(raw).substr(pos));
        if(parsed.status != RespParser::ParseStatus::Ok) break;
        pos += parsed.consumed;
        if(!parsed.args.empty()) records.push_back(parsed.args);
    }
    return records;
}

// Writes `records` as an AOF file, as if the server had logged them
void writeAof(const std::string& path, const std::vector<std::vector<std::string>>& records) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for(const auto& record : records) out << RespParser::array(record);
}

TEST(test_aof_rewrite_preserves_ttl) {
    // dumpState() must keep each key's expiry: a string's in its SET record,
    // so one record holds both
    const std::string aof_file = "test_aof_ttl.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        s.set("persistent", "no_ttl");
        s.set("expiring", "has_ttl", 300);  // 5 minute TTL
        s.rpush("list", {"a"});
        s.expire("list", 300);

        // Rewrite compacts the AOF
        auto commands = s.dumpState();
        ASSERT_TRUE(aof.rewrite(commands));

        bool found_set = false, found_list_expiry = false;
        for(const auto& r : aofRecords(aof.readRaw())) {
            if(r.size() == 5 && r[0] == "SET" && r[1] == "expiring" && r[3] == "PXAT") found_set = true;
            if(r.size() == 3 && r[0] == "PEXPIREAT" && r[1] == "list") found_list_expiry = true;
        }
        ASSERT_TRUE(found_set);
        ASSERT_TRUE(found_list_expiry);
    }
    // Verify replaying compacted AOF restores TTL
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);

        Result r = s.ttl("persistent");
        ASSERT_EQ(r.value, "-1");  // no TTL

        r = s.ttl("expiring");
        ASSERT_TRUE(r.success);
        int remaining = std::stoi(r.value);
        ASSERT_TRUE(remaining > 0);  // TTL survived rewrite + replay
        ASSERT_TRUE(std::stoi(s.ttl("list").value) > 0);
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_logs_absolute_expiry_times) {
    const std::string aof_file = "test_aof_pxat.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        int64_t before = InMemoryStorage::unixTimeMs();
        s.set("k", "v", 10);
        s.expire("k", 20);
        int64_t after = InMemoryStorage::unixTimeMs();

        auto records = aofRecords(aof.readRaw());
        ASSERT_EQ(records.size(), (size_t)2);
        ASSERT_EQ(records[0].size(), (size_t)5);
        ASSERT_EQ(records[0][0], "SET");
        ASSERT_EQ(records[0][3], "PXAT");
        int64_t at = std::stoll(records[0][4]);
        ASSERT_TRUE(at >= before + 10000 && at <= after + 10000);
        ASSERT_EQ(records[1][0], "PEXPIREAT");
        at = std::stoll(records[1][2]);
        ASSERT_TRUE(at >= before + 20000 && at <= after + 20000);
        ASSERT_EQ(s.ttl("k").value, "20");      // rounded to the nearest second, as Redis does
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_expiry_is_absolute) {
    // TTLs used to be logged as relative seconds ("SETEX k 1 v", "EXPIRE k 1"),
    // so every restart gave keys their full TTL again, even keys that had
    // already expired. They must expire at the same moment after a restart.
    //
    // The AOF holds wall-clock times, but this test waits on the monotonic
    // clock, so a step of the wall clock during the wait shifts the result.
    // WSL2 steps it when it resyncs with Windows (its journal logs "Time
    // jumped backwards"). With 1 s TTLs and a 1.1 s wait, a step back of over
    // 100 ms made this fail once; 200 ms TTLs leave a 900 ms margin.
    const std::string aof_file = "test_aof_absolute.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        int64_t soon = InMemoryStorage::unixTimeMs() + 200;
        s.setExpiringAt("setex", "v", soon);
        s.set("expire", "v");
        s.expireAt("expire", soon);
        s.setExpiringAt("extended", "v", soon);     // its first TTL passes before the restart,
        s.expire("extended", 100);                  // but a later record extends it
        s.set("long", "v", 100);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);
        ASSERT_FALSE(s.get("setex").success);
        ASSERT_FALSE(s.get("expire").success);
        ASSERT_TRUE(s.get("extended").success);
        int extended = std::stoi(s.ttl("extended").value);
        ASSERT_TRUE(extended >= 97 && extended <= 99);
        int remaining = std::stoi(s.ttl("long").value);
        ASSERT_TRUE(remaining >= 97 && remaining <= 99);
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_old_relative_records_still_load) {
    // AOFs written before expiry times became absolute hold SETEX and EXPIRE
    // records. They still load, with the TTL counted from the replay.
    const std::string aof_file = "test_aof_old_format.aof";
    writeAof(aof_file, {{"SETEX", "a", "100", "v"}, {"SET", "b", "v"}, {"EXPIRE", "b", "100"}});
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);
        ASSERT_EQ(s.get("a").value, "v");
        ASSERT_EQ(s.ttl("a").value, "100");
        ASSERT_EQ(s.ttl("b").value, "100");
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_rewrite_keeps_ttl_under_a_second) {
    // A rewrite used to write a key's TTL in whole seconds, and nothing at all
    // when under a second was left: the key came back without a TTL, forever.
    const std::string aof_file = "test_aof_short_ttl.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        s.set("k", "v", 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ASSERT_TRUE(aof.rewrite(s.dumpState()));
    }
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);
        ASSERT_TRUE(s.get("k").success);
        ASSERT_TRUE(s.ttl("k").value != "-1");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        ASSERT_FALSE(s.get("k").success);
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_eviction_is_logged) {
    // Capacity 2: the live server evicts B, because A was read after it. GETs
    // aren't logged, so replay used to evict A instead: a restart brought back a
    // key the server had dropped and lost one it still had.
    const std::string aof_file = "test_aof_eviction.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(2, &aof);
        s.set("A", "1");
        s.set("B", "2");
        s.get("A");
        s.set("C", "3");
        ASSERT_TRUE(s.exists("A").success);
        ASSERT_FALSE(s.exists("B").success);
    }
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(2, &aof);
        replayAOF(s, aof);
        ASSERT_TRUE(s.exists("A").success);
        ASSERT_FALSE(s.exists("B").success);
        ASSERT_TRUE(s.exists("C").success);
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_lazy_expiry_is_logged) {
    // The live server removes the expired string and makes k a list. Replay
    // doesn't expire anything until loading is done, so without a logged DEL
    // the string was still there and LPUSH failed with WRONGTYPE: k was lost.
    const std::string aof_file = "test_aof_lazy_expiry.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        CommandContext ctx{s};
        executeCommand(ctx, {"SET", "k", "v", "PX", "50"});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_EQ(executeCommand(ctx, {"LPUSH", "k", "x"}), ":1\r\n");
    }
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);
        ASSERT_EQ(s.type("k").value, "list");
        ASSERT_EQ(s.llen("k").value, "1");
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_replay_into_smaller_capacity) {
    // Written with room for 10 keys, replayed with room for 2. Evicting during
    // replay would drop L at "SET C" and then rebuild it from "RPUSH L b" alone,
    // a wrong value rather than a missing one. Replay loads everything and then
    // evicts down to the capacity, logging those evictions too.
    const std::string aof_file = "test_aof_smaller.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        s.rpush("L", {"a"});
        s.set("B", "1");
        s.set("C", "1");
        s.rpush("L", {"b"});
    }
    for(int restart = 0; restart < 2; restart++) {
        AOFLogger aof(aof_file);
        InMemoryStorage s(2, &aof);
        replayAOF(s, aof);
        ASSERT_EQ(s.lrange("L", 0, -1).values, (std::vector<std::string>{"a", "b"}));
        ASSERT_FALSE(s.exists("B").success);
        ASSERT_TRUE(s.exists("C").success);
    }
    std::remove(aof_file.c_str());
}

// One key as a rewrite records it: the record that recreates it, with set
// members and hash fields sorted (their order is unspecified), and its expiry
// as Unix milliseconds, or -1 if it has none
struct KeyState {
    std::vector<std::string> record;
    int64_t expiry = -1;
};

std::map<std::string, KeyState> snapshot(const InMemoryStorage& s) {
    std::map<std::string, KeyState> keys;
    for(const auto& raw : s.dumpState()) {
        auto args = RespParser::parse(raw).args;
        KeyState& state = keys[args[1]];
        if(args[0] == "PEXPIREAT") {
            state.expiry = std::stoll(args[2]);
            continue;
        }
        if(args[0] == "SET" && args.size() == 5) {     // SET key value PXAT ms
            state.expiry = std::stoll(args[4]);
            args.resize(3);
        }
        std::vector<std::string> contents(args.begin() + 2, args.end());
        if(args[0] == "SADD") std::sort(contents.begin(), contents.end());
        if(args[0] == "HSET") {
            std::vector<std::pair<std::string, std::string>> fields;
            for(size_t i = 0; i + 1 < contents.size(); i += 2) fields.emplace_back(contents[i], contents[i + 1]);
            std::sort(fields.begin(), fields.end());
            contents.clear();
            for(const auto& [f, v] : fields) { contents.push_back(f); contents.push_back(v); }
        }
        state.record = {args[0]};
        state.record.insert(state.record.end(), contents.begin(), contents.end());
    }
    return keys;
}

std::string describe(const std::map<std::string, KeyState>& keys, const std::string& key) {
    auto it = keys.find(key);
    if(it == keys.end()) return "(missing)";
    std::string text;
    for(const auto& part : it->second.record) text += part + " ";
    return text + "expiry " + std::to_string(it->second.expiry);
}

// The wall clock minus the monotonic clock, in ms. It changes when the wall
// clock is stepped: WSL2 steps it every 30 s to follow Windows, measured by up
// to 341 ms.
int64_t wallMinusMonotonicMs() {
    using namespace std::chrono;
    return InMemoryStorage::unixTimeMs() -
           duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Runs `ops` random commands, through the dispatcher the server uses, against 8
// keys in room for 5, with short and long TTLs, reads that touch the LRU order
// and the odd rewrite. Then replays the AOF into a new instance and checks it
// holds exactly what the live one holds.
//
// Expiry is logged as wall-clock time but kept in memory on the monotonic
// clock. If the wall clock is stepped during the run, replay legitimately
// differs from the live data (as designed: see the README's trade-offs): a key
// that expired live can come back with time left, and expiry times shift by the
// step. So a mismatch is a failure only if the wall clock held steady; if it
// was stepped, this returns false and the caller runs the seed again. Measured
// after every command, so a step anywhere in the run is seen.
bool checkReplayMatchesLive(unsigned seed, int ops) {
    const std::string aof_file = "test_aof_random.aof";
    std::remove(aof_file.c_str());
    std::mt19937 rng(seed);
    auto pick = [&](unsigned n) { return static_cast<int>(rng() % n); };
    std::map<std::string, KeyState> live, replayed;

    int64_t lowest_offset = wallMinusMonotonicMs(), highest_offset = lowest_offset;
    auto trackClock = [&] {
        int64_t offset = wallMinusMonotonicMs();
        lowest_offset = std::min(lowest_offset, offset);
        highest_offset = std::max(highest_offset, offset);
    };
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(5, &aof);
        CommandContext ctx{s, &aof};
        for(int i = 0; i < ops; i++) {
            std::string key = "k" + std::to_string(pick(8));
            std::string value(1, static_cast<char>('a' + pick(4)));
            switch(pick(16)) {
                case 0:  executeCommand(ctx, {"SET", key, value}); break;
                case 1:  executeCommand(ctx, {"SET", key, value, "PX", std::to_string(1 + pick(20))}); break;
                case 2:  executeCommand(ctx, {"SET", key, value, "EX", "1000"}); break;
                case 3:
                case 4:  executeCommand(ctx, {"GET", key}); break;
                case 5:  executeCommand(ctx, {"DEL", key}); break;
                case 6:  executeCommand(ctx, {"EXISTS", key}); break;
                case 7:  executeCommand(ctx, {"EXPIRE", key, "1000"}); break;
                case 8:  executeCommand(ctx, {"PEXPIREAT", key,     // up to 10 ms ago, or soon
                             std::to_string(InMemoryStorage::unixTimeMs() + pick(30) - 10)}); break;
                case 9:  executeCommand(ctx, {"LPUSH", key, value, value}); break;
                case 10: executeCommand(ctx, {"RPOP", key}); break;
                case 11: executeCommand(ctx, {"SADD", key, value}); break;
                case 12: executeCommand(ctx, {"SREM", key, value}); break;
                case 13: executeCommand(ctx, {"HSET", key, value, std::to_string(i)}); break;
                case 14: executeCommand(ctx, {"HDEL", key, value}); break;
                case 15: if(pick(25) == 0) executeCommand(ctx, {"BGREWRITEAOF"}); break;
            }
            trackClock();
            if(i % 40 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        // Let every short TTL pass, so none expires between the two snapshots
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        live = snapshot(s);
        trackClock();
    }
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(5, &aof);
        auto loaded = replayAOF(s, aof);
        if(!loaded.ok || loaded.truncated_bytes > 0) {
            throw std::runtime_error("seed " + std::to_string(seed) + ": the AOF didn't load cleanly: " + loaded.error);
        }
        replayed = snapshot(s);
        trackClock();
    }
    std::remove(aof_file.c_str());

    // Reading the two clocks a moment apart, and to the millisecond, moves the
    // offset by a ms or so; more than this is a step of the wall clock
    constexpr int64_t kClockJitterMs = 10;
    bool clock_stepped = highest_offset - lowest_offset > kClockJitterMs;

    std::map<std::string, KeyState> all = live;
    all.insert(replayed.begin(), replayed.end());
    for(const auto& [key, unused] : all) {
        auto l = live.find(key), r = replayed.find(key);
        bool same = l != live.end() && r != replayed.end() && l->second.record == r->second.record &&
                    (l->second.expiry == -1) == (r->second.expiry == -1) &&
                    std::llabs(l->second.expiry - r->second.expiry) <= 50;   // clock reads differ by a ms or so
        if(same) continue;
        if(clock_stepped) return false;     // not a valid run: the caller runs it again
        throw std::runtime_error("seed " + std::to_string(seed) + ", key " + key + ": live " +
                                 describe(live, key) + ", replayed " + describe(replayed, key));
    }
    return true;
}

TEST(test_aof_replay_matches_live_randomized) {
    // RANDOM_TEST_SEEDS=<n> runs more seeds than the default 4 (each ~0.2 s)
    unsigned seeds = 4;
    if(const char* n = std::getenv("RANDOM_TEST_SEEDS")) seeds = static_cast<unsigned>(std::atoi(n));
    for(unsigned seed = 1; seed <= seeds; seed++) {
        // A run that mismatched while the wall clock was stepped proves nothing
        // either way; the same seed (the same commands) runs again. WSL2 steps
        // the clock every 30 s and a run takes well under a second, so a rerun
        // nearly always has a steady clock.
        int attempts = 1;
        while(!checkReplayMatchesLive(seed, 2500)) {
            if(++attempts > 5) {
                throw std::runtime_error("seed " + std::to_string(seed) +
                                         ": the wall clock was stepped during 5 runs in a row");
            }
        }
    }
}

TEST(test_aof_replay_uses_command_table) {
    // Replay runs records through the same table as client commands, so names
    // are case-insensitive for both. The old replay compared names
    // case-sensitively and silently skipped "set".
    const std::string aof_file = "test_aof_table.aof";
    writeAof(aof_file, {{"set", "a", "1"}, {"Rpush", "l", "x", "y"}});
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        auto loaded = replayAOF(s, aof);
        ASSERT_TRUE(loaded.ok);
        ASSERT_EQ(loaded.commands, (size_t)2);
        ASSERT_EQ(s.get("a").value, "1");
        ASSERT_EQ(s.llen("l").value, "2");
        ASSERT_EQ(countRespCommands(aof.readRaw()), (size_t)2);   // nothing logged again
    }
    std::remove(aof_file.c_str());
}

void writeRaw(const std::string& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

TEST(test_aof_replay_cuts_incomplete_last_record) {
    // A crash while writing "SET b 2" left part of it at the end. Replay used to
    // stop there and leave it, so the next records were appended after it and,
    // at the next start, read as its missing part: lost.
    const std::string aof_file = "test_aof_incomplete.aof";
    const std::string partial = "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1";
    writeRaw(aof_file, RespParser::array({"SET", "a", "1"}) + partial);
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        auto loaded = replayAOF(s, aof);
        ASSERT_TRUE(loaded.ok);
        ASSERT_EQ(loaded.commands, (size_t)1);
        ASSERT_EQ(loaded.truncated_bytes, partial.size());
        ASSERT_EQ(aof.readRaw(), RespParser::array({"SET", "a", "1"}));
        s.set("c", "3");
    }
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        auto loaded = replayAOF(s, aof);
        ASSERT_TRUE(loaded.ok);
        ASSERT_EQ(loaded.truncated_bytes, (size_t)0);
        ASSERT_EQ(s.get("a").value, "1");
        ASSERT_EQ(s.get("c").value, "3");
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_replay_refuses_damage_before_the_end) {
    // Replay used to stop at the first bad record and start with what came
    // before it, silently dropping everything after. And since only write
    // commands are logged, any other command means the file is damaged too.
    const std::string aof_file = "test_aof_damaged.aof";
    const std::string good = RespParser::array({"SET", "a", "1"});
    const std::string at = std::to_string(good.size());
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"*2\r\n$3\r\nGET\r\nxx\r\n", "expected '$', got 'x'"},
        {RespParser::array({"GET", "a"}), "unexpected command 'GET'"},
        {RespParser::array({"NOSUCH"}), "unexpected command 'NOSUCH'"},
        {RespParser::array({"SET", "b"}), "unexpected command 'SET'"},      // too few arguments
    };
    for(const auto& [bad, reason] : cases) {
        writeRaw(aof_file, good + bad + good);
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        auto loaded = replayAOF(s, aof);
        ASSERT_FALSE(loaded.ok);
        ASSERT_TRUE(loaded.error.find("damaged at byte " + at + " (" + reason + ")") != std::string::npos);
        ASSERT_TRUE(loaded.error.find("truncate -s " + at + " " + aof_file) != std::string::npos);
        ASSERT_EQ(aof.readRaw(), good + bad + good);     // left as it was
    }
    std::remove(aof_file.c_str());
}

TEST(test_aof_values_with_spaces) {
    // THIS TEST would FAIL with the old plain-text AOF format.
    // RESP format is binary-safe — length-prefixed, so spaces are preserved.
    const std::string aof_file = "test_aof_spaces.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        s.set("greeting", "hello world");   // value with space
        s.set("quote", "to be or not to be");
        s.rpush("words", {"foo bar", "baz qux"});
        s.hset("user", {{"bio", "loves c++"}});
    }
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);

        ASSERT_EQ(s.get("greeting").value, "hello world");
        ASSERT_EQ(s.get("quote").value, "to be or not to be");

        auto lr = s.lrange("words", 0, -1);
        ASSERT_EQ(lr.values.size(), (size_t)2);
        ASSERT_EQ(lr.values[0], "foo bar");
        ASSERT_EQ(lr.values[1], "baz qux");

        ASSERT_EQ(s.hget("user", "bio").value, "loves c++");
    }
    std::remove(aof_file.c_str());
}

// ==================== Command Table ====================
// The dispatcher the server uses, called directly: no sockets involved.

TEST(test_dispatch_is_case_insensitive) {
    InMemoryStorage s(10);
    CommandContext ctx{s};
    ASSERT_EQ(executeCommand(ctx, {"set", "k", "v"}), "+OK\r\n");
    ASSERT_EQ(executeCommand(ctx, {"GeT", "k"}), "$1\r\nv\r\n");
    ASSERT_EQ(executeCommand(ctx, {"GET", "missing"}), "$-1\r\n");
}

TEST(test_dispatch_checks_arity) {
    InMemoryStorage s(10);
    CommandContext ctx{s};
    ASSERT_EQ(executeCommand(ctx, {"GET"}), "-ERR wrong number of arguments for 'get' command\r\n");
    ASSERT_EQ(executeCommand(ctx, {"get", "a", "b"}),
              "-ERR wrong number of arguments for 'get' command\r\n");
    ASSERT_EQ(executeCommand(ctx, {"LPUSH", "k"}),
              "-ERR wrong number of arguments for 'lpush' command\r\n");
    ASSERT_EQ(executeCommand(ctx, {"HSET", "h", "f", "v", "g"}),   // field without a value
              "-ERR wrong number of arguments for 'hset' command\r\n");
    ASSERT_FALSE(s.exists("k").success);    // nothing ran
    ASSERT_FALSE(s.exists("h").success);
}

TEST(test_dispatch_unknown_command) {
    InMemoryStorage s(10);
    CommandContext ctx{s};
    ASSERT_EQ(executeCommand(ctx, {"NOPE", "x"}), "-ERR unknown command 'NOPE'\r\n");
    // Longer than any command: rejected without upper-casing a copy; 128 bytes quoted
    ASSERT_EQ(executeCommand(ctx, {std::string(1000, 'x')}),
              "-ERR unknown command '" + std::string(128, 'x') + "'\r\n");
    ASSERT_TRUE(lookupCommand("\xff\xfe") == nullptr);
}

TEST(test_dispatch_replies) {
    InMemoryStorage s(10);
    CommandContext ctx{s, nullptr, 3};
    ASSERT_EQ(executeCommand(ctx, {"RPUSH", "l", "a", "b"}), ":2\r\n");
    ASSERT_EQ(executeCommand(ctx, {"LRANGE", "l", "0", "-1"}), "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    ASSERT_EQ(executeCommand(ctx, {"GET", "l"}),
              "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
    ASSERT_EQ(executeCommand(ctx, {"TYPE", "l"}), "+list\r\n");
    ASSERT_EQ(executeCommand(ctx, {"TTL", "missing"}), ":-2\r\n");
    ASSERT_TRUE(executeCommand(ctx, {"INFO"}).find("connected_clients:3\r\n") != std::string::npos);
    ASSERT_EQ(executeCommand(ctx, {"BGREWRITEAOF"}).substr(0, 5), "-ERR ");   // AOF is off
}

TEST(test_dispatch_appends_replies) {
    // The server passes a client's output buffer, which can still hold replies
    // the socket hasn't taken: a reply must be appended, never replace them
    InMemoryStorage s(10);
    CommandContext ctx{s};
    std::string out = "+earlier reply\r\n";
    executeCommand(ctx, {"SET", "k", std::string(100, 'v')}, out);
    executeCommand(ctx, {"GET", "k"}, out);
    executeCommand(ctx, {"GET", "missing"}, out);
    executeCommand(ctx, {"LPUSH", "k", "x"}, out);       // wrong type
    executeCommand(ctx, {"NOSUCH"}, out);
    ASSERT_EQ(out, "+earlier reply\r\n+OK\r\n$100\r\n" + std::string(100, 'v') + "\r\n$-1\r\n"
                   "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
                   "-ERR unknown command 'NOSUCH'\r\n");
}

TEST(test_dispatch_quit_closes_connection) {
    InMemoryStorage s(10);
    CommandContext ctx{s};
    ASSERT_EQ(executeCommand(ctx, {"PING"}), "+PONG\r\n");
    ASSERT_FALSE(ctx.close_connection);
    ASSERT_EQ(executeCommand(ctx, {"quit"}), "+OK\r\n");
    ASSERT_TRUE(ctx.close_connection);
}

TEST(test_dispatch_set_options) {
    InMemoryStorage s(10);
    CommandContext ctx{s};
    int64_t now = InMemoryStorage::unixTimeMs();
    ASSERT_EQ(executeCommand(ctx, {"SET", "ex", "v", "EX", "100"}), "+OK\r\n");
    ASSERT_EQ(s.ttl("ex").value, "100");
    ASSERT_EQ(executeCommand(ctx, {"SET", "px", "v", "px", "100000"}), "+OK\r\n");  // was ignored
    ASSERT_EQ(s.ttl("px").value, "100");
    ASSERT_EQ(executeCommand(ctx, {"SET", "exat", "v", "EXAT", std::to_string(now / 1000 + 100)}),
              "+OK\r\n");
    int exat = std::stoi(s.ttl("exat").value);      // whole seconds, so 99 or 100
    ASSERT_TRUE(exat >= 99 && exat <= 100);
    ASSERT_EQ(executeCommand(ctx, {"SET", "pxat", "v", "PXAT", std::to_string(now + 100000)}),
              "+OK\r\n");
    ASSERT_EQ(s.ttl("pxat").value, "100");
    ASSERT_EQ(executeCommand(ctx, {"SET", "pxat", "v"}), "+OK\r\n");   // a plain SET clears the TTL
    ASSERT_EQ(s.ttl("pxat").value, "-1");
}

TEST(test_dispatch_set_rejects_bad_options) {
    // Each of these used to reply OK and store the key without a TTL (or,
    // with NX, overwrite it). Errors and their order are Redis's.
    InMemoryStorage s(10);
    CommandContext ctx{s};
    const std::string syntax = "-ERR syntax error\r\n";
    const std::string invalid = "-ERR invalid expire time in 'set' command\r\n";
    const std::string not_int = "-ERR value is not an integer or out of range\r\n";
    ASSERT_EQ(executeCommand(ctx, {"SET", "k", "v", "EX"}), syntax);             // no value
    ASSERT_EQ(executeCommand(ctx, {"SET", "k", "v", "NX"}), syntax);             // not supported
    ASSERT_EQ(executeCommand(ctx, {"SET", "k", "v", "EX", "10", "PX", "100"}), syntax);
    ASSERT_EQ(executeCommand(ctx, {"SET", "k", "v", "EX", "abc", "PX", "1"}), syntax);  // syntax first
    ASSERT_EQ(executeCommand(ctx, {"SET", "k", "v", "EX", "10abc"}), not_int);
    ASSERT_EQ(executeCommand(ctx, {"SET", "k", "v", "EX", "0"}), invalid);
    ASSERT_EQ(executeCommand(ctx, {"SET", "k", "v", "PX", "-5"}), invalid);
    ASSERT_EQ(executeCommand(ctx, {"SET", "k", "v", "PXAT", "0"}), invalid);
    ASSERT_EQ(executeCommand(ctx, {"SET", "k", "v", "EX", "9223372036854775"}), invalid);  // overflows
    ASSERT_FALSE(s.exists("k").success);        // nothing was stored
}

TEST(test_dispatch_strict_integer_arguments) {
    InMemoryStorage s(10);
    CommandContext ctx{s};
    const std::string not_int = "-ERR value is not an integer or out of range\r\n";
    // SETEX: a TTL of 0 used to store the key without one; std::stoi read "10abc" as 10
    ASSERT_EQ(executeCommand(ctx, {"SETEX", "k", "0", "v"}),
              "-ERR invalid expire time in 'setex' command\r\n");
    ASSERT_EQ(executeCommand(ctx, {"SETEX", "k", "10abc", "v"}), not_int);
    ASSERT_FALSE(s.exists("k").success);
    ASSERT_EQ(executeCommand(ctx, {"SETEX", "k", "100", "v"}), "+OK\r\n");
    ASSERT_EQ(s.ttl("k").value, "100");

    ASSERT_EQ(executeCommand(ctx, {"EXPIRE", "k", "10abc"}), not_int);
    ASSERT_EQ(s.ttl("k").value, "100");
    ASSERT_EQ(executeCommand(ctx, {"EXPIRE", "k", "9223372036854775807"}),
              "-ERR invalid expire time in 'expire' command\r\n");
    ASSERT_EQ(executeCommand(ctx, {"EXPIRE", "k", "3000000000"}), ":1\r\n");    // over INT_MAX
    ASSERT_EQ(executeCommand(ctx, {"EXPIRE", "k", "-1"}), ":1\r\n");   // in the past: gone at once
    ASSERT_FALSE(s.exists("k").success);

    executeCommand(ctx, {"RPUSH", "l", "a", "b"});
    ASSERT_EQ(executeCommand(ctx, {"LRANGE", "l", "0", "1x"}), not_int);
    ASSERT_EQ(executeCommand(ctx, {"LRANGE", "l", "0", "99999999999"}),     // past INT_MAX
              "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
}

TEST(test_dispatch_extreme_expiry_times) {
    // Clients choose these numbers. Converting them to the monotonic clock
    // saturates instead of overflowing (undefined behaviour, which UBSan
    // reports), and TTL can be far more than INT_MAX seconds.
    InMemoryStorage s(10);
    CommandContext ctx{s};
    ASSERT_EQ(executeCommand(ctx, {"SET", "far", "v", "PXAT", "9223372036854775807"}), "+OK\r\n");
    std::string ttl = executeCommand(ctx, {"TTL", "far"});
    ASSERT_EQ(ttl[0], ':');
    ASSERT_TRUE(std::stoll(ttl.substr(1)) > 9000000000000000LL);
    ASSERT_EQ(executeCommand(ctx, {"SET", "past", "v", "PXAT", "1"}), "+OK\r\n");
    ASSERT_FALSE(s.get("past").success);
    ASSERT_EQ(s.dumpState().size(), (size_t)1);     // converts "far" back to Unix time

    const std::string not_int = "-ERR value is not an integer or out of range\r\n";
    executeCommand(ctx, {"SET", "k", "v"});
    ASSERT_EQ(executeCommand(ctx, {"PEXPIREAT", "k", "9223372036854775807"}), ":1\r\n");
    ASSERT_EQ(executeCommand(ctx, {"PEXPIREAT", "k", "10abc"}), not_int);
    ASSERT_EQ(executeCommand(ctx, {"PEXPIREAT", "k", "9223372036854775808"}), not_int);  // overflows
    ASSERT_EQ(executeCommand(ctx, {"PEXPIREAT", "k", "-9223372036854775808"}), ":1\r\n");
    ASSERT_FALSE(s.exists("k").success);    // expired, and no overflow on the way
}

TEST(test_dispatch_write_flags) {
    // Replay runs only commands flagged as writes, so the flags decide what an AOF
    // can contain: every command that changes data needs one, no read may have one
    for(const char* name : {"SET", "SETEX", "DEL", "EXPIRE", "PEXPIREAT", "LPUSH", "RPUSH",
                            "LPOP", "RPOP", "SADD", "SREM", "HSET", "HDEL"}) {
        ASSERT_TRUE(lookupCommand(name) && (lookupCommand(name)->flags & kCmdWrite));
    }
    for(const char* name : {"GET", "EXISTS", "TYPE", "TTL", "LRANGE", "LLEN", "SMEMBERS",
                            "SISMEMBER", "SCARD", "HGET", "HGETALL", "HLEN", "PING", "INFO",
                            "BGREWRITEAOF"}) {
        ASSERT_TRUE(lookupCommand(name) && !(lookupCommand(name)->flags & kCmdWrite));
    }
}

// ==================== Pollers ====================

#ifndef _WIN32
// The same checks for every poller: the event loop relies on each behaving
// alike, level-triggered (a ready socket is reported until it is drained)
void checkPoller(Poller& poller) {
    int fds[2];
    ASSERT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    std::vector<Poller::Event> ready;
    poller.add(fds[0]);
    ASSERT_EQ(poller.wait(0, ready), 0);            // nothing to read; writing wasn't asked about

    ASSERT_TRUE(write(fds[1], "x", 1) == 1);
    ASSERT_EQ(poller.wait(100, ready), 1);
    ASSERT_TRUE(ready[0].fd == fds[0] && ready[0].readable && !ready[0].writable);
    ASSERT_EQ(poller.wait(0, ready), 1);            // level-triggered: still there until read
    char c;
    ASSERT_TRUE(read(fds[0], &c, 1) == 1);
    ASSERT_EQ(poller.wait(0, ready), 0);

    poller.setInterest(fds[0], false, true);        // only "can it take more data?"
    ASSERT_EQ(poller.wait(100, ready), 1);
    ASSERT_TRUE(ready[0].writable && !ready[0].readable);
    poller.setInterest(fds[0], true, false);

    close(fds[1]);                                  // the peer hangs up: end of stream
    ASSERT_EQ(poller.wait(100, ready), 1);
    ASSERT_TRUE(ready[0].readable);                 // the next recv() returns 0

    poller.remove(fds[0]);
    ASSERT_EQ(poller.wait(0, ready), 0);
    close(fds[0]);
}
#endif

TEST(test_poller_select) {
#ifndef _WIN32
    SelectPoller poller;
    ASSERT_TRUE(poller.canWatch(FD_SETSIZE - 1) && !poller.canWatch(FD_SETSIZE));
    checkPoller(poller);
#endif
}

TEST(test_poller_epoll) {
#ifdef __linux__
    EpollPoller poller;
    ASSERT_TRUE(poller.isOpen());
    ASSERT_TRUE(poller.canWatch(FD_SETSIZE));       // any socket number
    checkPoller(poller);
    ASSERT_TRUE(makePoller("epoll") != nullptr);
#else
    ASSERT_TRUE(makePoller("epoll") == nullptr);    // only on Linux
#endif
    ASSERT_TRUE(makePoller("select") != nullptr);
    ASSERT_TRUE(makePoller("kqueue") == nullptr);
}

// ==================== RESP Parser ====================

using ParseStatus = RespParser::ParseStatus;

// The reason the parser gives for rejecting `input`; fails the test if it doesn't.
std::string protocolErrorOf(const std::string& input) {
    auto parsed = RespParser::parse(input);
    if(parsed.status != ParseStatus::Error) {
        throw std::runtime_error("expected a protocol error for: " + input.substr(0, 40));
    }
    return parsed.error;
}

TEST(test_resp_parse_array) {
    // *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nmadhav\r\n
    std::string input = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nmadhav\r\n";
    auto parsed = RespParser::parse(input);
    ASSERT_TRUE(parsed.status == ParseStatus::Ok);
    ASSERT_EQ(parsed.args.size(), (size_t)3);
    ASSERT_EQ(parsed.args[0], "SET");
    ASSERT_EQ(parsed.args[1], "name");
    ASSERT_EQ(parsed.args[2], "madhav");
    ASSERT_EQ(parsed.consumed, input.size());
}

TEST(test_resp_parse_inline) {
    std::string input = "PING\r\n";
    auto parsed = RespParser::parse(input);
    ASSERT_TRUE(parsed.status == ParseStatus::Ok);
    ASSERT_EQ(parsed.args.size(), (size_t)1);
    ASSERT_EQ(parsed.args[0], "PING");
    ASSERT_EQ(parsed.consumed, input.size());
}

TEST(test_resp_parse_incomplete) {
    // Half a RESP command: valid so far, so wait for more data
    std::string input = "*3\r\n$3\r\nSET\r\n$4\r\n";
    auto parsed = RespParser::parse(input);
    ASSERT_TRUE(parsed.status == ParseStatus::NeedMore);
    ASSERT_EQ(parsed.consumed, (size_t)0);
    ASSERT_TRUE(parsed.args.empty());
}

TEST(test_resp_parse_into_reuses_argument_strings) {
    // The server parses every command into the same strings, so parsing
    // allocates nothing once they are large enough. A 16-byte key is too long
    // for the small-string buffer: the same heap buffer must be reused.
    std::vector<std::string> args;
    auto first = RespParser::parseInto(RespParser::array({"SET", "key:000000000001", "v"}), args);
    ASSERT_TRUE(first.status == RespParser::ParseStatus::Ok);
    ASSERT_EQ(first.argc, (size_t)3);
    const char* key_buffer = args[1].data();
    auto second = RespParser::parseInto(RespParser::array({"SET", "key:000000000002", "w"}), args);
    ASSERT_EQ(second.argc, (size_t)3);
    ASSERT_EQ(args[1], "key:000000000002");
    ASSERT_EQ(args[2], "w");
    ASSERT_TRUE(args[1].data() == key_buffer);          // no new allocation

    // A shorter command: argc says how many are its own; the rest are kept
    auto get = RespParser::parseInto(RespParser::array({"GET", "k"}), args);
    ASSERT_EQ(get.argc, (size_t)2);
    ASSERT_EQ(args[0], "GET");
    ASSERT_EQ(args[1], "k");
    ASSERT_EQ(args.size(), (size_t)3);

    // Inline commands go into the same strings
    auto inline_cmd = RespParser::parseInto("RPUSH list a b c\r\n", args);
    ASSERT_EQ(inline_cmd.argc, (size_t)5);
    ASSERT_EQ(args[4], "c");
    ASSERT_EQ(inline_cmd.consumed, (size_t)18);
}

TEST(test_resp_parse_multiple_commands) {
    // Two commands in one buffer (common with TCP pipelining)
    std::string input = "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$4\r\nname\r\n";
    auto first = RespParser::parse(input);
    ASSERT_TRUE(first.status == ParseStatus::Ok);
    ASSERT_EQ(first.args[0], "PING");

    auto second = RespParser::parse(std::string_view(input).substr(first.consumed));
    ASSERT_TRUE(second.status == ParseStatus::Ok);
    ASSERT_EQ(second.args[0], "GET");
    ASSERT_EQ(second.args[1], "name");
    ASSERT_EQ(first.consumed + second.consumed, input.size());
}

TEST(test_resp_empty_array_is_skipped) {
    // "*0" and "*-1" carry no command; they are consumed and ignored, as in Redis
    auto parsed = RespParser::parse("*0\r\n");
    ASSERT_TRUE(parsed.status == ParseStatus::Ok);
    ASSERT_TRUE(parsed.args.empty());
    ASSERT_EQ(parsed.consumed, (size_t)4);
    ASSERT_EQ(RespParser::parse("*-1\r\n").consumed, (size_t)5);
}

TEST(test_resp_rejects_malformed_input) {
    // These used to look "incomplete", so the server waited forever,
    // or (std::stoi reading "3abc" as 3) were accepted.
    ASSERT_EQ(protocolErrorOf("*abc\r\n"), "invalid multibulk length");
    ASSERT_EQ(protocolErrorOf("*3abc\r\n"), "invalid multibulk length");
    ASSERT_EQ(protocolErrorOf("*1\r\n$-5\r\n"), "invalid bulk length");
    ASSERT_EQ(protocolErrorOf("*1\r\n$3abc\r\nfoo\r\n"), "invalid bulk length");
    ASSERT_EQ(protocolErrorOf("*1\r\n$+3\r\nfoo\r\n"), "invalid bulk length");
    ASSERT_EQ(protocolErrorOf("*1\r\n$\r\n"), "invalid bulk length");
    ASSERT_EQ(protocolErrorOf("*1\r\n:3\r\n"), "expected '$', got ':'");
    ASSERT_EQ(protocolErrorOf("*1\r\n\r\n"), "expected '$', got '\\x0d'");
    ASSERT_EQ(protocolErrorOf("*1\r\n$3\r\nfooXY"), "bulk string not followed by CRLF");
}

TEST(test_resp_rejects_oversized_input) {
    // A huge declared length is refused at once instead of waited for
    ASSERT_EQ(protocolErrorOf("*1\r\n$2000000000\r\n"), "invalid bulk length");
    ASSERT_EQ(protocolErrorOf("*1\r\n$536870913\r\n"), "invalid bulk length");     // 512 MB + 1
    ASSERT_TRUE(RespParser::parse("*1\r\n$536870912\r\n").status == ParseStatus::NeedMore);
    ASSERT_EQ(protocolErrorOf("*" + std::string(40, '9')), "invalid multibulk length");
    ASSERT_EQ(protocolErrorOf("*1\r\n$" + std::string(40, '9')), "invalid bulk length");
    ASSERT_EQ(protocolErrorOf(std::string(100 * 1024, 'a')), "too big inline request");
}

TEST(test_resp_inline_length_limit) {
    const std::string longest(RespParser::kMaxInlineLength, 'a');
    auto parsed = RespParser::parse(longest + "\r\n");
    ASSERT_TRUE(parsed.status == ParseStatus::Ok);
    ASSERT_EQ(parsed.args.size(), (size_t)1);
    ASSERT_EQ(parsed.consumed, longest.size() + 2);
    ASSERT_TRUE(RespParser::parse(longest).status == ParseStatus::NeedMore);
    ASSERT_TRUE(RespParser::parse(longest + "\r").status == ParseStatus::NeedMore);
    ASSERT_EQ(protocolErrorOf(longest + "a\r\n"), "too big inline request");
    ASSERT_EQ(protocolErrorOf(longest + "ab"), "too big inline request");  // can't end in time
}

// Feeds `stream` to the parser the way the server receives it: append what
// arrived, parse until NeedMore, keep the rest. `cuts` are where reads end.
std::vector<std::vector<std::string>> parseInPieces(const std::string& stream,
                                                    const std::vector<size_t>& cuts) {
    std::vector<std::vector<std::string>> commands;
    std::string buffer;
    size_t fed = 0;
    auto feedUntil = [&](size_t end) {
        buffer.append(stream, fed, end - fed);
        fed = end;
        while(true) {
            auto parsed = RespParser::parse(buffer);
            if(parsed.status == ParseStatus::NeedMore) return;
            if(parsed.status == ParseStatus::Error) {
                throw std::runtime_error("protocol error after a split: " + parsed.error);
            }
            buffer.erase(0, parsed.consumed);
            if(!parsed.args.empty()) commands.push_back(parsed.args);
        }
    };
    for(size_t cut : cuts) feedUntil(cut);
    feedUntil(stream.size());
    if(!buffer.empty()) throw std::runtime_error("bytes left unparsed after a split");
    return commands;
}

TEST(test_resp_every_split_point) {
    // However TCP splits this stream between reads, the commands must be the same.
    // It has arguments containing "\r\n", an empty argument, inline commands with
    // both line endings, a blank line and an empty array.
    const std::string stream =
        "*3\r\n$3\r\nSET\r\n$6\r\nkey\r\n1\r\n$0\r\n\r\n"
        "PING\r\n"
        "\r\n"
        "*0\r\n"
        "*2\r\n$3\r\nGET\r\n$6\r\nkey\r\n1\r\n"
        "ECHO  hello   world\n";
    const std::vector<std::vector<std::string>> expected = {
        {"SET", "key\r\n1", ""}, {"PING"}, {"GET", "key\r\n1"}, {"ECHO", "hello", "world"}};

    ASSERT_TRUE(parseInPieces(stream, {}) == expected);
    for(size_t i = 0; i <= stream.size(); i++) {                // every split into two reads
        ASSERT_TRUE(parseInPieces(stream, {i}) == expected);
        for(size_t j = i; j <= stream.size(); j++) {            // and into three
            ASSERT_TRUE(parseInPieces(stream, {i, j}) == expected);
        }
    }
    std::vector<size_t> everyByte;                              // one byte per read
    for(size_t i = 1; i < stream.size(); i++) everyByte.push_back(i);
    ASSERT_TRUE(parseInPieces(stream, everyByte) == expected);
}

TEST(test_resp_serialize) {
    ASSERT_EQ(RespParser::simpleString("OK"), "+OK\r\n");
    ASSERT_EQ(RespParser::error("ERR bad"), "-ERR bad\r\n");
    ASSERT_EQ(RespParser::error("ERR a\r\nb\nc"), "-ERR a  b c\r\n");   // stays one line
    ASSERT_EQ(RespParser::integer(42), ":42\r\n");
    ASSERT_EQ(RespParser::nullBulk(), "$-1\r\n");
    ASSERT_EQ(RespParser::bulkString("hi"), "$2\r\nhi\r\n");
}

// ==================== Main ======================================

int main() {
    std::cout << "\n========== Mini-Redis Test Suite ==========\n\n";

    std::cout << "[Basic Operations]\n";
    RUN_TEST(test_set_and_get);
    RUN_TEST(test_get_nonexistent_key);
    RUN_TEST(test_overwrite_existing_key);
    RUN_TEST(test_delete_key);
    RUN_TEST(test_delete_nonexistent_key);
    RUN_TEST(test_exists_found);
    RUN_TEST(test_exists_not_found);

    std::cout << "\n[LRU Eviction]\n";
    RUN_TEST(test_lru_eviction_basic);
    RUN_TEST(test_lru_refresh_with_get);
    RUN_TEST(test_exists_does_not_refresh_lru);
    RUN_TEST(test_lru_capacity_one);
    RUN_TEST(test_capacity_zero_is_rejected);
    RUN_TEST(test_table_sized_for_capacity);
    RUN_TEST(test_table_presize_is_capped);

    std::cout << "\n[Key Table]\n";
    RUN_TEST(test_keytable_basics);
    RUN_TEST(test_keytable_one_long_chain);
    RUN_TEST(test_keytable_matches_unordered_map);
    RUN_TEST(test_keytable_rehash_is_incremental);

    std::cout << "\n[SIEVE Eviction]\n";
    RUN_TEST(test_sieve_evicts_first_unvisited_key);
    RUN_TEST(test_sieve_differs_from_lru);
    RUN_TEST(test_sieve_hand_moves_off_a_deleted_key);
    RUN_TEST(test_eviction_matches_a_model);
    RUN_TEST(test_load_file_with_a_key_twice);

    std::cout << "\n[TTL Expiration]\n";
    RUN_TEST(test_ttl_expiration);
    RUN_TEST(test_ttl_check_remaining);
    RUN_TEST(test_ttl_no_expiry);
    RUN_TEST(test_expire_existing_key);
    RUN_TEST(test_exists_removes_expired_key);

    std::cout << "\n[TTL > LRU Priority]\n";
    RUN_TEST(test_ttl_evicted_before_lru);
    RUN_TEST(test_expired_keys_evicted_before_live_ones);

    std::cout << "\n[Persistence]\n";
    RUN_TEST(test_save_and_load);
    RUN_TEST(test_load_enforces_capacity);
    RUN_TEST(test_save_skips_expired_keys);
    RUN_TEST(test_load_nonexistent_file);

    std::cout << "\n[Controller Validation]\n";
    RUN_TEST(test_controller_empty_key);

    std::cout << "\n[List Operations]\n";
    RUN_TEST(test_lpush_and_lrange);
    RUN_TEST(test_rpush_and_lrange);
    RUN_TEST(test_lpop_and_rpop);
    RUN_TEST(test_lpop_empty_list_deletes_key);
    RUN_TEST(test_lrange_negative_indices);
    RUN_TEST(test_llen);

    std::cout << "\n[Set Operations]\n";
    RUN_TEST(test_sadd_and_smembers);
    RUN_TEST(test_sadd_duplicates);
    RUN_TEST(test_srem);
    RUN_TEST(test_sismember);
    RUN_TEST(test_srem_empty_set_deletes_key);

    std::cout << "\n[Hash Operations]\n";
    RUN_TEST(test_hset_and_hget);
    RUN_TEST(test_hset_overwrite_field);
    RUN_TEST(test_hdel);
    RUN_TEST(test_hgetall);
    RUN_TEST(test_hget_missing_field);

    std::cout << "\n[Type Safety]\n";
    RUN_TEST(test_type_command);
    RUN_TEST(test_wrong_type_get_on_list);
    RUN_TEST(test_wrong_type_lpush_on_string);
    RUN_TEST(test_set_overwrites_any_type);

    std::cout << "\n[LRU with Mixed Types]\n";
    RUN_TEST(test_lru_evicts_across_types);

    std::cout << "\n[AOF Persistence]\n";
    RUN_TEST(test_aof_basic_replay);
    RUN_TEST(test_aof_all_types);
    RUN_TEST(test_aof_rewrite_compaction);
    RUN_TEST(test_aof_rewrite_failure_keeps_old_file);
    RUN_TEST(test_aof_failed_writes_wait_and_are_retried);
    RUN_TEST(test_aof_rewrite_replaces_failed_writes);
    RUN_TEST(test_aof_deferred_writes_wait_for_flush);
    RUN_TEST(test_aof_deferred_write_errors);
    RUN_TEST(test_aof_rewrite_drops_buffered_records);
    RUN_TEST(test_aof_fsync_always_syncs_each_write_or_group);
    RUN_TEST(test_aof_fsync_everysec_syncs_in_the_background);
    RUN_TEST(test_aof_fsync_always_write_error_is_fatal);
    RUN_TEST(test_aof_no_log_on_failed_ops);
    RUN_TEST(test_aof_rewrite_preserves_ttl);
    RUN_TEST(test_aof_logs_absolute_expiry_times);
    RUN_TEST(test_aof_expiry_is_absolute);
    RUN_TEST(test_aof_old_relative_records_still_load);
    RUN_TEST(test_aof_rewrite_keeps_ttl_under_a_second);
    RUN_TEST(test_aof_eviction_is_logged);
    RUN_TEST(test_aof_lazy_expiry_is_logged);
    RUN_TEST(test_aof_replay_into_smaller_capacity);
    RUN_TEST(test_aof_replay_matches_live_randomized);
    RUN_TEST(test_aof_replay_uses_command_table);
    RUN_TEST(test_aof_replay_cuts_incomplete_last_record);
    RUN_TEST(test_aof_replay_refuses_damage_before_the_end);
    RUN_TEST(test_aof_values_with_spaces);

    std::cout << "\n[Command Table]\n";
    RUN_TEST(test_dispatch_is_case_insensitive);
    RUN_TEST(test_dispatch_checks_arity);
    RUN_TEST(test_dispatch_unknown_command);
    RUN_TEST(test_dispatch_replies);
    RUN_TEST(test_dispatch_appends_replies);
    RUN_TEST(test_dispatch_quit_closes_connection);
    RUN_TEST(test_dispatch_set_options);
    RUN_TEST(test_dispatch_set_rejects_bad_options);
    RUN_TEST(test_dispatch_strict_integer_arguments);
    RUN_TEST(test_dispatch_extreme_expiry_times);
    RUN_TEST(test_dispatch_write_flags);

    std::cout << "\n[Pollers]\n";
    RUN_TEST(test_poller_select);
    RUN_TEST(test_poller_epoll);

    std::cout << "\n[RESP Parser]\n";
    RUN_TEST(test_resp_parse_array);
    RUN_TEST(test_resp_parse_inline);
    RUN_TEST(test_resp_parse_incomplete);
    RUN_TEST(test_resp_parse_into_reuses_argument_strings);
    RUN_TEST(test_resp_parse_multiple_commands);
    RUN_TEST(test_resp_empty_array_is_skipped);
    RUN_TEST(test_resp_rejects_malformed_input);
    RUN_TEST(test_resp_rejects_oversized_input);
    RUN_TEST(test_resp_inline_length_limit);
    RUN_TEST(test_resp_every_split_point);
    RUN_TEST(test_resp_serialize);

    std::cout << "\n==========================================\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
    std::cout << "==========================================\n\n";

    return failed > 0 ? 1 : 0;
}
