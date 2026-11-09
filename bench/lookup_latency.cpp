// How long one lookup takes in the storage engine, and how that grows as the
// table outgrows the CPU's caches.
//
// Times GET and SET (of an existing key) through InMemoryStorage, and find() in
// a plain std::unordered_map whose entries are as large as the engine's, for
// tables of 1,000 to 4,000,000 keys, with uniform and Zipf-distributed keys.
// No network and no parser: only the engine. WSL2 exposes no hardware
// counters, so cache misses can't be counted; their effect shows instead as
// the knee where the table stops fitting in the cache.
//
// Since C3c the engine hashes with SipHash-1-3 under a random key, while the
// plain std::unordered_map and KeyTable here still use std::hash, as in the
// earlier results; hash_flood.cpp measures what the keyed hash costs.
//
// The lookups run two ways:
// - independent: every key is known in advance, so the CPU can overlap the
//   cache misses of neighbouring lookups (memory-level parallelism), as it
//   can for the commands of a pipeline;
// - one at a time: each lookup's key depends on the previous lookup's result,
//   so their misses can't overlap. This is the full latency of one lookup.
//
// Build: cmake --build build-bench --target lookup_latency
// Run:   taskset -c 2 ./build-bench/lookup_latency [--sizes 1000,100000] [--ops N]
//            [--repeats R] [--dist uniform|zipf|both] [--alpha A] [--eviction lru|sieve|both]
//
// With --eviction both, one engine per policy holds the same keys, and the
// measurements of both are interleaved within each repeat, so they compare
// in the same binary, at the same moment.

#include "../src/InMemoryStorage.h"
#include "../src/KeyTable.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
#define HAVE_MALLINFO2 1
#endif

