#pragma once
// SipHash (Aumasson and Bernstein, "SipHash: a fast short-input PRF",
// INDOCRYPT 2012): a hash with a secret 128-bit key. The key table uses it so
// that clients can't choose keys that all land in one bucket.
//
// Why a keyed hash: libstdc++'s std::hash<std::string_view> is MurmurHash2
// with a fixed seed, the same in every process on every machine. Anyone can
// compute it offline and pick keys whose hashes share their low bits, which
// select the bucket. Thousands of such keys make one bucket chain thousands
// long, and every lookup in it a walk: inserting n of them costs O(n^2)
// ("hash flooding"; bench/hash_flood.cpp measures it). A random seed alone
// isn't enough for MurmurHash: Aumasson, Bernstein and Boßlet ("Hash-flooding
// DoS reloaded", 29C3, 2012) found collisions that hold for every seed.
// SipHash is designed so that without the key, outputs can't be predicted.
//
// SipHash-c-d does c rounds per 8-byte word and d at the end. Kosh uses
// SipHash-1-3, as Python, Rust's HashMap and Ruby do: one round per word
// instead of two keeps short keys cheap. Redis goes one step further, to 1-2,
// and its siphash.c says that is "not believed to be as strong"; Kosh keeps
// the extra final round, whose cost bench/hash_flood.cpp measures.
// SipHash-2-4, the paper's recommendation, is checked against the reference
// test vectors; 1-3, against CPython's string hash.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>

namespace siphash_detail {

inline uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

// 8 bytes as a little-endian number, on any CPU (compilers turn this into
// one load on little-endian ones)
inline uint64_t load64(const unsigned char* p) {
    return uint64_t(p[0]) | uint64_t(p[1]) << 8 | uint64_t(p[2]) << 16 | uint64_t(p[3]) << 24 |
           uint64_t(p[4]) << 32 | uint64_t(p[5]) << 40 | uint64_t(p[6]) << 48 | uint64_t(p[7]) << 56;
}

struct State {
    uint64_t v0, v1, v2, v3;
    void round() {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    }
};

}  // namespace siphash_detail

// SipHash-C-D of `len` bytes with the key (k0, k1): the key's first and last
// 8 bytes, each read as a little-endian number
template<int C, int D>
uint64_t sipHash(uint64_t k0, uint64_t k1, const void* data, size_t len) {
    using namespace siphash_detail;
    State s{k0 ^ 0x736f6d6570736575ull, k1 ^ 0x646f72616e646f6dull,
            k0 ^ 0x6c7967656e657261ull, k1 ^ 0x7465646279746573ull};
    const auto* p = static_cast<const unsigned char*>(data);
    const unsigned char* end = p + (len & ~size_t(7));
    for(; p != end; p += 8) {
        uint64_t m = load64(p);
        s.v3 ^= m;
        for(int i = 0; i < C; i++) s.round();
        s.v0 ^= m;
    }
    // The last 0-7 bytes, with the length's low byte on top
    uint64_t b = uint64_t(len) << 56;
    switch(len & 7) {
        case 7: b |= uint64_t(p[6]) << 48; [[fallthrough]];
        case 6: b |= uint64_t(p[5]) << 40; [[fallthrough]];
        case 5: b |= uint64_t(p[4]) << 32; [[fallthrough]];
        case 4: b |= uint64_t(p[3]) << 24; [[fallthrough]];
        case 3: b |= uint64_t(p[2]) << 16; [[fallthrough]];
        case 2: b |= uint64_t(p[1]) << 8;  [[fallthrough]];
        case 1: b |= uint64_t(p[0]);       break;
        case 0: break;
    }
    s.v3 ^= b;
    for(int i = 0; i < C; i++) s.round();
    s.v0 ^= b;
    s.v2 ^= 0xff;
    for(int i = 0; i < D; i++) s.round();
    return s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
}

struct SipKey {
    uint64_t k0 = 0, k1 = 0;

    // A key from std::random_device, the system's random source. Throws if
    // there is none, rather than run with a key an attacker could guess. Old
    // MinGW versions returned the same numbers in every process; with
    // MinGW-w64's GCC 13 on Windows 11, two runs were checked to differ.
    // (Its entropy() reports 0 there anyway, so nothing relies on that.)
    static SipKey random() {
        std::random_device source;
        auto word = [&] { return uint64_t(source()) << 32 | source(); };
        SipKey key;
        key.k0 = word();
        key.k1 = word();
        return key;
    }
};

// The key table's hash: SipHash-1-3 under a key chosen when the table is made
struct KeyedHash {
    SipKey key;
    size_t operator()(std::string_view s) const {
        return static_cast<size_t>(sipHash<1, 3>(key.k0, key.k1, s.data(), s.size()));
    }
};
