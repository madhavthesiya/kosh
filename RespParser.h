#pragma once
#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// RESP (Redis Serialization Protocol) parser and serializer.
//
// RESP2 is a simple text-based protocol with 5 types:
//   +OK\r\n              → Simple String
//   -ERR message\r\n     → Error
//   :42\r\n              → Integer
//   $6\r\nfoobar\r\n     → Bulk String (length-prefixed binary-safe string)
//   $-1\r\n              → Null Bulk String
//   *3\r\n...            → Array of N elements
//
// RESP3, which a client asks for with HELLO 3, adds types for replies that
// RESP2 spells as arrays and bulk strings (Kosh uses these):
//   _\r\n                → Null
//   %1\r\n<key><value>   → Map of N key/value pairs (HGETALL, CONFIG GET, HELLO)
//   ~2\r\n...            → Set of N elements (SMEMBERS)
//   =9\r\ntxt:hello\r\n  → Verbatim string: text meant for people (INFO)
// Requests are the same arrays of bulk strings in both.
//
// Client sends commands as RESP arrays of bulk strings:
//   SET name madhav  →  *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nmadhav\r\n
//
// Why header-only? This is a pure utility class with no state.
// No .cpp file needed — keeps the build simple.

class RespParser {
public:
    // ==================== Parsing ====================

    // TCP is a byte stream: one recv() may hold several commands, or part of one.
    // So a parse has three possible outcomes, and the caller must tell them apart:
    //   Ok        a whole command was read; `consumed` bytes can be dropped
    //   NeedMore  the bytes so far are a valid start; wait for more data
    //   Error     the bytes can never become a valid command; reply and disconnect
    // Treating "invalid" as "incomplete" would make the server wait forever on
    // garbage while the client's buffer keeps growing.
    enum class ParseStatus { Ok, NeedMore, Error };

    struct ParseResult {
        ParseStatus status = ParseStatus::NeedMore;
        std::vector<std::string> args;  // when Ok; empty for a blank line or "*0"
        size_t consumed = 0;            // when Ok: bytes of input the command used
        std::string error;              // when Error: the reason, for "-ERR Protocol error: ..."
    };

    // Limits, matching Redis's defaults, so one client can't make the server
    // wait for or buffer an unbounded amount of data.
    static constexpr size_t kMaxBulkLength = size_t{512} * 1024 * 1024;  // proto-max-bulk-len
    static constexpr size_t kMaxInlineLength = size_t{64} * 1024;        // PROTO_INLINE_MAX_SIZE
    // "*" or "$", a 64-bit number and "\r\n" need at most 23 bytes, so a longer
    // header line without "\r\n" is an error, not something to wait for.
    static constexpr size_t kMaxHeaderLength = 32;

    // Until a client has given the password, its commands may have at most 10
    // arguments of at most 16 KB, more than AUTH needs. Without these limits a
    // client that doesn't know the password could still make the server
    // buffer up to the 1 GB query limit for it, on every connection. Redis has
    // the same two limits (added in 6.2.6, the release that fixed
    // CVE-2021-32675, a denial of service from many-element requests).
    static constexpr size_t kMaxUnauthenticatedArgs = 10;
    static constexpr size_t kMaxUnauthenticatedBulk = size_t{16} * 1024;

    // What parseInto() found; the arguments themselves are in the caller's vector
    struct Parsed {
        ParseStatus status = ParseStatus::NeedMore;
        size_t argc = 0;        // when Ok: the arguments are args[0..argc); 0 for a blank line or "*0"
        size_t consumed = 0;    // when Ok: bytes of input the command used
        std::string error;      // when Error: the reason, for "-ERR Protocol error: ..."
    };

    // Parses one command from the start of `buffer` into args[0..argc).
    //
    // `args` is reused from one command to the next: it only grows, and each
    // argument is assign()ed into a string that keeps its memory, so once the
    // strings are large enough, parsing a command allocates nothing. (It used to
    // allocate a vector of positions, a vector of arguments, and every argument
    // too long for the small-string buffer: 3 allocations for "GET <16-byte
    // key>".) Arguments are copied only once the whole command has arrived, so
    // a command that arrives in many pieces isn't copied again each time.
    //
    // `authenticated`: false while the client still owes the password, which
    // applies the kMaxUnauthenticated* limits, checked as each header arrives.
    // (An inline command is at most 64 KB either way.)
    static Parsed parseInto(std::string_view buffer, std::vector<std::string>& args,
                            bool authenticated = true) {
        if(buffer.empty()) return needMore();

        // RESP commands start with '*' (array). Anything else is inline.
        if(buffer[0] == '*') {
            return parseRespInto(buffer, args, authenticated);
        }
        return parseInlineInto(buffer, args);
    }

    // The same, with the arguments in a vector of their own (exactly argc of
    // them): for tests and tools, where an allocation doesn't matter.
    static ParseResult parse(std::string_view buffer, bool authenticated = true) {
        std::vector<std::string> args;
        Parsed parsed = parseInto(buffer, args, authenticated);
        ParseResult r;
        r.status = parsed.status;
        r.consumed = parsed.consumed;
        r.error = std::move(parsed.error);
        if(parsed.status == ParseStatus::Ok) {
            args.resize(parsed.argc);
            r.args = std::move(args);
        }
        return r;
    }

