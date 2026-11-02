#pragma once
#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// RESP (Redis Serialization Protocol) parser and serializer.
//
// RESP is a simple text-based protocol with 5 types:
//   +OK\r\n              → Simple String
//   -ERR message\r\n     → Error
//   :42\r\n              → Integer
//   $6\r\nfoobar\r\n     → Bulk String (length-prefixed binary-safe string)
//   $-1\r\n              → Null Bulk String
//   *3\r\n...            → Array of N elements
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
    static constexpr size_t kMaxBulkLength = 512 * 1024 * 1024;  // proto-max-bulk-len
    static constexpr size_t kMaxInlineLength = 64 * 1024;        // PROTO_INLINE_MAX_SIZE
    // "*" or "$", a 64-bit number and "\r\n" need at most 23 bytes, so a longer
    // header line without "\r\n" is an error, not something to wait for.
    static constexpr size_t kMaxHeaderLength = 32;

    // Parses one command from the start of `buffer`.
    static ParseResult parse(std::string_view buffer) {
        if(buffer.empty()) return needMore();

        // RESP commands start with '*' (array). Anything else is inline.
        if(buffer[0] == '*') {
            return parseResp(buffer);
        }
        return parseInline(buffer);
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

private:
    static ParseResult needMore() { return {}; }

    static ParseResult protocolError(std::string reason) {
        ParseResult r;
        r.status = ParseStatus::Error;
        r.error = std::move(reason);
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
    static ParseResult parseResp(std::string_view buffer) {
        // Step 1: Read array header  *N\r\n
        long long count = 0;
        size_t pos = 0;
        ParseStatus header = readHeader(buffer, 0, count, pos);
        if(header == ParseStatus::NeedMore) return needMore();
        if(header == ParseStatus::Error || count > INT_MAX) {
            return protocolError("invalid multibulk length");
        }
        if(count <= 0) {   // "*0" or "*-1": nothing to run; Redis ignores these too
            ParseResult r;
            r.status = ParseStatus::Ok;
            r.consumed = pos;
            return r;
        }

        // Step 2: Read N bulk strings  $len\r\ndata\r\n
        // Only their positions are recorded here. The strings are copied once the
        // whole command has arrived, so a command that arrives in many pieces is
        // not copied again on every attempt.
        std::vector<std::pair<size_t, size_t>> spans;   // (start, length) of each argument
        spans.reserve(static_cast<size_t>(std::min(count, 16LL)));
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

            // Check we have enough data: len bytes + \r\n
            size_t length = static_cast<size_t>(len);
            if(buffer.size() - pos < length + 2) return needMore();
            if(buffer[pos + length] != '\r' || buffer[pos + length + 1] != '\n') {
                return protocolError("bulk string not followed by CRLF");
            }
            spans.emplace_back(pos, length);
            pos += length + 2;   // skip data + \r\n
        }

        ParseResult r;
        r.status = ParseStatus::Ok;
        r.consumed = pos;
        r.args.reserve(spans.size());
        for(auto [start, length] : spans) {
            r.args.emplace_back(buffer.substr(start, length));
        }
        return r;
    }

    // Parse an inline command: "PING\r\n" or "SET key value\r\n"
    // Supports both \r\n and \n line endings (for telnet compatibility)
    static ParseResult parseInline(std::string_view buffer) {
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
        ParseResult r;
        r.status = ParseStatus::Ok;
        r.consumed = nl + 1;
        size_t i = 0;
        while(i < line.size()) {
            while(i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
            size_t start = i;
            while(i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) i++;
            if(i > start) r.args.emplace_back(line.substr(start, i - start));
        }
        return r;
    }
};
