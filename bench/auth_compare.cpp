// How long checking a password guess takes, depending on how much of the
// guess is right. If the time grows with the number of right leading bytes,
// a client can find the password a byte at a time: keep the guess whose
// first byte takes longest, then the second, and so on.
//
// Times three ways to compare, for guesses that go wrong at the first byte,
// in the middle, at the last byte, or not at all, and guesses of another
// length:
// - std::string's ==, which stops at the first difference (memcmp);
// - a byte loop that returns at the first difference, the usual mistake;
// - constantTimeEquals (Commands.h), which AUTH uses.
// Every cell is the median of interleaved rounds, so drift affects all alike.
//
// Build: cmake --build build --target auth_compare
// Run:   taskset -c 2 ./build/auth_compare [--length N] [--calls N] [--rounds R]

#include "../src/Commands.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Hides a value from the optimizer, so a comparison of unchanging strings
// isn't computed once and reused (GCC and Clang; elsewhere, a volatile copy)
template<typename T>
void opaque(T& value) {
#if defined(__GNUC__)
    asm volatile("" : "+r"(value) : : "memory");
#else
    volatile T copy = value;
    value = copy;
#endif
}

bool stdEquals(std::string_view a, std::string_view b) { return a == b; }

bool earlyExit(std::string_view a, std::string_view b) {
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); i++) {
        if(a[i] != b[i]) return false;
    }
    return true;
}

struct Method { const char* name; bool (*equals)(std::string_view, std::string_view); };
struct Guess { const char* name; std::string text; };

// Every guess is copied here before it is timed, so all are read from the same
// address: separate strings would differ in alignment, which can change a
// copy's speed and look like a difference between guesses.
alignas(64) char guess_buffer[kMaxPasswordLength + 64];

// ns per call, for `calls` calls
double timeCalls(const Method& method, const std::string& guess, const std::string& secret,
                 size_t calls) {
    std::memcpy(guess_buffer, guess.data(), guess.size());
    const char* g = guess_buffer;
    size_t matches = 0;
    auto start = Clock::now();
    for(size_t i = 0; i < calls; i++) {
        opaque(g);
        matches += method.equals(std::string_view(g, guess.size()), secret);
    }
    auto ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    opaque(matches);
    return ns / static_cast<double>(calls);
}

}  // namespace

int main(int argc, char* argv[]) {
    size_t length = 32;
    size_t calls = 200000;
    int rounds = 15;
    for(int i = 1; i + 1 < argc; i += 2) {
        std::string option = argv[i];
        long long value = std::atoll(argv[i + 1]);
        if(option == "--length") length = static_cast<size_t>(value);
        else if(option == "--calls") calls = static_cast<size_t>(value);
        else if(option == "--rounds") rounds = static_cast<int>(value);
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if(length < 2 || length > kMaxPasswordLength) {
        std::fprintf(stderr, "--length must be 2 to %zu\n", kMaxPasswordLength);
        return 2;
    }

    std::string secret(length, 's');
    for(size_t i = 0; i < length; i++) secret[i] = static_cast<char>('a' + i % 26);
    auto wrongAt = [&](size_t pos) { std::string g = secret; g[pos] ^= 1; return g; };
    const std::vector<Guess> guesses = {
        {"wrong at byte 0", wrongAt(0)},
        {"wrong in the middle", wrongAt(length / 2)},
        {"wrong at the last byte", wrongAt(length - 1)},
        {"right", secret},
        {"one byte short", secret.substr(0, length - 1)},
    };
    const std::vector<Method> methods = {
        {"std::string ==", stdEquals},
        {"loop, stops early", earlyExit},
        {"constantTimeEquals", constantTimeEquals},
    };

    // samples[method][guess] = ns per call in each round; the order of the
    // cells rotates every round
    std::vector<std::vector<std::vector<double>>> samples(
        methods.size(), std::vector<std::vector<double>>(guesses.size()));
    const size_t cells = methods.size() * guesses.size();
    for(int round = 0; round < rounds; round++) {
        for(size_t k = 0; k < cells; k++) {
            size_t cell = (k + static_cast<size_t>(round)) % cells;
            size_t m = cell / guesses.size(), g = cell % guesses.size();
            samples[m][g].push_back(timeCalls(methods[m], guesses[g].text, secret, calls));
        }
    }

    std::printf("Password of %zu bytes; median ns per comparison over %d rounds of %zu calls\n\n",
                length, rounds, calls);
    std::printf("| Guess |");
    for(const auto& method : methods) std::printf(" %s |", method.name);
    std::printf("\n|---|");
    for(size_t m = 0; m < methods.size(); m++) std::printf("---:|");
    std::printf("\n");
    std::vector<double> lowest(methods.size(), 1e300), highest(methods.size(), 0);
    for(size_t g = 0; g < guesses.size(); g++) {
        std::printf("| %s |", guesses[g].name);
        for(size_t m = 0; m < methods.size(); m++) {
            auto& s = samples[m][g];
            std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
            double median = s[s.size() / 2];
            lowest[m] = std::min(lowest[m], median);
            highest[m] = std::max(highest[m], median);
            std::printf(" %.2f |", median);
        }
        std::printf("\n");
    }
    std::printf("| **slowest / fastest** |");
    for(size_t m = 0; m < methods.size(); m++) std::printf(" **%.2fx** |", highest[m] / lowest[m]);
    std::printf("\n");
    return 0;
}