    // ==================== Serialization ====================
    // Each Redis command has a specific response type. These methods
    // let the server construct the correct RESP response.

    static std::string simpleString(const std::string& s) {
        return "+" + s + "\r\n";
    }

    // An error reply ends at the first "\r\n". The message may quote client input,
    // so line breaks in it become spaces (as in Redis); otherwise the rest would be
    // read as the next reply and the connection would fall out of step.
    static std::string error(const std::string& s) {
        std::string line = s;
        std::replace_if(line.begin(), line.end(), [](char c) { return c == '\r' || c == '\n'; }, ' ');
        return "-" + line + "\r\n";
    }

    static std::string integer(long long n) {
        return ":" + std::to_string(n) + "\r\n";
    }

    static std::string bulkString(const std::string& s) {
        return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
    }

    static std::string nullBulk() {
        return "$-1\r\n";
    }

    static std::string array(const std::vector<std::string>& items) {
        std::string result = "*" + std::to_string(items.size()) + "\r\n";
        for(const auto& item : items) {
            result += bulkString(item);
        }
        return result;
    }

    static std::string emptyArray() {
        return "*0\r\n";
    }

    // Appending forms, for building into a buffer that is reused: unlike the
    // forms above, they create no temporary strings, so once the buffer has
    // grown, building a reply or an AOF record allocates nothing.
    static void appendLength(std::string& out, char type, size_t n) {
        char digits[20];    // enough for any 64-bit size_t
        auto result = std::to_chars(digits, digits + sizeof(digits), n);
        out += type;
        out.append(digits, result.ptr);
        out += "\r\n";
    }

    static void appendBulk(std::string& out, std::string_view s) {
        appendLength(out, '$', s.size());
        out.append(s);
        out += "\r\n";
    }

    static void appendArray(std::string& out, std::initializer_list<std::string_view> items) {
        appendLength(out, '*', items.size());
        for(std::string_view item : items) appendBulk(out, item);
    }

    static void appendArray(std::string& out, const std::vector<std::string>& items) {
        appendLength(out, '*', items.size());
        for(const auto& item : items) appendBulk(out, item);
    }

    static void appendSimpleString(std::string& out, std::string_view s) {
        out += '+';
        out.append(s);
        out += "\r\n";
    }

    // As error(): line breaks in the message become spaces
    static void appendError(std::string& out, std::string_view message) {
        out += '-';
        size_t start = out.size();
        out.append(message);
        std::replace_if(out.begin() + static_cast<std::ptrdiff_t>(start), out.end(),
                        [](char c) { return c == '\r' || c == '\n'; }, ' ');
        out += "\r\n";
    }

    static void appendInteger(std::string& out, long long n) {
        char digits[24];    // enough for any 64-bit value and its sign
        auto result = std::to_chars(digits, digits + sizeof(digits), n);
        out += ':';
        out.append(digits, result.ptr);
        out += "\r\n";
    }

    // The replies whose type depends on the connection's protocol (2 or 3).
    // There is deliberately no form without it: a reply written without
    // asking which protocol the client speaks would be wrong for one of them.
    static void appendNull(std::string& out, int protocol) {
        out += protocol == 3 ? "_\r\n" : "$-1\r\n";
    }
    // A map of `pairs` keys and values follows; RESP2 sends them as a flat
    // array of 2 * pairs elements
    static void appendMapLength(std::string& out, size_t pairs, int protocol) {
        if(protocol == 3) appendLength(out, '%', pairs);
        else appendLength(out, '*', 2 * pairs);
    }
    static void appendSetLength(std::string& out, size_t n, int protocol) {
        appendLength(out, protocol == 3 ? '~' : '*', n);
    }
    // Text for people: RESP3's verbatim string, whose payload starts with the
    // 3-letter format "txt" and a colon; a bulk string in RESP2
    static void appendVerbatim(std::string& out, std::string_view text, int protocol) {
        if(protocol != 3) return appendBulk(out, text);
        appendLength(out, '=', text.size() + 4);
        out += "txt:";
        out.append(text);
        out += "\r\n";
    }

private:
    static Parsed needMore() { return {}; }

    static Parsed protocolError(std::string reason) {
        Parsed r;
        r.status = ParseStatus::Error;
        r.error = std::move(reason);
        return r;
    }

    static Parsed ok(size_t argc, size_t consumed) {
        Parsed r;
        r.status = ParseStatus::Ok;
        r.argc = argc;
        r.consumed = consumed;
        return r;
    }

    // The whole of `text` must be a decimal integer. std::from_chars, unlike
    // std::stoi, rejects "3abc", leading spaces and "+3", and never throws.
    static bool parseInteger(std::string_view text, long long& out) {
        if(text.empty()) return false;
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc() && end == text.data() + text.size();
    }

