#include "Commands.h"
#include "Integer.h"
#include "RespParser.h"
#include "Version.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>
#ifdef _WIN32
#include <process.h>    // _getpid
#else
#include <unistd.h>     // getpid, sysconf
#endif

namespace {

std::string toUpper(std::string_view s) {
    std::string upper(s);
    // std::toupper needs an unsigned char value: passing a plain char that is
    // negative (a byte >= 0x80 where char is signed) is undefined behaviour.
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return upper;
}

// Every handler appends its reply to `out`, the client's output buffer, so a
// reply is built in place: no temporary string per reply, and GET copies the
// value once, from the table straight into the buffer.

void ok(std::string& out) { RespParser::appendSimpleString(out, "OK"); }

// Redis's error replies, word for word
constexpr const char* kWrongType = "WRONGTYPE Operation against a key holding the wrong kind of value";
constexpr const char* kNotAnInteger = "ERR value is not an integer or out of range";
constexpr const char* kSyntaxError = "ERR syntax error";
std::string invalidExpireTime(const char* command) {
    return std::string("ERR invalid expire time in '") + command + "' command";
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::toupper(x) == std::toupper(y);
           });
}

// An index past the range of int selects what INT_MIN or INT_MAX would
int clampToInt(int64_t value) {
    return static_cast<int>(std::clamp<int64_t>(value, INT_MIN, INT_MAX));
}

// A TTL argument of SET or SETEX, as an absolute Unix time in milliseconds.
// `seconds`: the number is in seconds (EX, EXAT, SETEX), not milliseconds.
// `relative`: it counts from now (EX, PX, SETEX), not from the Unix epoch.
// As in Redis, it must be a positive integer, and the absolute time must fit
// in 64 bits. Returns an error message, or an empty string if it is valid.
std::string parseExpiry(const std::string& text, bool seconds, bool relative,
                        const char* command, int64_t& unix_time_ms) {
    int64_t value;
    if(!parseInt64(text, value)) return kNotAnInteger;
    if(value <= 0) return invalidExpireTime(command);
    if(seconds) {
        if(value > INT64_MAX / 1000) return invalidExpireTime(command);
        value *= 1000;
    }
    if(relative) {
        int64_t now = InMemoryStorage::unixTimeMs();
        if(value > INT64_MAX - now) return invalidExpireTime(command);
        value += now;
    }
    unix_time_ms = value;
    return "";
}

// ---- Connection and server ----

// PING [message]: PONG, or the message back as a bulk string, as in Redis
void cmdPing(CommandContext&, const Args& args, std::string& out) {
    if(args.size() > 2) return RespParser::appendError(out, "ERR wrong number of arguments for 'ping' command");
    if(args.size() == 2) return RespParser::appendBulk(out, args[1]);
    RespParser::appendSimpleString(out, "PONG");
}

void cmdQuit(CommandContext& ctx, const Args&, std::string& out) {
    ctx.close_connection = true;
    ok(out);
}

constexpr const char* kWrongPass = "WRONGPASS invalid username-password pair or user is disabled.";

// Whether user/password may log in: the only user is "default" (as in Redis
// without ACL users), and user names are case-sensitive. With no password
// set, "default" accepts any password (Redis's nopass user).
bool credentialsMatch(const CommandContext& ctx, const std::string& user, const std::string& password) {
    bool password_ok = !ctx.password || constantTimeEquals(password, *ctx.password);
    return user == "default" && password_ok;
}

// AUTH <password>, or AUTH <username> <password>: Redis's two forms. Replies
// are Redis's, word for word. A failed AUTH changes nothing: a client that
// had authenticated stays authenticated, as in Redis.
void cmdAuth(CommandContext& ctx, const Args& args, std::string& out) {
    if(args.size() > 3) return RespParser::appendError(out, kSyntaxError);
    if(args.size() == 2 && !ctx.password) {
        return RespParser::appendError(out, "ERR AUTH <password> called without any password "
            "configured for the default user. Are you sure your configuration is correct?");
    }
    static const std::string kDefaultUser = "default";
    const std::string& user = args.size() == 3 ? args[1] : kDefaultUser;
    if(!credentialsMatch(ctx, user, args[args.size() - 1])) return RespParser::appendError(out, kWrongPass);
    ctx.authenticated = true;
    ok(out);
}

// A client name must be printable ASCII without spaces, as in Redis, whose
// CLIENT LIST output is split on spaces. Empty means "no name".
bool setClientName(CommandContext& ctx, const std::string& name, std::string& out) {
    for(unsigned char c : name) {
        if(c < '!' || c > '~') {
            RespParser::appendError(out, "ERR Client names cannot contain spaces, newlines or special characters.");
            return false;
        }
    }
    if(ctx.client_name) *ctx.client_name = name;
    return true;
}

