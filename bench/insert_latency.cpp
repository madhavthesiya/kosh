// How long one SET of a new key can take inside the storage engine, and why.
//
// Times every insert of N new keys, in the benchmark's key format and in random
// order, first into a plain std::unordered_map of entries the size of the
// server's (reporting each rehash), then through InMemoryStorage::set itself.
// No network and no client: what is left is the engine's own worst case.
//
// Build: cmake --build build-bench --target insert_latency
// Run:   ./build-bench/insert_latency [keys] [repeats]

#include "../InMemoryStorage.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double microsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// The keys redis-benchmark sends with -r: "key:" and a 12-digit number
std::vector<std::string> benchmarkKeys(size_t n, unsigned seed) {
    std::vector<size_t> ids(n);
    std::iota(ids.begin(), ids.end(), 0);
    std::shuffle(ids.begin(), ids.end(), std::mt19937(seed));
    std::vector<std::string> keys;
    keys.reserve(n);
    char buf[32];
    for(size_t id : ids) {
        std::snprintf(buf, sizeof(buf), "key:%012zu", id);
        keys.emplace_back(buf);
    }
    return keys;
}

struct Slow {
    double us;
    size_t size_before;   // keys stored before this insert
};

void printSlowest(std::vector<Slow>& slow, size_t count) {
    std::sort(slow.begin(), slow.end(), [](const Slow& a, const Slow& b) { return a.us > b.us; });
    for(size_t i = 0; i < count && i < slow.size(); i++) {
        std::printf("    %8.0f us  at key %zu\n", slow[i].us, slow[i].size_before + 1);
    }
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * (v.size() - 1))];
}

// An entry as large as the server's CacheEntry (112 bytes with GCC 13)
struct FakeEntry {
    char bytes[112];
};

}  // namespace

int main(int argc, char** argv) {
    size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 100000;
    int repeats = argc > 2 ? std::atoi(argv[2]) : 5;

    std::printf("# Insert latency of new keys\n\n");
    std::printf("%zu new keys of 16 bytes (\"key:000000012345\"), random order, "
                "3-byte values; %d repeats.\n\n", n, repeats);

    std::printf("## 1. std::unordered_map<std::string, 112-byte entry>: every rehash\n\n");
    for(int r = 0; r < repeats; r++) {
        auto keys = benchmarkKeys(n, 1000 + r);
        std::unordered_map<std::string, FakeEntry> map;
        std::printf("Repeat %d: rehashes that took over 100 us (keys before -> buckets after: time)\n", r + 1);
        double total_rehash = 0;
        for(const auto& key : keys) {
            size_t buckets = map.bucket_count();
            size_t size = map.size();
            auto start = Clock::now();
            map.emplace(key, FakeEntry{});
            double us = microsSince(start);
            if(map.bucket_count() != buckets) {
                total_rehash += us;
                if(us > 100) std::printf("    %7zu -> %7zu: %8.0f us\n", size, map.bucket_count(), us);
            }
        }
        std::printf("    all rehashes together: %.0f us\n", total_rehash);
    }

    std::printf("\n## 2. InMemoryStorage::set of a new key (no AOF)\n\n");
    for(int r = 0; r < repeats; r++) {
        auto keys = benchmarkKeys(n, 2000 + r);
        InMemoryStorage storage(2 * n, nullptr);    // room for all: no eviction
        std::vector<double> times;
        std::vector<Slow> slow;
        times.reserve(n);
        for(size_t i = 0; i < n; i++) {
            auto start = Clock::now();
            storage.set(keys[i], "xxx");
            double us = microsSince(start);
            times.push_back(us);
            slow.push_back({us, i});
        }
        std::printf("Repeat %d: p50 %.2f us, p99 %.2f us, p99.9 %.2f us, max %.0f us. Slowest:\n",
                    r + 1, percentile(times, 0.5), percentile(times, 0.99),
                    percentile(times, 0.999), *std::max_element(times.begin(), times.end()));
        printSlowest(slow, 4);
    }
    return 0;
}
