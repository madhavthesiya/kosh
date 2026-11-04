// Runs a libFuzzer-style target without libFuzzer, which GCC doesn't have:
//
//   fuzz_commands <file or directory>...
//       Runs each input once: the regression run that ctest does.
//   fuzz_commands --mutate <seconds> [--seed N] <corpus directory>
//       Mutates the corpus at random until the time is up.
//
// Random mutation isn't coverage-guided, as libFuzzer is, so it explores
// less; but it runs anywhere, under GCC's sanitizers. An input that fails is
// written to crash-input.bin before the process dies, so it can be replayed.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include(<sanitizer/common_interface_defs.h>) && \
    (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__))
#include <sanitizer/common_interface_defs.h>
#define HAVE_SANITIZER_CALLBACK 1
#endif
#endif

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

std::string current;    // the input being run, saved if the process dies

void saveCurrent() {
    if(std::FILE* f = std::fopen("crash-input.bin", "wb")) {
        std::fwrite(current.data(), 1, current.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "\nThe failing input (%zu bytes) is in crash-input.bin\n", current.size());
    }
}

void onFatalSignal(int signal) {
    saveCurrent();
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

void run(const std::string& input) {
    current = input;
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(input.data()), input.size());
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> readInputs(const std::vector<std::string>& paths) {
    std::vector<std::string> inputs;
    for(const auto& p : paths) {
        if(std::filesystem::is_directory(p)) {
            std::vector<std::filesystem::path> files;
            for(const auto& entry : std::filesystem::directory_iterator(p)) {
                if(entry.is_regular_file()) files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            for(const auto& f : files) inputs.push_back(readFile(f));
        } else {
            inputs.push_back(readFile(p));
        }
    }
    return inputs;
}

// Pieces of the protocol and the command set, which random bytes rarely spell
const char* const kTokens[] = {
    "*", "$", "\r\n", "\n", "\r", " ", "-1", "0", "1", "2", "3", "10", "11", "16384",
    "16385", "536870912", "536870913", "2147483648", "99999999999999999999",
    "AUTH", "default", "fuzz-password", "QUIT", "PING", "SET", "GET", "DEL", "EXISTS",
    "EX", "PX", "EXAT", "PXAT", "EXPIRE", "PEXPIREAT", "TTL", "TYPE", "SETEX", "LPUSH",
    "RPUSH", "LPOP", "RPOP", "LRANGE", "LLEN", "SADD", "SREM", "SMEMBERS", "SISMEMBER",
    "SCARD", "HSET", "HGET", "HDEL", "HGETALL", "HLEN", "INFO", "CONFIG", "CLIENT",
    "COMMAND", "BGREWRITEAOF", "*1\r\n$4\r\nPING\r\n", "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n",
};

std::string mutate(std::string s, const std::vector<std::string>& corpus, std::mt19937_64& rng) {
    auto pick = [&](size_t n) { return n ? static_cast<size_t>(rng() % n) : 0; };
    int count = 1 + static_cast<int>(rng() % 4);
    for(int m = 0; m < count; m++) {
        switch(rng() % 8) {
            case 0:     // flip a bit
                if(!s.empty()) s[pick(s.size())] ^= static_cast<char>(1u << (rng() % 8));
                break;
            case 1:     // a random byte
                if(!s.empty()) s[pick(s.size())] = static_cast<char>(rng());
                break;
            case 2:     // insert random bytes
                s.insert(pick(s.size() + 1), std::string(1 + pick(4), static_cast<char>(rng())));
                break;
            case 3: {   // delete a range
                if(s.empty()) break;
                size_t at = pick(s.size());
                s.erase(at, 1 + pick(std::min<size_t>(16, s.size() - at)));
                break;
            }
            case 4:     // insert a token
                s.insert(pick(s.size() + 1), kTokens[pick(std::size(kTokens))]);
                break;
            case 5: {   // repeat a chunk
                if(s.empty()) break;
                size_t at = pick(s.size()), len = 1 + pick(std::min<size_t>(64, s.size() - at));
                s.insert(pick(s.size() + 1), s.substr(at, len));
                break;
            }
            case 6: {   // splice in part of another input
                const std::string& other = corpus[pick(corpus.size())];
                if(other.empty()) break;
                size_t at = pick(other.size());
                s.insert(pick(s.size() + 1), other.substr(at, 1 + pick(other.size() - at)));
                break;
            }
            case 7:     // new options and split point (the first two bytes)
                if(s.size() >= 2) { s[0] = static_cast<char>(rng()); s[1] = static_cast<char>(rng()); }
                break;
        }
    }
    if(s.size() > 4096) s.resize(4096);
    return s;
}

}  // namespace

int main(int argc, char* argv[]) {
    // abort() (a broken invariant) isn't the sanitizers' to report
    std::signal(SIGABRT, onFatalSignal);
#ifdef HAVE_SANITIZER_CALLBACK
    // The sanitizers report crashes with a stack trace, then call this. A
    // SIGSEGV handler of our own would replace ASan's and lose the trace.
    __sanitizer_set_death_callback(saveCurrent);
#else
    std::signal(SIGSEGV, onFatalSignal);
    std::signal(SIGFPE, onFatalSignal);
#endif

    double seconds = 0;
    uint64_t seed = 1;
    std::vector<std::string> paths;
    for(int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if(arg == "--mutate" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if(arg == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else paths.push_back(arg);
    }
    std::vector<std::string> corpus = readInputs(paths);
    if(corpus.empty()) {
        std::fprintf(stderr, "usage: %s [--mutate seconds [--seed N]] <file or directory>...\n", argv[0]);
        return 2;
    }

    for(const auto& input : corpus) run(input);
    std::printf("%zu inputs ran\n", corpus.size());
    if(seconds <= 0) return 0;

    std::mt19937_64 rng(seed);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    uint64_t runs = 0;
    while(std::chrono::steady_clock::now() < deadline) {
        for(int batch = 0; batch < 1000; batch++, runs++) {
            run(mutate(corpus[rng() % corpus.size()], corpus, rng));
        }
    }
    std::printf("%llu mutated inputs ran (seed %llu), none failed\n",
                static_cast<unsigned long long>(runs), static_cast<unsigned long long>(seed));
    return 0;
}
