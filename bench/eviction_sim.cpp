// The miss ratio of the storage engine's eviction, on real and synthetic
// cache traces.
//
// Each request is a GET; a miss is followed by a SET of the key, as a
// read-through cache would do (and as libCacheSim simulates). The capacity is
// counted in keys (--maxkeys), as a fraction of the trace's distinct keys. The
// requests go straight to InMemoryStorage: no network.
//
// Traces:
//   zipf:ALPHA       Zipf popularity over --universe keys, ranks scattered by a
//                    random permutation (YCSB's "scrambled" Zipf)
//   zipfscan:ALPHA   the same, plus scans: after every 90,000 requests, 10,000
//                    requests for keys that are never requested again
//   csv:PATH         one request per line, the key in column --key-column
//                    (default 2: libCacheSim's "time, object, size,
//                    next_access_vtime"); lines starting with '#' are skipped
//
// Every trace's keys are numbered in order of first appearance and stored as
// "key:000000000042" (16 bytes, as redis-benchmark's). --dump-trace writes
// those numbers, one request per line, so an independent implementation can
// replay the exact same requests (bench/eviction_reference.py does).
//
// Build: cmake --build build-bench --target eviction_sim
// Run:   ./build-bench/eviction_sim zipf:0.99 csv:twitter_cluster52.csv

#include "../src/InMemoryStorage.h"
#include "../src/Router.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::vector<double> capacities{0.001, 0.01, 0.1};
    size_t requests = 10000000;
    size_t universe = 1000000;
    int key_column = 2;
    std::string dump_trace;
    std::vector<std::string> traces;
    std::vector<EvictionPolicy> policies{EvictionPolicy::LRU};
    std::vector<size_t> threads;    // split the capacity across this many threads; empty: one, no column
};

const char* policyName(EvictionPolicy p) {
    return p == EvictionPolicy::SIEVE ? "sieve" : "lru";
}

struct Trace {
    std::string name;
    std::vector<uint32_t> ids;  // one per request; ids are 0 .. distinct-1, in order of first appearance
    size_t distinct = 0;
};

// Renumbers ids in order of first appearance, so every trace's keys are dense
void renumber(Trace& t) {
    std::unordered_map<uint64_t, uint32_t> seen;
    seen.reserve(t.ids.size() / 4);
    for(auto& id : t.ids) {
        auto [it, inserted] = seen.emplace(id, static_cast<uint32_t>(seen.size()));
        id = it->second;
    }
    t.distinct = seen.size();
}

std::vector<double> zipfCdf(size_t n, double alpha) {
    std::vector<double> cdf(n);
    double sum = 0;
    for(size_t r = 0; r < n; r++) {
        sum += 1.0 / std::pow(static_cast<double>(r + 1), alpha);
        cdf[r] = sum;
    }
    return cdf;
}

// Zipf requests over `universe` keys; with scans, every 90,000 of them are
// followed by 10,000 requests for keys used once and never again
Trace zipfTrace(double alpha, bool scans, const Options& opt) {
    Trace t;
    char name[64];
    std::snprintf(name, sizeof(name), "%s alpha %.2f", scans ? "Zipf + scans," : "Zipf", alpha);
    t.name = name;
    auto cdf = zipfCdf(opt.universe, alpha);
    std::vector<uint32_t> id_of_rank(opt.universe);
    std::iota(id_of_rank.begin(), id_of_rank.end(), 0);
    std::mt19937_64 rng(static_cast<uint64_t>(alpha * 1000) + (scans ? 1 : 0));
    std::shuffle(id_of_rank.begin(), id_of_rank.end(), rng);
    std::uniform_real_distribution<double> u(0.0, cdf.back());

    t.ids.reserve(opt.requests);
    uint64_t next_scan_key = opt.universe;
    while(t.ids.size() < opt.requests) {
        size_t zipf_requests = scans ? 90000 : opt.requests;
        for(size_t i = 0; i < zipf_requests && t.ids.size() < opt.requests; i++) {
            size_t rank = static_cast<size_t>(std::lower_bound(cdf.begin(), cdf.end(), u(rng)) - cdf.begin());
            t.ids.push_back(id_of_rank[std::min(rank, opt.universe - 1)]);
        }
        for(size_t i = 0; scans && i < 10000 && t.ids.size() < opt.requests; i++) {
            t.ids.push_back(static_cast<uint32_t>(next_scan_key++));
        }
    }
    renumber(t);
    return t;
}