    // Reads a header line, the '*' or '$' at buffer[pos] followed by an integer and
    // "\r\n". On Ok, sets `value` and `next` (the position after the "\r\n").
    // Error means the line is too long to be a header or not an integer.
    static ParseStatus readHeader(std::string_view buffer, size_t pos,
                                  long long& value, size_t& next) {
        size_t window = std::min(buffer.size(), pos + kMaxHeaderLength);
        size_t crlf = buffer.substr(0, window).find("\r\n", pos + 1);
        if(crlf == std::string_view::npos) {
            return buffer.size() >= pos + kMaxHeaderLength ? ParseStatus::Error
                                                          : ParseStatus::NeedMore;
        }
        if(!parseInteger(buffer.substr(pos + 1, crlf - pos - 1), value)) return ParseStatus::Error;
        next = crlf + 2;
        return ParseStatus::Ok;
    }

    // A byte for an error message: as itself if printable, else as \xHH, so the
    // error reply stays a single line.
    static std::string describeByte(char c) {
        if(std::isprint(static_cast<unsigned char>(c))) return std::string(1, c);
        char hex[8];
        std::snprintf(hex, sizeof(hex), "\\x%02x", static_cast<unsigned char>(c));
        return hex;
    }

    // Parse a RESP array: *N\r\n followed by N bulk strings
    static Parsed parseRespInto(std::string_view buffer, std::vector<std::string>& args,
                                bool authenticated) {
        // Step 1: Read array header  *N\r\n
        long long count = 0;
        size_t pos = 0;
        ParseStatus header = readHeader(buffer, 0, count, pos);
        if(header == ParseStatus::NeedMore) return needMore();
        if(header == ParseStatus::Error || count > INT_MAX) {
            return protocolError("invalid multibulk length");
        }
        if(!authenticated && count > static_cast<long long>(kMaxUnauthenticatedArgs)) {
            return protocolError("unauthenticated multibulk length");
        }
        if(count <= 0) {   // "*0" or "*-1": nothing to run; Redis ignores these too
            return ok(0, pos);
        }

        // Step 2: check that all N bulk strings ($len\r\ndata\r\n) have arrived
        // and are well formed, without copying anything: a command that arrives
        // in many pieces is checked again on each attempt, but copied once.
        const size_t first = pos;
        for(long long i = 0; i < count; i++) {
            if(pos >= buffer.size()) return needMore();
            if(buffer[pos] != '$') {
                return protocolError("expected '$', got '" + describeByte(buffer[pos]) + "'");
            }

            long long len = 0;
            header = readHeader(buffer, pos, len, pos);
            if(header == ParseStatus::NeedMore) return needMore();
            if(header == ParseStatus::Error || len < 0 ||
               static_cast<unsigned long long>(len) > kMaxBulkLength) {
                return protocolError("invalid bulk length");
            }
            if(!authenticated && static_cast<unsigned long long>(len) > kMaxUnauthenticatedBulk) {
                return protocolError("unauthenticated bulk length");
            }

            // Check we have enough data: len bytes + \r\n
            size_t length = static_cast<size_t>(len);
            if(buffer.size() - pos < length + 2) return needMore();
            if(buffer[pos + length] != '\r' || buffer[pos + length + 1] != '\n') {
                return protocolError("bulk string not followed by CRLF");
            }
            pos += length + 2;   // skip data + \r\n
        }

        // Step 3: copy the arguments, reading the same headers again (now known
        // to be good) instead of keeping their positions in a vector
        size_t argc = static_cast<size_t>(count);
        if(args.size() < argc) args.resize(argc);
        size_t at = first;
        for(size_t i = 0; i < argc; i++) {
            long long length = 0;
            readHeader(buffer, at, length, at);
            args[i].assign(buffer.data() + at, static_cast<size_t>(length));
            at += static_cast<size_t>(length) + 2;
        }
        return ok(argc, pos);
    }

    // Parse an inline command: "PING\r\n" or "SET key value\r\n"
    // Supports both \r\n and \n line endings (for telnet compatibility)
    static Parsed parseInlineInto(std::string_view buffer, std::vector<std::string>& args) {
        // Look for the newline only as far as the longest allowed line ("\r\n"
        // included), so a client can't make the server buffer an endless line.
        size_t window = std::min(buffer.size(), kMaxInlineLength + 2);
        size_t nl = buffer.substr(0, window).find('\n');
        if(nl == std::string_view::npos) {
            return buffer.size() >= kMaxInlineLength + 2 ? protocolError("too big inline request")
                                                         : needMore();
        }

        // Extract line (strip \r if present)
        size_t length = (nl > 0 && buffer[nl - 1] == '\r') ? nl - 1 : nl;
        if(length > kMaxInlineLength) return protocolError("too big inline request");
        std::string_view line = buffer.substr(0, length);

        // Split by whitespace
        size_t argc = 0;
        size_t i = 0;
        while(i < line.size()) {
            while(i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
            size_t start = i;
            while(i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) i++;
            if(i > start) {
                if(args.size() == argc) args.emplace_back();
                args[argc++].assign(line.data() + start, i - start);
            }
        }
        return ok(argc, nl + 1);
    }
};
