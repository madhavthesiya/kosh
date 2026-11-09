#pragma once
#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

// Whether `text` is a 64-bit integer as Redis reads one (string2ll in its
// util.c), and its value. Only the form the number itself would be written
// in counts: no sign but '-', no leading zero ("0" itself is fine), no
// spaces. So "5", "-5" and "0" are integers, and "05", "-0", "+5" and " 5"
// are not.
//
// std::from_chars alone accepts "05" and "-0". Redis refuses them in every
// argument (EXPIRE k 010, LRANGE l 01 1), and in a stored value INCR reads:
// a value counts as a number only if it is written the way that number is,
// so INCR never turns a string like the zip code "02139" into "2140", and
// what INCR writes back reads the same.
inline bool parseInt64(std::string_view text, int64_t& out) {
    // The longest int64_t is 20 bytes ("-9223372036854775808"). Checked first
    // so a long argument costs nothing: from_chars would scan all its digits.
    if(text.empty() || text.size() > 20) return false;
    const size_t first_digit = text[0] == '-' ? 1 : 0;
    if(first_digit == text.size()) return false;                        // "-"
    if(text[first_digit] == '0' && text.size() > 1) return false;       // "05", "-0", "00"
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();      // rejects "+5", " 5", "5x"
}