namespace {

using Clock = std::chrono::steady_clock;

// "key:" and a 12-digit number: the keys redis-benchmark sends with -r
constexpr size_t kKeyLen = 16;

void formatKey(char* out, size_t id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key:%012zu", id);
    std::memcpy(out, buf, kKeyLen);
}

std::string keyString(size_t id) {
    std::string key(kKeyLen, ' ');
    formatKey(key.data(), id);
    return key;
}

// The results go here, so the compiler can't drop the lookups as unused
volatile size_t g_sink = 0;

struct Options {
    std::vector<size_t> sizes{1000, 10000, 30000, 100000, 300000, 1000000, 4000000};
    size_t ops = 2000000;
    int repeats = 5;
    bool uniform = true;
    bool zipf = true;
    double alpha = 0.99;    // YCSB's default Zipf constant
    std::vector<EvictionPolicy> policies{EvictionPolicy::LRU};
};

const char* policyName(EvictionPolicy p) {
    return p == EvictionPolicy::SIEVE ? "sieve" : "lru";
}

// Key ids, uniform over [0, n)
std::vector<uint32_t> uniformIds(size_t n, size_t ops, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint32_t> pick(0, static_cast<uint32_t>(n - 1));
    std::vector<uint32_t> ids(ops);
    for(auto& id : ids) id = pick(rng);
    return ids;
}

// Key ids with Zipf-distributed popularity: rank r is drawn with probability
// proportional to 1 / (r + 1)^alpha. A random permutation then maps ranks to
// ids (YCSB's "scrambled" Zipf), so the hottest keys are spread over the table
// and over memory, as in a real cache, instead of being neighbours.
std::vector<uint32_t> zipfIds(size_t n, size_t ops, double alpha, uint64_t seed) {
    std::vector<double> cdf(n);
    double sum = 0;
    for(size_t r = 0; r < n; r++) {
        sum += 1.0 / std::pow(static_cast<double>(r + 1), alpha);
        cdf[r] = sum;
    }
    std::vector<uint32_t> id_of_rank(n);
    std::iota(id_of_rank.begin(), id_of_rank.end(), 0);
    std::mt19937_64 rng(seed);
    std::shuffle(id_of_rank.begin(), id_of_rank.end(), rng);

    std::uniform_real_distribution<double> u(0.0, sum);
    std::vector<uint32_t> ids(ops);
    for(auto& id : ids) {
        size_t rank = static_cast<size_t>(std::lower_bound(cdf.begin(), cdf.end(), u(rng)) - cdf.begin());
        id = id_of_rank[std::min(rank, n - 1)];
    }
    return ids;
}

// The keys of a run, formatted in advance into one contiguous array. The timed
// loop reads it in order, which the hardware prefetcher keeps ahead of, so the
// only random accesses left are the lookups' own.
std::vector<char> formatKeys(const std::vector<uint32_t>& ids) {
    std::vector<char> keys(ids.size() * kKeyLen);
    for(size_t i = 0; i < ids.size(); i++) formatKey(&keys[i * kKeyLen], ids[i]);
    return keys;
}

// ns per lookup over `ops` lookups. `lookup(key)` returns a value read from
// the entry it found. The key goes into one reused string, as the server's
// argument strings are reused, so copying it allocates nothing.
//
// One at a time: the next key's address is computed from the previous
// result. That `carry` is always 0 (no stored value is 2^62 bytes long), but
// the CPU only knows it once the previous lookup has finished, and it is a
// data dependency, not a branch, so speculation can't run ahead of it.
template<typename Lookup>
double nsPerLookup(const std::vector<char>& keys, bool one_at_a_time, Lookup&& lookup) {
    const size_t ops = keys.size() / kKeyLen;
    const char* base = keys.data();
    std::string key(kKeyLen, ' ');
    size_t sink = 0;
    auto start = Clock::now();
    if(one_at_a_time) {
        size_t carry = 0;
        for(size_t i = 0; i < ops; i++) {
            key.assign(base + (i + carry) * kKeyLen, kKeyLen);
            size_t r = lookup(key);
            sink += r;
            carry = r >> 62;
        }
    } else {
        for(size_t i = 0; i < ops; i++) {
            key.assign(base + i * kKeyLen, kKeyLen);
            sink += lookup(key);
        }
    }
    double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    g_sink = g_sink + sink;
    return ns / static_cast<double>(ops);
}

// An entry as large as the server's CacheEntry (112 bytes with GCC 13)
struct FakeEntry {
    char bytes[112];
};

// An entry as KeyTable holds them: a fixed part, then the key's bytes, in one
// allocation (one block per key, where std::unordered_map needs a node and,
// for a 16-byte key, a heap copy of the key)
struct InlineEntry {
    char fixed[72];
    uint32_t key_len;
    std::string_view key() const { return {reinterpret_cast<const char*>(this + 1), key_len}; }
    static InlineEntry* create(std::string_view key) {
        void* block = ::operator new(sizeof(InlineEntry) + key.size());
        auto* e = new (block) InlineEntry{};
        e->key_len = static_cast<uint32_t>(key.size());
        std::memcpy(e + 1, key.data(), key.size());
        return e;
    }
};
struct InlineKeyOf {
    static std::string_view key(const InlineEntry& e) { return e.key(); }
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

// "median (min-max)" of the repeats, in ns
std::string summary(const std::vector<double>& v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f (%.1f-%.1f)", median(v),
                  *std::min_element(v.begin(), v.end()), *std::max_element(v.begin(), v.end()));
    return buf;
}

#ifdef HAVE_MALLINFO2
size_t heapInUse() {
    struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
}
#endif

// "1000,100000" -> {1000, 100000}. A bad entry becomes 0, which parseArgs rejects.
std::vector<size_t> parseSizes(const std::string& s) {
    std::vector<size_t> sizes;
    size_t pos = 0;
    while(pos <= s.size()) {
        size_t comma = s.find(',', pos);
        if(comma == std::string::npos) comma = s.size();
        std::string item = s.substr(pos, comma - pos);
        char* end = nullptr;
        unsigned long long n = std::strtoull(item.c_str(), &end, 10);
        sizes.push_back(item.empty() || *end != '\0' ? 0 : static_cast<size_t>(n));
        pos = comma + 1;
    }
    return sizes;
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for(int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if(a == "--sizes" && v)        { opt.sizes = parseSizes(v); i++; }
        else if(a == "--ops" && v)     { opt.ops = std::strtoull(v, nullptr, 10); i++; }
        else if(a == "--repeats" && v) { opt.repeats = std::atoi(v); i++; }
        else if(a == "--alpha" && v)   { opt.alpha = std::atof(v); i++; }
        else if(a == "--dist" && v) {
            std::string d = v;
            opt.uniform = (d == "uniform" || d == "both");
            opt.zipf = (d == "zipf" || d == "both");
            i++;
        } else if(a == "--eviction" && v) {
            std::string e = v;
            opt.policies.clear();
            if(e == "lru" || e == "both") opt.policies.push_back(EvictionPolicy::LRU);
            if(e == "sieve" || e == "both") opt.policies.push_back(EvictionPolicy::SIEVE);
            if(opt.policies.empty()) return false;
            i++;
        } else {
            std::fprintf(stderr, "usage: %s [--sizes 1000,100000] [--ops N] [--repeats R] "
                                 "[--dist uniform|zipf|both] [--alpha A] [--eviction lru|sieve|both]\n",
                         argv[0]);
            return false;
        }
    }
    for(size_t n : opt.sizes) {
        if(n == 0 || n > UINT32_MAX) { std::fprintf(stderr, "bad size %zu\n", n); return false; }
    }
    return opt.ops > 0 && opt.repeats > 0 && (opt.uniform || opt.zipf) && !opt.sizes.empty();
}

// The rows of one table size, for one key distribution: one per eviction
// policy. The plain tables don't depend on the policy, so only the first row
// has them.
struct Row {
    size_t keys;
    const char* policy;
    double engine_mb;
    std::vector<double> get, get_one, set, find, find_one, kfind, kfind_one;
};

void printTable(const char* title, const std::vector<Row>& rows) {
    std::printf("\n## %s\n\n", title);
    std::printf("ns per lookup, median of the repeats (min-max).\n\n");
    std::printf("| Keys | Eviction | Engine memory | GET | GET, one at a time | SET of an existing key | "
                "find() in std::unordered_map | find(), one at a time | find() in KeyTable | "
                "KeyTable, one at a time |\n");
    std::printf("|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    auto cell = [](const std::vector<double>& v) { return v.empty() ? std::string("-") : summary(v); };
    for(const Row& r : rows) {
        char mem[32];
        if(r.engine_mb >= 0) std::snprintf(mem, sizeof(mem), "%.1f MB", r.engine_mb);
        else std::snprintf(mem, sizeof(mem), "-");
        std::printf("| %zu | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", r.keys, r.policy, mem,
                    summary(r.get).c_str(), summary(r.get_one).c_str(), summary(r.set).c_str(),
                    cell(r.find).c_str(), cell(r.find_one).c_str(),
                    cell(r.kfind).c_str(), cell(r.kfind_one).c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if(!parseArgs(argc, argv, opt)) return 2;

    std::printf("# Lookup latency in the storage engine\n\n");
    std::printf("Keys of 16 bytes (\"key:000000012345\") inserted in random order, 3-byte values, "
                "no AOF, capacity = key count (no eviction). %zu lookups per measurement, "
                "%d repeats; Zipf alpha %.2f, scrambled.\n", opt.ops, opt.repeats, opt.alpha);

    std::vector<Row> uniform_rows, zipf_rows;
    double harness_ns = 0;

    for(size_t n : opt.sizes) {
        // Insert every key once, in random order
        std::vector<uint32_t> order(n);
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), std::mt19937_64(n));

        // One engine per eviction policy, filled the same way
        std::vector<std::unique_ptr<InMemoryStorage>> engines;
        std::vector<double> engine_mb;
        for(EvictionPolicy policy : opt.policies) {
#ifdef HAVE_MALLINFO2
            size_t before = heapInUse();
#endif
            engines.push_back(std::make_unique<InMemoryStorage>(n, nullptr, policy));
            for(uint32_t id : order) engines.back()->set(keyString(id), "xxx");
#ifdef HAVE_MALLINFO2
            engine_mb.push_back(static_cast<double>(heapInUse() - before) / 1e6);
#else
            engine_mb.push_back(-1);
#endif
        }

        // The same keys in a plain table, sized up front like the engine's
        std::unordered_map<std::string, FakeEntry> table;
        table.reserve(n);
        for(uint32_t id : order) table.emplace(keyString(id), FakeEntry{});

        // And in a KeyTable, the entries allocated in the same order
        KeyTable<InlineEntry, InlineKeyOf> ktable;
        std::vector<InlineEntry*> kentries;
        kentries.reserve(n);
        for(uint32_t id : order) {
            kentries.push_back(InlineEntry::create(keyString(id)));
            ktable.insert(kentries.back());
        }

        const std::string value = "xxx";
        auto find = [&table](const std::string& key) {
            auto it = table.find(key);
            return it == table.end() ? size_t{0} : static_cast<size_t>(static_cast<unsigned char>(it->second.bytes[0]));
        };
        auto kfind = [&ktable](const std::string& key) {
            InlineEntry* e = ktable.find(key);
            return e ? static_cast<size_t>(static_cast<unsigned char>(e->fixed[0])) : size_t{0};
        };

        for(int dist = 0; dist < 2; dist++) {
            if((dist == 0 && !opt.uniform) || (dist == 1 && !opt.zipf)) continue;
            uint64_t seed = 7919 * n + dist;
            auto ids = dist == 0 ? uniformIds(n, opt.ops, seed) : zipfIds(n, opt.ops, opt.alpha, seed);
            auto keys = formatKeys(ids);

            std::vector<Row> rows;
            for(size_t p = 0; p < engines.size(); p++) {
                rows.push_back(Row{n, policyName(opt.policies[p]), engine_mb[p], {}, {}, {}, {}, {}, {}, {}});
            }
            // One repeat's measurements: GET both ways and SET for each
            // engine, and find() both ways
            std::vector<std::function<void()>> tasks;
            for(size_t p = 0; p < engines.size(); p++) {
                InMemoryStorage* e = engines[p].get();
                Row* row = &rows[p];
                auto get = [e](const std::string& key) {
                    size_t len = 0;
                    e->readString(key, [&len](const std::string& v) { len = v.size(); });
                    return len;
                };
                auto set = [e, &value](const std::string& key) {
                    return static_cast<size_t>(e->set(key, value).success);
                };
                tasks.push_back([&keys, row, get] { row->get.push_back(nsPerLookup(keys, false, get)); });
                tasks.push_back([&keys, row, get] { row->get_one.push_back(nsPerLookup(keys, true, get)); });
                tasks.push_back([&keys, row, set] { row->set.push_back(nsPerLookup(keys, false, set)); });
            }
            tasks.push_back([&] { rows[0].find.push_back(nsPerLookup(keys, false, find)); });
            tasks.push_back([&] { rows[0].find_one.push_back(nsPerLookup(keys, true, find)); });
            tasks.push_back([&] { rows[0].kfind.push_back(nsPerLookup(keys, false, kfind)); });
            tasks.push_back([&] { rows[0].kfind_one.push_back(nsPerLookup(keys, true, kfind)); });

            // Warm up (fault in the pages, fill the caches and TLB), then
            // drop what the warm-up measured
            for(auto& task : tasks) task();
            for(Row& row : rows) {
                row.get.clear();
                row.get_one.clear();
                row.set.clear();
                row.find.clear();
                row.find_one.clear();
                row.kfind.clear();
                row.kfind_one.clear();
            }

            std::vector<size_t> order_of_tasks(tasks.size());
            std::iota(order_of_tasks.begin(), order_of_tasks.end(), 0);
            for(int r = 0; r < opt.repeats; r++) {
                // Rotate the order each repeat, and reverse it every other
                // one: rotation alone keeps neighbours in the same order, so
                // a drifting machine would favour the same one of each pair
                std::vector<size_t> now = order_of_tasks;
                std::rotate(now.begin(), now.begin() + static_cast<std::ptrdiff_t>(r % now.size()), now.end());
                if(r % 2) std::reverse(now.begin(), now.end());
                for(size_t t : now) tasks[t]();
            }
            auto& out = dist == 0 ? uniform_rows : zipf_rows;
            for(Row& row : rows) out.push_back(std::move(row));

            // The harness alone: copying each key, with no lookup
            if(harness_ns == 0) harness_ns = nsPerLookup(keys, false, [](const std::string& k) { return k.size(); });
        }
        for(InlineEntry* e : kentries) {
            e->~InlineEntry();
            ::operator delete(e);
        }
        std::fprintf(stderr, "done: %zu keys\n", n);
    }

    if(!uniform_rows.empty()) printTable("Uniform keys", uniform_rows);
    if(!zipf_rows.empty()) {
        char title[64];
        std::snprintf(title, sizeof(title), "Zipf keys (alpha %.2f)", opt.alpha);
        printTable(title, zipf_rows);
    }
    std::printf("\nThe harness alone (copying each key, no lookup): %.1f ns per lookup.\n", harness_ns);
    std::printf("Engine memory: heap growth from inserting the keys (glibc mallinfo2), "
                "including the hash table's bucket array.\n");
    return 0;
}
