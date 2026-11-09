#pragma once
// Glob-style matching for SCAN's MATCH, as Redis 7.0.15's stringmatchlen
// (src/util.c) does it, case-sensitive:
//   *        any run of bytes, empty included
//   ?        any one byte
//   [abc]    one of these bytes; [^abc] none of them; [a-z] a range (given
//            backwards, [z-a], it is turned round); \ escapes inside []
//   \x       the byte x itself
// Byte for byte, NULs included. Two details are Redis's own and kept:
// - Bytes are compared as signed chars in ranges, as Redis's C code does on
//   x86, so [a-\xff] matches nothing above 0x7f.
// - A pattern can't match an empty key unless it is made of '*'s that sit
//   after something that matched (Redis: "*" alone doesn't match "").
// Depth: the matcher recurses once per '*' it has to try. Upstream Redis
// 7.0.15's source has no limit; the Redis 7.0.15 that Ubuntu 24.04 ships
// (what compare_with_redis.py runs against) carries a later security fix
// that stops at 1,000 levels. So does this: "a*" x 1,001 matches a key of
// 1,001 'a's on both, "a*" x 1,002 doesn't (measured, and compared).

#include <string_view>
#include <utility>

constexpr int kMaxGlobNesting = 1000;

namespace glob_detail {

// Redis reads one byte past a pattern on purpose, where its strings end in a
// NUL byte; a string_view doesn't, so past the end reads as '\0'
inline char at(std::string_view s, size_t i) { return i < s.size() ? s[i] : '\0'; }

// "Skip longer matches": once the rest of the pattern after a '*' matched
// nowhere in the rest of the string, a longer match for an earlier '*'
// can't help either (it would leave even less string). Without it, a pattern
// like a*a*a*a*b takes exponential time (Redis, CVE-2022-36021).
inline bool match(std::string_view p, std::string_view s, bool& skipLonger, int nesting) {
    if(nesting > kMaxGlobNesting) return false;
    while(!p.empty() && !s.empty()) {
        switch(p[0]) {
        case '*':
            while(at(p, 1) == '*') p.remove_prefix(1);
            if(p.size() == 1) return true;
            while(!s.empty()) {
                if(match(p.substr(1), s, skipLonger, nesting + 1)) return true;
                if(skipLonger) return false;
                s.remove_prefix(1);
            }
            skipLonger = true;
            return false;
        case '?':
            s.remove_prefix(1);
            break;
        case '[': {
            p.remove_prefix(1);
            bool negate = at(p, 0) == '^';
            if(negate) p.remove_prefix(1);
            bool matched = false;
            while(true) {
                if(at(p, 0) == '\\' && p.size() >= 2) {
                    p.remove_prefix(1);
                    if(p[0] == s[0]) matched = true;
                } else if(at(p, 0) == ']' || p.empty()) {
                    // The class ends at ']', or at the pattern's end if the
                    // [ is unclosed (Redis then steps back one, so its common
                    // step ends the pattern; here the check after the loop does)
                    break;
                } else if(p.size() >= 3 && p[1] == '-') {
                    // Signed on purpose: Redis compares bytes as signed chars
                    // here (see the top), and compare_with_redis.py checks it
                    // NOLINTBEGIN(bugprone-signed-char-misuse)
                    int start = static_cast<signed char>(p[0]);
                    int end = static_cast<signed char>(p[2]);
                    int c = static_cast<signed char>(s[0]);
                    // NOLINTEND(bugprone-signed-char-misuse)
                    if(start > end) std::swap(start, end);
                    p.remove_prefix(2);
                    if(c >= start && c <= end) matched = true;
                } else if(p[0] == s[0]) {
                    matched = true;
                }
                p.remove_prefix(1);
            }
            if(negate) matched = !matched;
            if(!matched) return false;
            s.remove_prefix(1);
            // The common step below skips the ']' (or nothing, if unclosed)
            if(p.empty()) { if(s.empty()) return true; return false; }
            break;
        }
        case '\\':
            if(p.size() >= 2) p.remove_prefix(1);
            [[fallthrough]];
        default:
            if(p[0] != s[0]) return false;
            s.remove_prefix(1);
            break;
        }
        p.remove_prefix(1);
        if(s.empty()) {
            while(at(p, 0) == '*') p.remove_prefix(1);
            break;
        }
    }
    return p.empty() && s.empty();
}

}  // namespace glob_detail

inline bool globMatch(std::string_view pattern, std::string_view key) {
    bool skipLonger = false;
    return glob_detail::match(pattern, key, skipLonger, 0);
}