// HELLO [protover [AUTH username password] [SETNAME clientname]]
//
// What modern clients send first: redis-py 8 and node-redis 6 open every
// connection with HELLO 3 (with AUTH when there's a password), and give up
// if it fails, without falling back to RESP2. It switches the connection's
// protocol, can authenticate and name it, and describes the server. Checks
// and replies follow Redis 7.0.15's, in its order: the version, the
// options, the credentials, then whether the client is authenticated.
void cmdHello(CommandContext& ctx, const Args& args, std::string& out) {
    int protocol = ctx.protocol;
    size_t next = 1;
    if(args.size() >= 2) {
        int64_t version;
        if(!parseInt64(args[1], version)) {
            return RespParser::appendError(out, "ERR Protocol version is not an integer or out of range");
        }
        if(version < 2 || version > 3) return RespParser::appendError(out, "NOPROTO unsupported protocol version");
        protocol = static_cast<int>(version);
        next = 2;
    }
    const std::string* user = nullptr;
    const std::string* password = nullptr;
    const std::string* name = nullptr;
    for(size_t i = next; i < args.size(); i++) {
        size_t more = args.size() - 1 - i;
        if(equalsIgnoreCase(args[i], "AUTH") && more >= 2) {
            user = &args[i + 1];
            password = &args[i + 2];
            i += 2;
        } else if(equalsIgnoreCase(args[i], "SETNAME") && more >= 1) {
            name = &args[i + 1];
            i += 1;
        } else {
            return RespParser::appendError(out, "ERR Syntax error in HELLO option '" + args[i].substr(0, 128) + "'");
        }
    }
    if(user) {
        if(!credentialsMatch(ctx, *user, *password)) return RespParser::appendError(out, kWrongPass);
        ctx.authenticated = true;
    }
    if(!ctx.authenticated) {
        return RespParser::appendError(out, "NOAUTH HELLO must be called with the client already "
            "authenticated, otherwise the HELLO AUTH <user> <pass> option can be used to authenticate "
            "the client and select the RESP protocol version at the same time");
    }
    if(name && !setClientName(ctx, *name, out)) return;
    ctx.protocol = protocol;

    // The server's description, in the protocol just chosen: a map in RESP3,
    // a flat array of names and values in RESP2. Same fields as Redis.
    RespParser::appendMapLength(out, 7, protocol);
    RespParser::appendBulk(out, "server");
    RespParser::appendBulk(out, "kosh");
    RespParser::appendBulk(out, "version");
    RespParser::appendBulk(out, kKoshVersion);
    RespParser::appendBulk(out, "proto");
    RespParser::appendInteger(out, protocol);
    RespParser::appendBulk(out, "id");
    RespParser::appendInteger(out, static_cast<long long>(ctx.client_id));
    RespParser::appendBulk(out, "mode");
    RespParser::appendBulk(out, "standalone");
    RespParser::appendBulk(out, "role");
    RespParser::appendBulk(out, "master");
    RespParser::appendBulk(out, "modules");
    RespParser::appendLength(out, '*', 0);
}

// CLIENT ID | GETNAME | SETNAME name | HELP. Anything else gets Redis's
// "unknown subcommand" error, which clients expect: on connect they send
// subcommands newer Redis versions have (SETINFO, MAINT_NOTIFICATIONS) and
// carry on when they fail. Answering OK would claim support Kosh lacks.
void cmdClient(CommandContext& ctx, const Args& args, std::string& out) {
    const std::string& sub = args[1];
    // A known subcommand with the wrong number of arguments gets Redis's
    // arity error, which names it in lower case
    static const std::pair<const char*, size_t> kSubcommands[] = {
        {"id", 2}, {"getname", 2}, {"setname", 3}, {"help", 2}};
    for(const auto& [name, argc] : kSubcommands) {
        if(equalsIgnoreCase(sub, name) && args.size() != argc) {
            return RespParser::appendError(out, std::string("ERR wrong number of arguments for 'client|")
                                                + name + "' command");
        }
    }
    if(equalsIgnoreCase(sub, "ID")) return RespParser::appendInteger(out, static_cast<long long>(ctx.client_id));
    if(equalsIgnoreCase(sub, "GETNAME")) {
        if(!ctx.client_name || ctx.client_name->empty()) return RespParser::appendNull(out, ctx.protocol);
        return RespParser::appendBulk(out, *ctx.client_name);
    }
    if(equalsIgnoreCase(sub, "SETNAME")) {
        if(setClientName(ctx, args[2], out)) ok(out);
        return;
    }
    if(equalsIgnoreCase(sub, "HELP")) {
        static const char* const kHelp[] = {
            "CLIENT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
            "GETNAME",
            "    Return the name of the current connection.",
            "ID",
            "    Return the ID of the current connection.",
            "SETNAME <name>",
            "    Assign the name <name> to the current connection.",
            "HELP",
            "    Print this help.",
        };
        RespParser::appendLength(out, '*', std::size(kHelp));
        for(const char* line : kHelp) RespParser::appendSimpleString(out, line);
        return;
    }
    RespParser::appendError(out, "ERR unknown subcommand '" + sub.substr(0, 128) + "'. Try CLIENT HELP.");
}

// redis-cli sends COMMAND DOCS at startup; an empty reply means "no docs"
void cmdCommand(CommandContext&, const Args&, std::string& out) { RespParser::appendLength(out, '*', 0); }

// redis-benchmark sends CONFIG GET save and CONFIG GET appendonly at
// startup; Kosh has no such settings to report, so the map is empty
void cmdConfig(CommandContext& ctx, const Args& args, std::string& out) {
    if(equalsIgnoreCase(args[1], "GET")) return RespParser::appendMapLength(out, 0, ctx.protocol);
    RespParser::appendError(out, "ERR unknown subcommand '" + args[1].substr(0, 128) + "'");
}

// Resident memory in bytes (Linux, from /proc); 0 where it isn't known.
// Not glibc's allocator count, which would miss the key table's mmap'ed arrays.
size_t residentBytes() {
#if defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    size_t total_pages = 0, resident_pages = 0;
    if(statm >> total_pages >> resident_pages) {
        return resident_pages * static_cast<size_t>(sysconf(_SC_PAGESIZE));
    }
#endif
    return 0;
}

