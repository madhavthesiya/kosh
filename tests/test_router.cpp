// Router: which shard owns a key, and the command table's record of where keys are.

#include "../src/AofManifest.h"
#include "../src/Commands.h"
#include "../src/Router.h"

#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if(!ok) {
        std::cout << "FAILED: " << what << "\n";
        failures++;
    }
}

// Redis Cluster's examples, from its specification
void testHashTags() {
    check(Router::routingPart("{user1000}.following") == "user1000", "a tag at the start");
    check(Router::routingPart("foo{bar}{zap}") == "bar", "only the first tag");
    check(Router::routingPart("foo{}{bar}") == "foo{}{bar}", "an empty first tag means the whole key");
    check(Router::routingPart("foo{{bar}}zap") == "{bar", "the first { to the first } after it");
    check(Router::routingPart("nobraces") == "nobraces", "no tag");
    check(Router::routingPart("only{open") == "only{open", "no closing brace");
    check(Router::routingPart("only}close{") == "only}close{", "the closing brace must follow");
    check(Router::routingPart("") == "", "the empty key");
}

void testRange() {
    Router router(8);
    for(int i = 0; i < 20000; i++) check(router.shardOf("key" + std::to_string(i)) < 8, "shard in range");
    Router one(1);
    check(one.shardOf("anything") == 0, "one shard owns everything");
}

void testSameTagSameShard() {
    Router router(16);
    for(int i = 0; i < 2000; i++) {
        const std::string tag = "t" + std::to_string(i);
        const size_t shard = router.shardOf("{" + tag + "}:a");
        check(router.shardOf("{" + tag + "}:b") == shard, "keys with one tag share a shard");
        check(router.shardOf("x{" + tag + "}y") == shard, "wherever the tag is");
    }
}

void testEvenSpread() {
    Router router(8);
    std::vector<int> counts(8);
    const int keys = 400000;
    for(int i = 0; i < keys; i++) counts[router.shardOf("key:" + std::to_string(i))]++;
    // Binomial: the standard deviation is about 209 keys, so 1,500 is over 7 of them
    for(int count : counts) check(count > keys / 8 - 1500 && count < keys / 8 + 1500, "keys spread evenly: " + std::to_string(count));
}

void testKeyedPerRouter() {
    Router a(1024), b(1024);
    int same = 0;
    for(int i = 0; i < 1000; i++) if(a.shardOf("k" + std::to_string(i)) == b.shardOf("k" + std::to_string(i))) same++;
    check(same < 20, "two routers use different keys: " + std::to_string(same) + " of 1000 agree");
}

// The reason routing has its own key: the keys of one shard must still spread
// over all of a table's buckets, which are chosen by the hash's low bits
void testShardKeysFillTheTable() {
    Router router(4);
    KeyedHash table{SipKey::random()};
    std::set<size_t> buckets;
    for(int i = 0; i < 40000; i++) {
        const std::string key = "key" + std::to_string(i);
        if(router.shardOf(key) == 0) buckets.insert(table(key) & 255);
    }
    check(buckets.size() == 256, "one shard's keys reach every bucket: " + std::to_string(buckets.size()) + " of 256");
}

// A command's place in the table decides where it runs, so a wrong entry sends
// it to the wrong shard. Every write must have keys or be a whole-server one.
void testCommandTable() {
    for(const Command* cmd : allCommands()) {
        const std::string name = cmd->name;
        if(cmd->flags & kCmdWrite) {
            check(cmd->first_key > 0 || (cmd->flags & kCmdWholeServer), name + ": a write says which keys it touches");
        }
        if(cmd->first_key > 0) {
            check(cmd->key_step >= 1, name + ": a key step");
            check(cmd->last_key == -1 || cmd->last_key >= cmd->first_key, name + ": a sensible last key");
            check(cmd->arity < 0 ? -cmd->arity > cmd->first_key : cmd->arity > cmd->first_key,
                  name + ": its arity leaves room for the first key");
            check(!(cmd->flags & kCmdWholeServer), name + ": not both keyed and whole-server");
        }
    }
    for(const char* name : {"ping", "auth", "hello", "quit", "exec", "discard", "command", "client", "config"}) {
        check(lookupCommand(name)->first_key == 0, std::string(name) + " has no keys");
    }
    for(const char* name : {"get", "set", "setex", "incr", "expire", "ttl", "lpush", "sadd", "hset", "hget", "type", "persist"}) {
        check(lookupCommand(name)->first_key == 1 && lookupCommand(name)->last_key == 1, std::string(name) + " has one key, first");
    }
    for(const char* name : {"mget", "del", "exists"}) {
        const Command* cmd = lookupCommand(name);
        check(cmd->first_key == 1 && cmd->last_key == -1 && cmd->key_step == 1, std::string(name) + ": every argument is a key");
    }
    const Command* mset = lookupCommand("mset");
    check(mset->first_key == 1 && mset->last_key == -1 && mset->key_step == 2, "mset: every other argument is a key");
    for(const char* name : {"dbsize", "scan", "flushall", "flushdb", "bgrewriteaof", "aofrewrite"}) {
        check(lookupCommand(name)->flags & kCmdWholeServer, std::string(name) + " is a whole-server command");
    }
}

void testManifest() {
    SipKey key;
    key.k0 = 0x0123456789abcdefull;
    key.k1 = 0xfedcba9876543210ull;
    const std::string text = formatManifest(4, key);
    size_t shards = 0;
    SipKey parsed;
    check(parseManifest(text, shards, parsed), "a manifest parses");
    check(shards == 4 && parsed.k0 == key.k0 && parsed.k1 == key.k1, "and says what was written");
    for(const std::string& bad : {std::string(""), std::string("nonsense\n"), text.substr(0, text.size() - 1),
                                  text + "x", "kosh-aof 2" + text.substr(10), formatManifest(1, key), formatManifest(257, key)}) {
        check(!parseManifest(bad, shards, parsed), "a damaged manifest is refused: " + bad);
    }
}

}  // namespace

int main() {
    testHashTags();
    testRange();
    testSameTagSameShard();
    testEvenSpread();
    testKeyedPerRouter();
    testShardKeysFillTheTable();
    testCommandTable();
    testManifest();
    if(failures == 0) {
        std::cout << "Router: all checks passed\n";
        return 0;
    }
    std::cout << "Router: " << failures << " checks FAILED\n";
    return 1;
}
