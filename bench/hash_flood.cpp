// Hash flooding, and what defending against it costs.
//
// 1. The attack. libstdc++'s std::hash for strings is MurmurHash2 with a
//    fixed seed, so anyone can compute it. A key's bucket is its hash's low
//    bits, so keys whose hashes agree in enough low bits share one bucket at
//    every table size: a chain that every insert and lookup walks. This
//    generates such keys by trying candidates (the attacker's offline work,
//    timed), then inserts them into a KeyTable with std::hash and with
//    KeyedHash (SipHash-1-3 under a random key), each insert after a lookup,
//    as SET does. Doubling the keys should quadruple std::hash's time
//    (O(n^2)) and double KeyedHash's (O(n)).
// 2. The cost for ordinary keys: ns per hash of a 16-byte key, and ns per
//    lookup in tables of 1,000 to 1,000,000 such keys, with each hash. The
//    measurements alternate between the hashes, round by round.
//
// Build: cmake --build build --target hash_flood
// Run:   taskset -c 2 ./build/hash_flood [--max-keys N] [--rounds R]

#include "../KeyTable.h"
#include "../SipHash.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Entry { std::string key; };
struct KeyOfEntry { static std::string_view key(const Entry& e) { return e.key; } };

template<typename T>
void opaque(T& value) {
#if defined(__GNUC__)
    asm volatile("" : "+r"(value) : : "memory");
#else
    volatile T copy = value;
    value = copy;
#endif
}

double median(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Keys "flood-xxxxxxxxxx" (hex counter) whose std::hash has its low `bits`
// bits all zero. Counts the candidates tried.
std::vector<std::string> collidingKeys(size_t count, int bits, uint64_t& tried) {
    const size_t mask = (size_t{1} << bits) - 1;
    char key[] = "flood-0000000000";
    const size_t first_digit = 6, len = sizeof(key) - 1;
    std::vector<std::string> keys;
    std::hash<std::string_view> hash;
    tried = 0;
    while(keys.size() < count) {
        // Next candidate: increment the hex counter in place
        for(size_t i = len - 1; i >= first_digit; i--) {
            char& c = key[i];
            if(c == '9') { c = 'a'; break; }
            if(c != 'f') { c++; break; }
            c = '0';
        }
        tried++;
        if((hash(std::string_view(key, len)) & mask) == 0) keys.emplace_back(key, len);
    }
    return keys;
}

// Time to look up and then insert every key into an empty table, in ms
template<typename Hash>
double insertAll(const std::vector<std::string>& keys, size_t n, Hash hash) {
    std::vector<Entry> entries(n);
    for(size_t i = 0; i < n; i++) entries[i].key = keys[i];
    KeyTable<Entry, KeyOfEntry, Hash> table(hash);
    auto start = Clock::now();
    for(size_t i = 0; i < n; i++) {
        if(!table.find(entries[i].key)) table.insert(&entries[i]);
    }
    double ms = msSince(start);
    if(table.size() != n) { std::fprintf(stderr, "lost keys\n"); std::exit(1); }
    return ms;
}

// "key:" and a 12-digit number, as redis-benchmark's -r keys
std::string benchKey(size_t id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key:%012zu", id);
    return buf;
}

// ns per lookup of every key, in a shuffled order. Independent: the CPU can
// overlap the cache misses of neighbouring lookups, as in a tight loop. One
// at a time: each lookup's key depends on the previous result (through a
// zero the compiler can't see), so the misses can't overlap; in the server,
// other work separates lookups, which is closer to this.
template<typename Hash>
double lookupNs(const KeyTable<Entry, KeyOfEntry, Hash>& table, const std::vector<std::string>& order,
                bool one_at_a_time) {
    size_t found = 0;
    auto start = Clock::now();
    if(one_at_a_time) {
        size_t zero = 0;
        for(size_t i = 0; i < order.size(); i++) {
            const Entry* e = table.find(order[i + zero]);
            zero = e ? e->key.size() >> 20 : 0;     // keys are 16 bytes: always 0
            found += e != nullptr;
        }
    } else {
        for(const auto& key : order) found += table.find(key) != nullptr;
    }
    double ns = msSince(start) * 1e6 / static_cast<double>(order.size());
    opaque(found);
    return ns;
}

template<typename Hash>
double hashNs(Hash hash, const std::string& key, size_t calls) {
    const char* p = key.data();
    size_t sum = 0;
    auto start = Clock::now();
    for(size_t i = 0; i < calls; i++) {
        opaque(p);
        sum += hash(std::string_view(p, key.size()));
    }
    double ns = msSince(start) * 1e6 / static_cast<double>(calls);
    opaque(sum);
    return ns;
}

}  // namespace