// Keys from column `column` (1-based) of a comma-separated file
bool csvTrace(const std::string& path, int column, Trace& t) {
    std::ifstream in(path);
    if(!in) {
        std::fprintf(stderr, "can't open %s\n", path.c_str());
        return false;
    }
    std::unordered_map<std::string, uint32_t> seen;
    std::string line;
    while(std::getline(in, line)) {
        if(line.empty() || line[0] == '#') continue;
        size_t start = 0;
        for(int c = 1; c < column; c++) {
            start = line.find(',', start);
            if(start == std::string::npos) break;
            start++;
        }
        if(start == std::string::npos) {
            std::fprintf(stderr, "%s: a line has fewer than %d columns\n", path.c_str(), column);
            return false;
        }
        size_t end = line.find(',', start);
        std::string key = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
        key.erase(0, key.find_first_not_of(' '));
        key.erase(key.find_last_not_of(" \r") + 1);
        auto [it, inserted] = seen.emplace(std::move(key), static_cast<uint32_t>(seen.size()));
        t.ids.push_back(it->second);
    }
    t.distinct = seen.size();
    size_t slash = path.find_last_of('/');
    t.name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    return !t.ids.empty();
}

// "key:" and the id in 12 digits, into a reused string (no allocation)
void formatKey(std::string& key, uint32_t id) {
    char digits[16];
    auto [end, ec] = std::to_chars(digits, digits + sizeof(digits), id);
    (void)ec;
    size_t len = static_cast<size_t>(end - digits);
    key.assign("key:000000000000");
    std::memcpy(&key[16 - len], digits, len);
}

struct SimResult {
    size_t capacity;
    size_t misses;
    double ns_per_request;
};

// With several threads, each owns a share of the capacity and evicts on its own,
// as the server does: a request goes to the storage of its key's thread. The
// routing key is fixed, so that a run can be repeated exactly.
SimResult simulate(const Trace& t, size_t capacity, EvictionPolicy policy, size_t threads) {
    std::vector<std::unique_ptr<InMemoryStorage>> stores;
    for(size_t i = 0; i < threads; i++) {
        stores.push_back(std::make_unique<InMemoryStorage>(capacity / threads + (i < capacity % threads ? 1 : 0), nullptr, policy));
    }
    SipKey routing_key;
    routing_key.k0 = 1;
    routing_key.k1 = 2;
    const Router router(threads, routing_key);
    std::string key;
    key.reserve(16);
    const std::string value = "x";
    size_t misses = 0;
    auto start = Clock::now();
    for(uint32_t id : t.ids) {
        formatKey(key, id);
        InMemoryStorage& storage = *stores[router.shardOf(key)];
        if(!storage.readString(key, [](const std::string&) {}).success) {
            misses++;
            storage.set(key, value);
        }
    }
    double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    return {capacity, misses, ns / static_cast<double>(t.ids.size())};
}