uint64_t processId() {
#ifdef _WIN32
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

// INFO [section ...]: the server's state in Redis's format and with Redis's
// field names, so what reads Redis's INFO (redis-py's info(), dashboards)
// reads Kosh's: "# Section" lines, name:value lines, a blank line between
// sections. Sections are chosen by name, in any case; none, "all", "default"
// or "everything" means all of them, and an unknown one gives nothing, as in
// Redis. Left out rather than made up:
// - redis_version: Kosh isn't a complete Redis of any version, and tools
//   decide what to send by it;
// - used_memory (glibc's count would miss the key table), and avg_ttl, which
//   Kosh doesn't track. Kosh's limit is maxkeys, not maxmemory.
void cmdInfo(CommandContext& ctx, const Args& args, std::string& out) {
    appendInfo(localInfo(ctx), args, ctx.protocol, out);
}

// ---- Strings ----

// SET key value [EX seconds | PX milliseconds | EXAT unix-seconds | PXAT unix-milliseconds]
// The AOF records every SET with a TTL as PXAT. Redis's other options (NX, XX,
// GET, KEEPTTL) aren't supported and are a syntax error: ignoring NX would
// let "SET lock me NX" take a lock that someone else holds.
void cmdSet(CommandContext& ctx, const Args& args, std::string& out) {
    // As Redis does: check the options' syntax first, then the number
    const std::string* option = nullptr;
    const std::string* number = nullptr;
    for(size_t i = 3; i < args.size(); i += 2) {
        bool known = equalsIgnoreCase(args[i], "EX") || equalsIgnoreCase(args[i], "PX") ||
                     equalsIgnoreCase(args[i], "EXAT") || equalsIgnoreCase(args[i], "PXAT");
        // Each option needs a value, and only one expiry may be given
        if(!known || option || i + 1 == args.size()) return RespParser::appendError(out, kSyntaxError);
        option = &args[i];
        number = &args[i + 1];
    }
    if(!option) {
        auto r = ctx.storage.set(args[1], args[2]);
        return r.success ? ok(out) : RespParser::appendError(out, "ERR set failed");
    }
    bool seconds = equalsIgnoreCase(*option, "EX") || equalsIgnoreCase(*option, "EXAT");
    bool relative = equalsIgnoreCase(*option, "EX") || equalsIgnoreCase(*option, "PX");
    int64_t at;
    std::string error = parseExpiry(*number, seconds, relative, "set", at);
    if(!error.empty()) return RespParser::appendError(out, error);
    auto r = ctx.storage.setExpiringAt(args[1], args[2], at);
    r.success ? ok(out) : RespParser::appendError(out, "ERR set failed");
}

// The value goes from the table straight into the reply, copied once
void cmdGet(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.readString(args[1], [&out](const std::string& value) {
        RespParser::appendBulk(out, value);
    });
    if(r.error == ErrorCode::WRONG_TYPE) RespParser::appendError(out, kWrongType);
    else if(!r.success) RespParser::appendNull(out, ctx.protocol);
}

void cmdSetex(CommandContext& ctx, const Args& args, std::string& out) {
    int64_t at;
    std::string error = parseExpiry(args[2], true, true, "setex", at);
    if(!error.empty()) return RespParser::appendError(out, error);
    auto r = ctx.storage.setExpiringAt(args[1], args[3], at);
    r.success ? ok(out) : RespParser::appendError(out, "ERR setex failed");
}

// MGET key [key ...]: each value, or nil where there is no string (a list,
// say: unlike GET, MGET doesn't fail with WRONGTYPE)
void cmdMget(CommandContext& ctx, const Args& args, std::string& out) {
    RespParser::appendLength(out, '*', args.size() - 1);
    ctx.storage.readStrings(args.begin() + 1, args.end(), [&](const std::string* value) {
        if(value) RespParser::appendBulk(out, *value);
        else RespParser::appendNull(out, ctx.protocol);
    });
}

// MSET key value [key value ...]
void cmdMset(CommandContext& ctx, const Args& args, std::string& out) {
    if(args.size() % 2 == 0) {      // a key without a value
        return RespParser::appendError(out, "ERR wrong number of arguments for 'mset' command");
    }
    ctx.storage.mset(args.begin() + 1, args.end());
    ok(out);
}

// INCR, DECR, INCRBY, DECRBY. In Redis's order (t_string.c): the increment
// first, then the key's type, whether its value is an integer, and whether the
// sum fits in 64 bits. Each error changes nothing.
void incrementBy(CommandContext& ctx, const std::string& key, int64_t delta, std::string& out) {
    auto r = ctx.storage.incrBy(key, delta);
    switch(r.error) {
        case ErrorCode::NONE:             return RespParser::appendInteger(out, std::stoll(r.value));
        case ErrorCode::WRONG_TYPE:       return RespParser::appendError(out, kWrongType);
        case ErrorCode::INTEGER_OVERFLOW: return RespParser::appendError(out, "ERR increment or decrement would overflow");
        default:                          return RespParser::appendError(out, kNotAnInteger);
    }
}

void cmdIncr(CommandContext& ctx, const Args& args, std::string& out) { incrementBy(ctx, args[1], 1, out); }
void cmdDecr(CommandContext& ctx, const Args& args, std::string& out) { incrementBy(ctx, args[1], -1, out); }

void cmdIncrby(CommandContext& ctx, const Args& args, std::string& out) {
    int64_t delta;
    if(!parseInt64(args[2], delta)) return RespParser::appendError(out, kNotAnInteger);
    incrementBy(ctx, args[1], delta, out);
}

void cmdDecrby(CommandContext& ctx, const Args& args, std::string& out) {
    int64_t delta;
    if(!parseInt64(args[2], delta)) return RespParser::appendError(out, kNotAnInteger);
    // Its negation doesn't fit in 64 bits
    if(delta == INT64_MIN) return RespParser::appendError(out, "ERR decrement would overflow");
    incrementBy(ctx, args[1], -delta, out);
}

// ---- Keys ----

// DEL key [key ...]: how many were removed
void cmdDel(CommandContext& ctx, const Args& args, std::string& out) {
    RespParser::appendInteger(out, static_cast<long long>(ctx.storage.deleteKeys(args.begin() + 1, args.end())));
}

// EXISTS key [key ...]: how many exist, a key given twice counted twice
void cmdExists(CommandContext& ctx, const Args& args, std::string& out) {
    RespParser::appendInteger(out, static_cast<long long>(ctx.storage.countExisting(args.begin() + 1, args.end())));
}

void cmdType(CommandContext& ctx, const Args& args, std::string& out) {
    RespParser::appendSimpleString(out, ctx.storage.type(args[1]).value);
}

// Unlike SET, a TTL of 0 or less is allowed: the key expires at once (Redis
// deletes it). Only a time that doesn't fit in 64 bits is an error.
void cmdExpire(CommandContext& ctx, const Args& args, std::string& out) {
    int64_t seconds;
    if(!parseInt64(args[2], seconds)) return RespParser::appendError(out, kNotAnInteger);
    int64_t now = InMemoryStorage::unixTimeMs();
    if(seconds > (INT64_MAX - now) / 1000 || seconds < INT64_MIN / 1000) {
        return RespParser::appendError(out, invalidExpireTime("expire"));
    }
    RespParser::appendInteger(out, ctx.storage.expireAt(args[1], now + seconds * 1000).success ? 1 : 0);
}

// The AOF records every expiry as PEXPIREAT: an absolute Unix time in milliseconds
void cmdPexpireat(CommandContext& ctx, const Args& args, std::string& out) {
    int64_t at;
    if(!parseInt64(args[2], at)) return RespParser::appendError(out, kNotAnInteger);
    RespParser::appendInteger(out, ctx.storage.expireAt(args[1], at).success ? 1 : 0);
}

// As EXPIRE, in milliseconds. Redis checks only that now + ms fits in 64 bits:
// a time far in the past just expires the key
void cmdPexpire(CommandContext& ctx, const Args& args, std::string& out) {
    int64_t ms;
    if(!parseInt64(args[2], ms)) return RespParser::appendError(out, kNotAnInteger);
    int64_t now = InMemoryStorage::unixTimeMs();
    if(ms > INT64_MAX - now) return RespParser::appendError(out, invalidExpireTime("pexpire"));
    RespParser::appendInteger(out, ctx.storage.expireAt(args[1], now + ms).success ? 1 : 0);
}

void cmdTtl(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.ttl(args[1]);
    if(!r.success) return RespParser::appendInteger(out, -2);
    // 64-bit: an expiry set with PEXPIREAT can be further away than INT_MAX
    // seconds, and std::stoi would throw out of the event loop
    RespParser::appendInteger(out, std::stoll(r.value));
}

void cmdPttl(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.pttl(args[1]);
    if(!r.success) return RespParser::appendInteger(out, -2);
    RespParser::appendInteger(out, std::stoll(r.value));
}

// 1 if an expiry was removed; 0 if the key is missing or has none
void cmdPersist(CommandContext& ctx, const Args& args, std::string& out) {
    RespParser::appendInteger(out, ctx.storage.persist(args[1]).success ? 1 : 0);
}

void cmdDbsize(CommandContext& ctx, const Args&, std::string& out) {
    RespParser::appendInteger(out, static_cast<long long>(ctx.storage.dbsize()));
}

// FLUSHALL [ASYNC|SYNC] and FLUSHDB [ASYNC|SYNC]: Kosh has one database, so
// both empty it. Always at once: ASYNC (free in the background) is accepted
// and served synchronously. Any other argument is a syntax error, as in Redis.
void flushKeys(CommandContext& ctx, const Args& args, std::string& out, const char* record) {
    if(args.size() > 2 || (args.size() == 2 && !equalsIgnoreCase(args[1], "sync") &&
                           !equalsIgnoreCase(args[1], "async"))) {
        return RespParser::appendError(out, kSyntaxError);
    }
    ctx.storage.flushAll(record);
    RespParser::appendSimpleString(out, "OK");
}
void cmdFlushall(CommandContext& ctx, const Args& args, std::string& out) { flushKeys(ctx, args, out, "FLUSHALL"); }
void cmdFlushdb(CommandContext& ctx, const Args& args, std::string& out) { flushKeys(ctx, args, out, "FLUSHDB"); }

// SCAN cursor [MATCH pattern] [COUNT count] [TYPE type], as Redis 7.0.15's
// (db.c, scanGenericCommand): options in any order, any case, the last of
// each kind wins; MATCH * is no pattern at all
void cmdScan(CommandContext& ctx, const Args& args, std::string& out) {
    uint64_t cursor;
    if(!parseScanCursor(args[1], cursor)) return RespParser::appendError(out, "ERR invalid cursor");
    long long count = 10;
    const std::string* pattern = nullptr;
    const std::string* type = nullptr;
    for(size_t i = 2; i < args.size(); i += 2) {
        const bool hasValue = args.size() - i >= 2;
        if(hasValue && equalsIgnoreCase(args[i], "count")) {
            int64_t n;
            if(!parseInt64(args[i + 1], n)) return RespParser::appendError(out, kNotAnInteger);
            if(n < 1) return RespParser::appendError(out, kSyntaxError);
            count = n;
        } else if(hasValue && equalsIgnoreCase(args[i], "match")) {
            pattern = (args[i + 1] == "*") ? nullptr : &args[i + 1];
        } else if(hasValue && equalsIgnoreCase(args[i], "type")) {
            type = &args[i + 1];
        } else {
            return RespParser::appendError(out, kSyntaxError);
        }
    }
    auto r = ctx.storage.scan(cursor, count, pattern, type);
    RespParser::appendLength(out, '*', 2);
    RespParser::appendBulk(out, std::to_string(r.cursor));
    RespParser::appendArray(out, r.keys);
}

// ---- Lists ----

void cmdPush(CommandContext& ctx, const Args& args, std::string& out, bool left) {
    std::vector<std::string> values(args.begin() + 2, args.end());
    auto r = left ? ctx.storage.lpush(args[1], values) : ctx.storage.rpush(args[1], values);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendInteger(out, std::stoi(r.value));
}
void cmdLpush(CommandContext& ctx, const Args& args, std::string& out) { cmdPush(ctx, args, out, true); }
void cmdRpush(CommandContext& ctx, const Args& args, std::string& out) { cmdPush(ctx, args, out, false); }

void cmdPop(CommandContext& ctx, const Args& args, std::string& out, bool left) {
    auto r = left ? ctx.storage.lpop(args[1]) : ctx.storage.rpop(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    r.success ? RespParser::appendBulk(out, r.value) : RespParser::appendNull(out, ctx.protocol);
}
void cmdLpop(CommandContext& ctx, const Args& args, std::string& out) { cmdPop(ctx, args, out, true); }
void cmdRpop(CommandContext& ctx, const Args& args, std::string& out) { cmdPop(ctx, args, out, false); }

void cmdLrange(CommandContext& ctx, const Args& args, std::string& out) {
    int64_t start, stop;
    if(!parseInt64(args[2], start) || !parseInt64(args[3], stop)) {
        return RespParser::appendError(out, kNotAnInteger);
    }
    auto r = ctx.storage.lrange(args[1], clampToInt(start), clampToInt(stop));
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendArray(out, r.values);
}

void cmdLlen(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.llen(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendInteger(out, std::stoi(r.value));
}

// ---- Sets ----

void cmdSadd(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.sadd(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendInteger(out, std::stoi(r.value));
}

void cmdSrem(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.srem(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendInteger(out, std::stoi(r.value));
}

void cmdSmembers(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.smembers(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendSetLength(out, r.values.size(), ctx.protocol);
    for(const auto& member : r.values) RespParser::appendBulk(out, member);
}

void cmdSismember(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.sismember(args[1], args[2]);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendInteger(out, std::stoi(r.value));
}

void cmdScard(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.scard(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendInteger(out, std::stoi(r.value));
}

// ---- Hashes ----

void cmdHset(CommandContext& ctx, const Args& args, std::string& out) {
    if(args.size() % 2 != 0) {      // field without a value
        return RespParser::appendError(out, "ERR wrong number of arguments for 'hset' command");
    }
    std::vector<std::pair<std::string, std::string>> fields;
    for(size_t i = 2; i < args.size(); i += 2) fields.emplace_back(args[i], args[i + 1]);
    auto r = ctx.storage.hset(args[1], fields);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendInteger(out, std::stoi(r.value));
}

void cmdHget(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.hget(args[1], args[2]);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    r.success ? RespParser::appendBulk(out, r.value) : RespParser::appendNull(out, ctx.protocol);
}

void cmdHdel(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.hdel(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendInteger(out, std::stoi(r.value));
}

void cmdHgetall(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.hgetall(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    // r.values holds field, value, field, value...: a map in RESP3
    RespParser::appendMapLength(out, r.values.size() / 2, ctx.protocol);
    for(const auto& item : r.values) RespParser::appendBulk(out, item);
}

void cmdHlen(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.hlen(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return RespParser::appendError(out, kWrongType);
    RespParser::appendInteger(out, std::stoi(r.value));
}

// ---- Persistence ----
// There is no SAVE: the AOF is the server's only persistence. (SAVE used to
// write string keys to dump.rdb, which nothing loaded.)

void cmdRewriteAof(CommandContext& ctx, const Args&, std::string& out) {
    if(!ctx.aof) {
        return RespParser::appendError(out, "ERR AOF is disabled (start the server with --appendonly yes)");
    }
    if(!ctx.aof->rewrite(ctx.storage.dumpState())) {
        return RespParser::appendError(out, "ERR AOF rewrite failed: " + ctx.aof->lastError());
    }
    ok(out);
}

// ---- Transactions ----
// MULTI starts queuing (executeCommand queues, see there), EXEC runs the
// queue, DISCARD drops it. Replies and errors are Redis 7.0.15's.

void cmdMulti(CommandContext& ctx, const Args&, std::string& out) {
    if(!ctx.transaction) return RespParser::appendError(out, "ERR MULTI needs a client connection");
    // A nested MULTI is an error, but it doesn't abort the transaction
    if(ctx.transaction->active) return RespParser::appendError(out, "ERR MULTI calls can not be nested");
    *ctx.transaction = Transaction{};
    ctx.transaction->active = true;
    ok(out);
}

void cmdDiscard(CommandContext& ctx, const Args&, std::string& out) {
    if(!ctx.transaction || !ctx.transaction->active) {
        return RespParser::appendError(out, "ERR DISCARD without MULTI");
    }
    *ctx.transaction = Transaction{};
    ok(out);
}

// Runs the queued commands one after another and replies with an array of
// their replies. A command that fails as it runs (WRONGTYPE) doesn't stop
// the others: as in Redis, there is no rollback. Nothing else can run in
// between, since the server runs one command at a time.
//
// In the AOF, a transaction that may write is logged between MULTI and EXEC
// records, and replay applies it only once it reaches the EXEC: a server that
// stops while writing the transaction leaves none of it, not a part.
void cmdExec(CommandContext& ctx, const Args&, std::string& out) {
    if(!ctx.transaction || !ctx.transaction->active) {
        return RespParser::appendError(out, "ERR EXEC without MULTI");
    }
    if(ctx.transaction->aborted) {
        *ctx.transaction = Transaction{};
        return RespParser::appendError(out, "EXECABORT Transaction discarded because of previous errors.");
    }
    Transaction taken = std::move(*ctx.transaction);
    *ctx.transaction = Transaction{};       // so the commands run, instead of being queued
    bool log = taken.writes && ctx.aof;
    if(log) ctx.aof->log("*1\r\n$5\r\nMULTI\r\n");
    RespParser::appendLength(out, '*', taken.queued.size());
    for(const auto& command : taken.queued) executeCommand(ctx, Args(command), out);
    if(log) ctx.aof->log("*1\r\n$4\r\nEXEC\r\n");
}

const Command kCommands[] = {
    // name           arity  flags      handler, first key, last key, key step
    {"ping",            -1,  0,         cmdPing},
    {"auth",            -2,  kCmdNoAuth | kCmdConnection, cmdAuth},
    {"hello",           -1,  kCmdNoAuth | kCmdConnection, cmdHello},
    {"quit",            -1,  kCmdNoAuth | kCmdNotQueued, cmdQuit},
    {"multi",            1,  kCmdNotQueued, cmdMulti},
    {"exec",             1,  kCmdNotQueued, cmdExec},
    {"discard",          1,  kCmdNotQueued, cmdDiscard},
    {"command",         -1,  0,         cmdCommand},
    {"client",          -2,  kCmdConnection, cmdClient},
    {"config",          -2,  0,         cmdConfig},
    {"info",            -1,  kCmdWholeServer, cmdInfo, 0, 0, 0, Merge::Info},

    {"set",             -3,  kCmdWrite, cmdSet, 1, 1, 1},
    {"get",              2,  0,         cmdGet, 1, 1, 1},
    {"setex",            4,  kCmdWrite, cmdSetex, 1, 1, 1},
    {"mget",            -2,  0,         cmdMget, 1, -1, 1, Merge::Values},
    {"mset",            -3,  kCmdWrite, cmdMset, 1, -1, 2},
    {"incr",            2,  kCmdWrite, cmdIncr, 1, 1, 1},
    {"decr",             2,  kCmdWrite, cmdDecr, 1, 1, 1},
    {"incrby",           3,  kCmdWrite, cmdIncrby, 1, 1, 1},
    {"decrby",           3,  kCmdWrite, cmdDecrby, 1, 1, 1},

    {"del",             -2,  kCmdWrite, cmdDel, 1, -1, 1, Merge::Sum},
    {"exists",          -2,  0,         cmdExists, 1, -1, 1, Merge::Sum},
    {"type",             2,  0,         cmdType, 1, 1, 1},
    {"expire",           3,  kCmdWrite, cmdExpire, 1, 1, 1},
    {"pexpire",          3,  kCmdWrite, cmdPexpire, 1, 1, 1},
    {"pexpireat",        3,  kCmdWrite, cmdPexpireat, 1, 1, 1},
    {"persist",          2,  kCmdWrite, cmdPersist, 1, 1, 1},
    {"ttl",              2,  0,         cmdTtl, 1, 1, 1},
    {"pttl",             2,  0,         cmdPttl, 1, 1, 1},
    {"dbsize",           1,  kCmdWholeServer, cmdDbsize, 0, 0, 0, Merge::Sum},
    {"scan",            -2,  kCmdWholeServer, cmdScan, 0, 0, 0, Merge::ScanStep},
    {"flushall",        -1,  kCmdWrite | kCmdWholeServer, cmdFlushall, 0, 0, 0, Merge::AllOk},
    {"flushdb",         -1,  kCmdWrite | kCmdWholeServer, cmdFlushdb, 0, 0, 0, Merge::AllOk},

    {"lpush",           -3,  kCmdWrite, cmdLpush, 1, 1, 1},
    {"rpush",           -3,  kCmdWrite, cmdRpush, 1, 1, 1},
    {"lpop",             2,  kCmdWrite, cmdLpop, 1, 1, 1},
    {"rpop",             2,  kCmdWrite, cmdRpop, 1, 1, 1},
    {"lrange",           4,  0,         cmdLrange, 1, 1, 1},
    {"llen",             2,  0,         cmdLlen, 1, 1, 1},

    {"sadd",            -3,  kCmdWrite, cmdSadd, 1, 1, 1},
    {"srem",            -3,  kCmdWrite, cmdSrem, 1, 1, 1},
    {"smembers",         2,  0,         cmdSmembers, 1, 1, 1},
    {"sismember",        3,  0,         cmdSismember, 1, 1, 1},
    {"scard",            2,  0,         cmdScard, 1, 1, 1},

    {"hset",            -4,  kCmdWrite, cmdHset, 1, 1, 1},
    {"hget",             3,  0,         cmdHget, 1, 1, 1},
    {"hdel",            -3,  kCmdWrite, cmdHdel, 1, 1, 1},
    {"hgetall",          2,  0,         cmdHgetall, 1, 1, 1},
    {"hlen",             2,  0,         cmdHlen, 1, 1, 1},

    // Refused inside MULTI, unlike in Redis: Kosh rewrites in the foreground,
    // so a rewrite in the middle of EXEC would start the new file after the
    // transaction's MULTI record, and replay would find an EXEC without one
    {"bgrewriteaof",    -1,  kCmdNoMulti | kCmdWholeServer, cmdRewriteAof, 0, 0, 0, Merge::AllOk},
    {"aofrewrite",      -1,  kCmdNoMulti | kCmdWholeServer, cmdRewriteAof, 0, 0, 0, Merge::AllOk},
};

struct CommandIndex {
    std::unordered_map<std::string, const Command*> by_name;   // upper-case name
    size_t longest_name = 0;
};

const CommandIndex& commandIndex() {
    static const CommandIndex index = [] {      // built on first use (thread-safe)
        CommandIndex built;
        for(const auto& cmd : kCommands) {
            std::string name = toUpper(cmd.name);
            built.longest_name = std::max(built.longest_name, name.size());
            built.by_name.emplace(std::move(name), &cmd);
        }
        return built;
    }();
    return index;
}

// Undoes InMemoryStorage::setLoading(true) even if a command throws
struct LoadingGuard {
    InMemoryStorage& storage;
    explicit LoadingGuard(InMemoryStorage& s) : storage(s) { storage.setLoading(true); }
    ~LoadingGuard() { storage.setLoading(false); }
};

}  // namespace

// As Redis reads a cursor, with strtoul: "+5" and "-1" (every bit set) are
// accepted, so is "" (0); a leading space, anything after the digits, or more
// than 64 bits is not
bool parseScanCursor(const std::string& text, uint64_t& cursor) {
    char* end = nullptr;
    errno = 0;
    cursor = std::strtoull(text.c_str(), &end, 10);
    return !((!text.empty() && std::isspace(static_cast<unsigned char>(text[0]))) || *end != '\0' || errno == ERANGE);
}

InfoData localInfo(const CommandContext& ctx) {
    InfoData info;
    if(ctx.server) info.server = *ctx.server;
    info.has_server = ctx.server != nullptr;
    info.keys = ctx.storage.stats();
    info.clients = ctx.clients;
    info.aof_enabled = ctx.aof != nullptr;
    info.aof_failing = ctx.aof && ctx.aof->failing();
    return info;
}

// The server-wide fields (port, uptime, the client count, already a total) are
// the receiving thread's own
void mergeInfo(InfoData& into, const InfoData& other) {
    into.server.connections_received += other.server.connections_received;
    into.server.rejected_connections += other.server.rejected_connections;
    into.server.commands_processed += other.server.commands_processed;
    into.server.ops_per_sec += other.server.ops_per_sec;
    into.server.peak_pending_replies = std::max(into.server.peak_pending_replies, other.server.peak_pending_replies);
    into.keys.keyspace_hits += other.keys.keyspace_hits;
    into.keys.keyspace_misses += other.keys.keyspace_misses;
    into.keys.expired_keys += other.keys.expired_keys;
    into.keys.evicted_keys += other.keys.evicted_keys;
    into.keys.keys += other.keys.keys;
    into.keys.expires += other.keys.expires;
    into.keys.max_keys += other.keys.max_keys;
    into.aof_failing = into.aof_failing || other.aof_failing;
}

void appendInfo(const InfoData& info, const Args& args, int protocol, std::string& out) {
    auto wanted = [&](const char* section) {
        if(args.size() == 1) return true;
        for(size_t i = 1; i < args.size(); i++) {
            if(equalsIgnoreCase(args[i], section) || equalsIgnoreCase(args[i], "all") ||
               equalsIgnoreCase(args[i], "default") || equalsIgnoreCase(args[i], "everything")) {
                return true;
            }
        }
        return false;
    };
    const ServerStats& server = info.server;
    const InMemoryStorage::Stats& keys = info.keys;
    std::string text;
    auto section = [&](const char* name) {
        if(!text.empty()) text += "\r\n";
        text += "# ";
        text += name;
        text += "\r\n";
    };
    auto field = [&](const char* name, std::string_view value) {
        text += name;
        text += ':';
        text += value;
        text += "\r\n";
    };
    auto number = [&](const char* name, uint64_t value) { field(name, std::to_string(value)); };

    if(wanted("server")) {
        section("Server");
        field("kosh_version", kKoshVersion);
        number("process_id", processId());
        number("tcp_port", static_cast<uint64_t>(server.tcp_port));
        uint64_t uptime = 0;
        if(info.has_server) {
            uptime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - server.started).count());
        }
        number("uptime_in_seconds", uptime);
        number("uptime_in_days", uptime / 86400);
    }
    if(wanted("clients")) {
        section("Clients");
        number("connected_clients", info.clients);
        number("maxclients", server.max_clients);
    }
    if(wanted("memory")) {
        section("Memory");
        if(size_t rss = residentBytes()) number("used_memory_rss", rss);
        number("maxkeys", keys.max_keys);
    }
    if(wanted("persistence")) {
        section("Persistence");
        number("aof_enabled", info.aof_enabled ? 1 : 0);
        field("aof_last_write_status", info.aof_failing ? "err" : "ok");
    }
    if(wanted("stats")) {
        section("Stats");
        number("total_connections_received", server.connections_received);
        number("total_commands_processed", server.commands_processed);
        number("instantaneous_ops_per_sec", server.ops_per_sec);
        number("rejected_connections", server.rejected_connections);
        // Only ever above 0 with several threads
        if(server.peak_pending_replies > 0) number("peak_pending_replies", server.peak_pending_replies);
        number("expired_keys", keys.expired_keys);
        number("evicted_keys", keys.evicted_keys);
        number("keyspace_hits", keys.keyspace_hits);
        number("keyspace_misses", keys.keyspace_misses);
    }
    if(wanted("keyspace")) {
        section("Keyspace");
        if(keys.keys > 0) {
            field("db0", "keys=" + std::to_string(keys.keys) + ",expires=" + std::to_string(keys.expires));
        }
    }
    RespParser::appendVerbatim(out, text, protocol);
}

const Command* lookupCommand(std::string_view name) {
    const CommandIndex& index = commandIndex();
    // The name comes from the client and can be up to 512 MB. One longer than
    // every command can't match, so don't copy it to upper-case it.
    if(name.size() > index.longest_name) return nullptr;
    auto it = index.by_name.find(toUpper(name));
    return it == index.by_name.end() ? nullptr : it->second;
}

bool constantTimeEquals(std::string_view given, std::string_view secret) {
    if(given.size() > kMaxPasswordLength || secret.size() > kMaxPasswordLength) return false;
    // Zero-padded copies, so the loop always runs over the same 512 bytes.
    // (Copying takes time in proportion to the lengths: the client's own, and
    // the password's, which is the same on every attempt.)
    unsigned char a[kMaxPasswordLength] = {};
    unsigned char b[kMaxPasswordLength] = {};
    if(!given.empty()) std::memcpy(a, given.data(), given.size());
    if(!secret.empty()) std::memcpy(b, secret.data(), secret.size());
    // volatile, so the compiler can't stop at the first difference: the
    // result alone would allow it
    volatile unsigned char diff = 0;
    for(size_t i = 0; i < kMaxPasswordLength; i++) {
        diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
    }
    // Lengths matter too: "abc" padded with zeros equals "abc\0"
    return diff == 0 && given.size() == secret.size();
}

std::vector<const Command*> allCommands() {
    std::vector<const Command*> all;
    for(const auto& cmd : kCommands) all.push_back(&cmd);
    return all;
}

bool arityMatches(const Command& cmd, size_t argc) {
    if(cmd.arity >= 0) return argc == static_cast<size_t>(cmd.arity);
    return argc >= static_cast<size_t>(-cmd.arity);
}

// Redis's unknown-command error, byte for byte (server.c in 7.0.15): the name
// (128 bytes at most), then the arguments, each quoted, until 128 bytes of
// them are written. Each stops at a NUL byte, as C's %s does. Some clients
// and tests match on this text.
std::string unknownCommandError(const Args& args) {
    auto asCString = [](const std::string& s) {
        return std::string_view(s.data(), std::min(s.size(), s.find('\0')));
    };
    std::string quoted;
    for(size_t i = 1; i < args.size() && quoted.size() < 128; i++) {
        size_t room = 128 - quoted.size();
        quoted += '\'';
        quoted += asCString(args[i]).substr(0, room);
        quoted += "' ";
    }
    return "ERR unknown command '" + std::string(asCString(args[0]).substr(0, 128)) +
           "', with args beginning with: " + quoted;
}

void executeCommand(CommandContext& ctx, const Args& args, std::string& out) {
    if(args.empty()) return RespParser::appendError(out, "ERR empty command");
    Transaction* tx = ctx.transaction;
    const bool in_multi = tx && tx->active;
    const Command* cmd = lookupCommand(args[0]);

    // A command refused before it runs, as Redis's rejectCommand: inside
    // MULTI it also aborts the transaction, and a refused EXEC discards it,
    // with an EXECABORT that says why
    auto reject = [&](const std::string& error) {
        if(in_multi) tx->aborted = true;
        if(cmd && cmd->handler == cmdExec) {
            if(tx) *tx = Transaction{};
            return RespParser::appendError(out, "EXECABORT Transaction discarded because of: " + error);
        }
        RespParser::appendError(out, error);
    };

    if(!cmd) return reject(unknownCommandError(args));
    if(!arityMatches(*cmd, args.size())) {
        return reject(std::string("ERR wrong number of arguments for '") + cmd->name + "' command");
    }
    // After the two checks above, as in Redis (checked on 7.0.15). They tell a
    // client without the password only what is public: the command names and
    // how many arguments each takes.
    if(!ctx.authenticated && !(cmd->flags & kCmdNoAuth)) return reject("NOAUTH Authentication required.");
    if(in_multi && (cmd->flags & kCmdNoMulti)) return reject("ERR Command not allowed inside a transaction");
    // While the AOF can't be written (a full disk), refuse writes, as Redis
    // does: their records couldn't be saved. Reads still work. Each attempt
    // retries first, so writes resume as soon as the file takes data again.
    // An EXEC whose transaction writes is refused whole, as in Redis.
    bool writes = (cmd->flags & kCmdWrite) || (cmd->handler == cmdExec && in_multi && tx->writes);
    if(writes && ctx.aof && !ctx.aof->flushPending()) {
        return reject("MISCONF Errors writing to the AOF file: " + ctx.aof->lastWriteError());
    }

    // Inside MULTI, queue it: it runs at EXEC
    if(in_multi && !(cmd->flags & kCmdNotQueued)) {
        tx->queued.emplace_back(args.begin(), args.end());
        if(cmd->flags & kCmdWrite) tx->writes = true;
        return RespParser::appendSimpleString(out, "QUEUED");
    }
    cmd->handler(ctx, args, out);
    // As Redis counts total_commands_processed: once a command has run (so an
    // INFO doesn't count itself), EXEC and each command it runs included;
    // refused or queued ones not
    if(ctx.server) ctx.server->commands_processed++;
}

void executeCommand(CommandContext& ctx, const std::vector<std::string>& args, std::string& out) {
    executeCommand(ctx, Args(args), out);
}

std::string executeCommand(CommandContext& ctx, const std::vector<std::string>& args) {
    std::string reply;
    executeCommand(ctx, Args(args), reply);
    return reply;
}

// Runs every complete command in the AOF through the command table. While
// loading, the storage doesn't log the commands again (see setLoading).
AofLoadResult replayAOF(InMemoryStorage& storage, AOFLogger& aof, const Router* router, size_t shard) {
    AofLoadResult result;
    const std::string buffer = aof.readRaw();
    CommandContext ctx{storage};
    LoadingGuard loading(storage);
    std::string discarded;      // replies: nobody reads them
    std::vector<std::string> scratch;   // every record is parsed into these strings

    auto damaged = [&](size_t at, const std::string& reason) {
        result.ok = false;
        result.error = "the AOF is damaged at byte " + std::to_string(at) + " (" + reason +
                       "), so what follows can't be read. The " + std::to_string(at) +
                       " bytes before it are intact: to start with them, cut the file there"
                       " (truncate -s " + std::to_string(at) + " " + aof.getFilename() + ")";
        return result;
    };

    auto belongsHere = [&](const Command& cmd, const Args& args) {
        if(!router || cmd.first_key == 0) return true;
        const size_t last = cmd.last_key < 0 ? args.size() - 1 : static_cast<size_t>(cmd.last_key);
        for(size_t i = static_cast<size_t>(cmd.first_key); i <= last; i += static_cast<size_t>(cmd.key_step)) {
            if(router->shardOf(args[i]) != shard) return false;
        }
        return true;
    };

    // A transaction's records, from its MULTI on: applied together at its
    // EXEC, so one the server stopped writing partway leaves nothing behind
    bool in_transaction = false;
    size_t transaction_start = 0;
    std::vector<std::vector<std::string>> transaction;
    auto run = [&](const Command& cmd, const Args& args) {
        discarded.clear();
        cmd.handler(ctx, args, discarded);
        result.commands++;
    };

    // Parse from a moving position. Erasing each command from the front of the
    // buffer moved the whole rest of the file every time: quadratic in its size.
    size_t pos = 0;
    size_t incomplete_at = buffer.size();       // where a record cut short begins
    while(pos < buffer.size()) {
        auto parsed = RespParser::parseInto(std::string_view(buffer).substr(pos), scratch);
        if(parsed.status == RespParser::ParseStatus::NeedMore) {
            incomplete_at = pos;
            break;
        }
        if(parsed.status == RespParser::ParseStatus::Error) return damaged(pos, parsed.error);

        Args args(scratch.data(), parsed.argc);
        if(!args.empty()) {
            if(args.size() == 1 && equalsIgnoreCase(args[0], "MULTI")) {
                if(in_transaction) return damaged(pos, "MULTI inside a transaction");
                in_transaction = true;
                transaction_start = pos;
                transaction.clear();
            } else if(args.size() == 1 && equalsIgnoreCase(args[0], "EXEC")) {
                if(!in_transaction) return damaged(pos, "EXEC without MULTI");
                for(const auto& command : transaction) {
                    Args queued(command);
                    run(*lookupCommand(queued[0]), queued);
                }
                in_transaction = false;
                transaction.clear();
            } else {
                // Only write commands are logged, so anything else means the file is damaged
                const Command* cmd = lookupCommand(args[0]);
                if(!cmd || !(cmd->flags & kCmdWrite) || !arityMatches(*cmd, args.size())) {
                    return damaged(pos, "unexpected command '" + args[0].substr(0, 128) + "'");
                }
                if(!belongsHere(*cmd, args)) return damaged(pos, "a key that belongs to another thread's file");
                if(in_transaction) transaction.emplace_back(args.begin(), args.end());
                else run(*cmd, args);
            }
        }
        pos += parsed.consumed;
    }

    // The file ends partway through a record, or a transaction without its
    // EXEC: the server stopped while writing it. Cut it off, or new records
    // would be appended after it and read as its missing part at the next
    // start (Redis also does this, with aof-load-truncated). A length damaged
    // into a larger one looks the same and would cut more; nothing in the
    // file tells them apart.
    size_t cut = in_transaction ? transaction_start : incomplete_at;
    if(cut < buffer.size()) {
        if(!aof.truncate(cut)) {
            result.ok = false;
            result.error = "cannot cut off the incomplete record at byte " +
                           std::to_string(cut) + ": " + aof.lastError();
            return result;
        }
        result.truncated_bytes = buffer.size() - cut;
    }
    return result;
}