int main(int argc, char* argv[]) {
    size_t max_keys = 40000;
    int rounds = 5;
    for(int i = 1; i + 1 < argc; i += 2) {
        std::string option = argv[i];
        if(option == "--max-keys") max_keys = static_cast<size_t>(std::atoll(argv[i + 1]));
        else if(option == "--rounds") rounds = std::atoi(argv[i + 1]);
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    // ---- 1. The attack ----
    // The largest table holding max_keys has about 2 * next_pow2(max_keys / 7)
    // buckets (it grows at 7 keys per bucket); agreeing in that many low bits
    // puts the keys in one bucket at every size up to it
    int bits = 1;
    while((size_t{1} << bits) < 2 * std::max<size_t>(max_keys / 7, 1)) bits++;
    bits++;
    uint64_t tried = 0;
    auto start = Clock::now();
    std::vector<std::string> flood = collidingKeys(max_keys, bits, tried);
    double gen_s = msSince(start) / 1000;
    std::printf("## 1. Keys chosen to collide under std::hash\n\n");
    std::printf("Found %zu keys whose std::hash agrees in its low %d bits, from %.2e candidates, "
                "in %.1f s on one core.\n\n", flood.size(), bits, static_cast<double>(tried), gen_s);

    std::vector<std::string> random_keys;
    for(size_t i = 0; i < max_keys; i++) random_keys.push_back(benchKey(i * 7919 + 1));

    std::printf("Look up, then insert, each key into an empty table: median ms of %d rounds\n\n", rounds);
    std::printf("| Keys | Colliding keys, std::hash | Colliding keys, KeyedHash | Ordinary keys, std::hash | Ordinary keys, KeyedHash |\n");
    std::printf("|---:|---:|---:|---:|---:|\n");
    SipKey key = SipKey::random();
    for(size_t n = max_keys / 16; n <= max_keys; n *= 2) {
        std::vector<double> a, b, c, d;
        for(int r = 0; r < rounds; r++) {
            a.push_back(insertAll(flood, n, std::hash<std::string_view>()));
            b.push_back(insertAll(flood, n, KeyedHash{key}));
            c.push_back(insertAll(random_keys, n, std::hash<std::string_view>()));
            d.push_back(insertAll(random_keys, n, KeyedHash{key}));
        }
        std::printf("| %zu | %.1f | %.2f | %.2f | %.2f |\n", n, median(a), median(b), median(c), median(d));
    }

    // ---- 2. The cost for ordinary keys ----
    std::printf("\n## 2. The cost for ordinary keys\n\n");
    const std::string sample = benchKey(123456789);
    std::vector<double> h_std, h_13, h_24;
    for(int r = 0; r < rounds; r++) {
        h_std.push_back(hashNs(std::hash<std::string_view>(), sample, 20000000));
        h_13.push_back(hashNs(KeyedHash{key}, sample, 20000000));
        h_24.push_back(hashNs([&](std::string_view s) {
            return static_cast<size_t>(sipHash<2, 4>(key.k0, key.k1, s.data(), s.size()));
        }, sample, 20000000));
    }
    std::printf("One hash of a 16-byte key: std::hash %.2f ns, SipHash-1-3 %.2f ns, SipHash-2-4 %.2f ns "
                "(median of %d rounds)\n\n", median(h_std), median(h_13), median(h_24), rounds);

    std::printf("Lookups of random keys: median ns per lookup of %d rounds\n\n", rounds);
    std::printf("| Keys | Independent: std::hash | KeyedHash | ratio "
                "| One at a time: std::hash | KeyedHash | ratio |\n|---:|---:|---:|---:|---:|---:|---:|\n");
    std::mt19937_64 rng(42);
    for(size_t n : {size_t{1000}, size_t{100000}, size_t{1000000}}) {
        std::vector<Entry> entries(n);
        for(size_t i = 0; i < n; i++) entries[i].key = benchKey(i * 7919 + 1);
        KeyTable<Entry, KeyOfEntry> by_std;
        KeyTable<Entry, KeyOfEntry, KeyedHash> by_sip(KeyedHash{key});
        for(auto& e : entries) { by_std.insert(&e); by_sip.insert(&e); }
        std::vector<std::string> order;
        size_t lookups = std::max<size_t>(n, 2000000);
        for(size_t i = 0; i < lookups; i++) order.push_back(entries[rng() % n].key);
        std::vector<double> s, k, s1, k1;
        for(int r = 0; r < rounds; r++) {
            s.push_back(lookupNs(by_std, order, false));
            k.push_back(lookupNs(by_sip, order, false));
            s1.push_back(lookupNs(by_std, order, true));
            k1.push_back(lookupNs(by_sip, order, true));
        }
        std::printf("| %zu | %.1f | %.1f | %.2fx | %.1f | %.1f | %.2fx |\n", n,
                    median(s), median(k), median(k) / median(s),
                    median(s1), median(k1), median(k1) / median(s1));
    }
    return 0;
}
