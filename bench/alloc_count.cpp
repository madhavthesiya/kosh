// Heap allocations per request, layer by layer: parsing, the storage engine,
// the command (dispatch and reply), and AOF logging. Unlike throughput, these
// counts don't depend on the machine or the session, so a change that removes
// an allocation shows up exactly.
//
// Every operator new in the process is counted; each line below is the count
// over many operations divided by their number, with keys and values shaped
// like redis-benchmark's (16-byte keys, 3-byte values).
//
// Build: cmake --build build-bench --target alloc_count
// Run:   ./build-bench/alloc_count

#include "../AOFLogger.h"
#include "../Commands.h"
#include "../InMemoryStorage.h"
#include "../RespParser.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

static size_t g_allocations = 0;

void* operator new(size_t size) {
    g_allocations++;
    if(void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

constexpr size_t kKeys = 10000;
constexpr size_t kOps = 100000;

std::string key(size_t i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key:%012zu", i % kKeys);
    return buf;
}

// Average allocations per call of `op(i)`, over kOps calls
template<typename Op>
double perCall(Op op) {
    size_t before = g_allocations;
    for(size_t i = 0; i < kOps; i++) op(i);
    return static_cast<double>(g_allocations - before) / kOps;
}

void row(const char* what, double allocations) {
    std::printf("| %s | %.2f |\n", what, allocations);
}

}  // namespace

int main() {
    // Keys and the requests carrying them are built before counting
    std::vector<std::string> keys, get_requests, set_requests;
    for(size_t i = 0; i < kKeys; i++) {
        keys.push_back(key(i));
        get_requests.push_back(RespParser::array({"GET", keys.back()}));
        set_requests.push_back(RespParser::array({"SET", keys.back(), "xxx"}));
    }

    // Storage without AOF, filled with every key
    InMemoryStorage storage(2 * kKeys, nullptr);
    for(const auto& k : keys) storage.set(k, "xxx");

    // Storage with an AOF on /dev/null: logging costs the same, nothing is stored
    AOFLogger aof("/dev/null");
    InMemoryStorage logged(2 * kKeys, &aof);
    for(const auto& k : keys) logged.set(k, "xxx");

    std::printf("# Heap allocations per operation\n\n");
    std::printf("%zu keys of 16 bytes, 3-byte values, averages over %zu operations.\n\n", kKeys, kOps);
    std::printf("| Operation | Allocations |\n|---|---:|\n");

    row("Parse a GET request", perCall([&](size_t i) {
        auto parsed = RespParser::parse(get_requests[i % kKeys]);
        (void)parsed;
    }));
    row("Parse a SET request", perCall([&](size_t i) {
        auto parsed = RespParser::parse(set_requests[i % kKeys]);
        (void)parsed;
    }));
    row("Engine: GET of an existing key", perCall([&](size_t i) {
        auto r = storage.get(keys[i % kKeys]);
        (void)r;
    }));
    row("Engine: SET of an existing key, no AOF", perCall([&](size_t i) {
        storage.set(keys[i % kKeys], "yyy");
    }));
    row("Engine: SET of an existing key, AOF on", perCall([&](size_t i) {
        logged.set(keys[i % kKeys], "yyy");
    }));

    // Whole commands: dispatch, engine and reply, from already parsed arguments
    std::vector<std::vector<std::string>> get_args, set_args;
    for(const auto& k : keys) {
        get_args.push_back({"GET", k});
        set_args.push_back({"SET", k, "zzz"});
    }
    CommandContext plain{storage};
    CommandContext with_aof{logged, &aof};
    std::string out;
    out.reserve(1 << 20);
    row("Command: GET (dispatch, engine, reply)", perCall([&](size_t i) {
        out += executeCommand(plain, get_args[i % kKeys]);
        if(out.size() > (1 << 19)) out.clear();
    }));
    row("Command: SET, AOF on (dispatch, engine, log, reply)", perCall([&](size_t i) {
        out += executeCommand(with_aof, set_args[i % kKeys]);
        if(out.size() > (1 << 19)) out.clear();
    }));

    // A whole request as the server handles it: parse, run, append the reply
    row("Request: GET, parsed and run", perCall([&](size_t i) {
        auto parsed = RespParser::parse(get_requests[i % kKeys]);
        out += executeCommand(plain, parsed.args);
        if(out.size() > (1 << 19)) out.clear();
    }));
    row("Request: SET, AOF on, parsed and run", perCall([&](size_t i) {
        auto parsed = RespParser::parse(set_requests[i % kKeys]);
        out += executeCommand(with_aof, parsed.args);
        if(out.size() > (1 << 19)) out.clear();
    }));
    return 0;
}