std::vector<double> parseList(const std::string& s) {
    std::vector<double> v;
    size_t pos = 0;
    while(pos <= s.size()) {
        size_t comma = s.find(',', pos);
        if(comma == std::string::npos) comma = s.size();
        v.push_back(std::atof(s.substr(pos, comma - pos).c_str()));
        pos = comma + 1;
    }
    return v;
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for(int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if(a == "--capacities" && v)      { opt.capacities = parseList(v); i++; }
        else if(a == "--requests" && v)   { opt.requests = std::strtoull(v, nullptr, 10); i++; }
        else if(a == "--universe" && v)   { opt.universe = std::strtoull(v, nullptr, 10); i++; }
        else if(a == "--key-column" && v) { opt.key_column = std::atoi(v); i++; }
        else if(a == "--threads" && v) {
            for(double n : parseList(v)) opt.threads.push_back(static_cast<size_t>(n));
            i++;
        }
        else if(a == "--dump-trace" && v) { opt.dump_trace = v; i++; }
        else if(a == "--eviction" && v) {
            std::string e = v;
            opt.policies.clear();
            if(e == "lru" || e == "both") opt.policies.push_back(EvictionPolicy::LRU);
            if(e == "sieve" || e == "both") opt.policies.push_back(EvictionPolicy::SIEVE);
            if(opt.policies.empty()) return false;
            i++;
        }
        else if(a.rfind("--", 0) == 0) return false;
        else opt.traces.push_back(a);
    }
    for(double c : opt.capacities) {
        if(!(c > 0 && c <= 1)) return false;
    }
    return !opt.traces.empty() && opt.requests > 0 && opt.universe > 0 &&
           opt.universe < UINT32_MAX / 2 && opt.key_column >= 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if(!parseArgs(argc, argv, opt)) {
        std::fprintf(stderr, "usage: %s [--capacities 0.001,0.01,0.1] [--requests N] [--universe N]\n"
                             "       [--key-column N] [--dump-trace PATH] [--eviction lru|sieve|both] [--threads 1,2,4,8] TRACE...\n"
                             "TRACE: zipf:ALPHA | zipfscan:ALPHA | csv:PATH\n", argv[0]);
        return 2;
    }

    std::printf("# Miss ratio of the engine's eviction\n\n");
    std::printf("Each request is a GET, and a miss is followed by a SET of the key. Capacity "
                "in keys, as a fraction of the trace's distinct keys. Synthetic traces: %zu "
                "requests over %zu keys.\n\n", opt.requests, opt.universe);
    if(opt.threads.empty()) {
        std::printf("| Trace | Eviction | Requests | Distinct keys | Capacity | Keys | Misses | Miss ratio | ns per request |\n");
        std::printf("|---|---|---:|---:|---:|---:|---:|---:|---:|\n");
    } else {
        std::printf("| Trace | Eviction | Threads | Requests | Distinct keys | Capacity | Keys | Misses | Miss ratio |\n");
        std::printf("|---|---|---:|---:|---:|---:|---:|---:|---:|\n");
    }

    // A default-constructed ofstream converts to true, so test the option,
    // not the stream, to decide whether to write the trace
    const bool dumping = !opt.dump_trace.empty();
    std::ofstream dump;
    if(dumping) {
        dump.open(opt.dump_trace);
        if(!dump) {
            std::fprintf(stderr, "can't write %s\n", opt.dump_trace.c_str());
            return 1;
        }
    }

    for(const std::string& spec : opt.traces) {
        Trace t;
        if(spec.rfind("zipf:", 0) == 0) {
            t = zipfTrace(std::atof(spec.c_str() + 5), false, opt);
        } else if(spec.rfind("zipfscan:", 0) == 0) {
            t = zipfTrace(std::atof(spec.c_str() + 9), true, opt);
        } else if(spec.rfind("csv:", 0) == 0) {
            if(!csvTrace(spec.substr(4), opt.key_column, t)) return 1;
        } else {
            std::fprintf(stderr, "unknown trace %s\n", spec.c_str());
            return 2;
        }
        if(dumping) {
            dump << "# trace " << t.name << "\n";
            for(uint32_t id : t.ids) dump << id << "\n";
        }
        for(double fraction : opt.capacities) {
            size_t capacity = std::max<size_t>(1, static_cast<size_t>(fraction * static_cast<double>(t.distinct)));
            for(EvictionPolicy policy : opt.policies) {
                for(size_t threads : opt.threads) {
                    if(threads > capacity) continue;    // as the server refuses it
                    SimResult r = simulate(t, capacity, policy, threads);
                    std::printf("| %s | %s | %zu | %zu | %zu | %g | %zu | %zu | %.4f |\n", t.name.c_str(), policyName(policy),
                                threads, t.ids.size(), t.distinct, fraction, r.capacity, r.misses,
                                static_cast<double>(r.misses) / static_cast<double>(t.ids.size()));
                    std::fflush(stdout);
                }
                if(!opt.threads.empty()) continue;
                SimResult r = simulate(t, capacity, policy, 1);
                std::printf("| %s | %s | %zu | %zu | %g | %zu | %zu | %.4f | %.0f |\n", t.name.c_str(),
                            policyName(policy), t.ids.size(), t.distinct, fraction, r.capacity,
                            r.misses, static_cast<double>(r.misses) / static_cast<double>(t.ids.size()),
                            r.ns_per_request);
                std::fflush(stdout);
            }
        }
    }
    if(dumping && !dump.flush()) {
        std::fprintf(stderr, "writing %s failed\n", opt.dump_trace.c_str());
        return 1;
    }
    return 0;
}
