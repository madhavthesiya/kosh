#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <cstdio>
#include <fstream>
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
        aof.rewrite(commands);

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
        aof.rewrite(commands);

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
    const std::string aof_file = "test_aof_absolute.aof";
    std::remove(aof_file.c_str());
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        s.set("setex", "v", 1);
        s.set("expire", "v");
        s.expire("expire", 1);
        s.set("extended", "v", 1);      // its first TTL passes before the restart,
        s.expire("extended", 100);      // but a later record extends it
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
        aof.rewrite(s.dumpState());
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

TEST(test_aof_replay_uses_command_table) {
    // Replay runs records through the same table as client commands, so names
    // are case-insensitive for both. The old replay compared names
    // case-sensitively and silently skipped "set". Only write commands run.
    const std::string aof_file = "test_aof_table.aof";
    writeAof(aof_file, {{"set", "a", "1"}, {"Rpush", "l", "x", "y"}, {"GET", "a"},
                        {"NOSUCH", "b"}, {"SET", "b"}});
    {
        AOFLogger aof(aof_file);
        InMemoryStorage s(10, &aof);
        replayAOF(s, aof);
        ASSERT_EQ(s.get("a").value, "1");
        ASSERT_EQ(s.llen("l").value, "2");
        ASSERT_FALSE(s.exists("b").success);    // unknown command, wrong arity: skipped
        ASSERT_EQ(countRespCommands(aof.readRaw()), (size_t)5);   // nothing logged again
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

TEST(test_dispatch_quit_closes_connection) {
    InMemoryStorage s(10);
    CommandContext ctx{s};
    ASSERT_EQ(executeCommand(ctx, {"PING"}), "+PONG\r\n");
    ASSERT_FALSE(ctx.close_connection);
    ASSERT_EQ(executeCommand(ctx, {"quit"}), "+OK\r\n");
    ASSERT_TRUE(ctx.close_connection);
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
    ASSERT_EQ(executeCommand(ctx, {"SET", "past", "v", "PXAT", "-9223372036854775808"}), "+OK\r\n");
    ASSERT_FALSE(s.get("past").success);
    ASSERT_EQ(s.dumpState().size(), (size_t)1);     // converts "far" back to Unix time

    executeCommand(ctx, {"SET", "k", "v"});
    ASSERT_EQ(executeCommand(ctx, {"PEXPIREAT", "k", "9223372036854775807"}), ":1\r\n");
    ASSERT_EQ(executeCommand(ctx, {"PEXPIREAT", "k", "10abc"}), "-ERR value is not an integer\r\n");
    ASSERT_EQ(executeCommand(ctx, {"PEXPIREAT", "k", "9223372036854775808"}),    // overflows
              "-ERR value is not an integer\r\n");
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

    std::cout << "\n[TTL Expiration]\n";
    RUN_TEST(test_ttl_expiration);
    RUN_TEST(test_ttl_check_remaining);
    RUN_TEST(test_ttl_no_expiry);
    RUN_TEST(test_expire_existing_key);
    RUN_TEST(test_exists_removes_expired_key);

    std::cout << "\n[TTL > LRU Priority]\n";
    RUN_TEST(test_ttl_evicted_before_lru);

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
    RUN_TEST(test_aof_no_log_on_failed_ops);
    RUN_TEST(test_aof_rewrite_preserves_ttl);
    RUN_TEST(test_aof_logs_absolute_expiry_times);
    RUN_TEST(test_aof_expiry_is_absolute);
    RUN_TEST(test_aof_old_relative_records_still_load);
    RUN_TEST(test_aof_rewrite_keeps_ttl_under_a_second);
    RUN_TEST(test_aof_replay_uses_command_table);
    RUN_TEST(test_aof_values_with_spaces);

    std::cout << "\n[Command Table]\n";
    RUN_TEST(test_dispatch_is_case_insensitive);
    RUN_TEST(test_dispatch_checks_arity);
    RUN_TEST(test_dispatch_unknown_command);
    RUN_TEST(test_dispatch_replies);
    RUN_TEST(test_dispatch_quit_closes_connection);
    RUN_TEST(test_dispatch_extreme_expiry_times);
    RUN_TEST(test_dispatch_write_flags);

    std::cout << "\n[RESP Parser]\n";
    RUN_TEST(test_resp_parse_array);
    RUN_TEST(test_resp_parse_inline);
    RUN_TEST(test_resp_parse_incomplete);
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
