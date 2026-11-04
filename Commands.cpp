#include "Commands.h"
#include "RespParser.h"
#include "Version.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cstring>
#include <iterator>
#include <unordered_map>

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

// The whole of `text` as a 64-bit integer. std::from_chars, unlike std::stoll,
// rejects trailing garbage such as "10abc" and reports overflow instead of throwing.
bool parseInt64(const std::string& text, int64_t& out) {
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
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

void cmdPing(CommandContext&, const Args&, std::string& out) {
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

void cmdInfo(CommandContext& ctx, const Args&, std::string& out) {
    RespParser::appendVerbatim(out, std::string("# Server\r\nkosh_version:") + kKoshVersion + "\r\n"
                               "# Clients\r\nconnected_clients:" + std::to_string(ctx.clients) + "\r\n",
                               ctx.protocol);
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

// ---- Keys ----

void cmdDel(CommandContext& ctx, const Args& args, std::string& out) {
    RespParser::appendInteger(out, ctx.storage.del(args[1]).success ? 1 : 0);
}

void cmdExists(CommandContext& ctx, const Args& args, std::string& out) {
    RespParser::appendInteger(out, ctx.storage.exists(args[1]).success ? 1 : 0);
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

void cmdTtl(CommandContext& ctx, const Args& args, std::string& out) {
    auto r = ctx.storage.ttl(args[1]);
    if(!r.success) return RespParser::appendInteger(out, -2);
    // 64-bit: an expiry set with PEXPIREAT can be further away than INT_MAX
    // seconds, and std::stoi would throw out of the event loop
    RespParser::appendInteger(out, std::stoll(r.value));
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

const Command kCommands[] = {
    // name           arity  flags      handler
    {"ping",            -1,  0,         cmdPing},
    {"auth",            -2,  kCmdNoAuth, cmdAuth},
    {"hello",           -1,  kCmdNoAuth, cmdHello},
    {"quit",            -1,  kCmdNoAuth, cmdQuit},
    {"command",         -1,  0,         cmdCommand},
    {"client",          -2,  0,         cmdClient},
    {"config",          -2,  0,         cmdConfig},
    {"info",            -1,  0,         cmdInfo},

    {"set",             -3,  kCmdWrite, cmdSet},
    {"get",              2,  0,         cmdGet},
    {"setex",            4,  kCmdWrite, cmdSetex},

    {"del",              2,  kCmdWrite, cmdDel},
    {"exists",           2,  0,         cmdExists},
    {"type",             2,  0,         cmdType},
    {"expire",           3,  kCmdWrite, cmdExpire},
    {"pexpireat",        3,  kCmdWrite, cmdPexpireat},
    {"ttl",              2,  0,         cmdTtl},

    {"lpush",           -3,  kCmdWrite, cmdLpush},
    {"rpush",           -3,  kCmdWrite, cmdRpush},
    {"lpop",             2,  kCmdWrite, cmdLpop},
    {"rpop",             2,  kCmdWrite, cmdRpop},
    {"lrange",           4,  0,         cmdLrange},
    {"llen",             2,  0,         cmdLlen},

    {"sadd",            -3,  kCmdWrite, cmdSadd},
    {"srem",            -3,  kCmdWrite, cmdSrem},
    {"smembers",         2,  0,         cmdSmembers},
    {"sismember",        3,  0,         cmdSismember},
    {"scard",            2,  0,         cmdScard},

    {"hset",            -4,  kCmdWrite, cmdHset},
    {"hget",             3,  0,         cmdHget},
    {"hdel",            -3,  kCmdWrite, cmdHdel},
    {"hgetall",          2,  0,         cmdHgetall},
    {"hlen",             2,  0,         cmdHlen},

    {"bgrewriteaof",    -1,  0,         cmdRewriteAof},
    {"aofrewrite",      -1,  0,         cmdRewriteAof},
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
    const Command* cmd = lookupCommand(args[0]);
    if(!cmd) return RespParser::appendError(out, unknownCommandError(args));
    if(!arityMatches(*cmd, args.size())) {
        return RespParser::appendError(out, std::string("ERR wrong number of arguments for '")
                                            + cmd->name + "' command");
    }
    // After the two checks above, as in Redis (checked on 7.0.15). They tell a
    // client without the password only what is public: the command names and
    // how many arguments each takes.
    if(!ctx.authenticated && !(cmd->flags & kCmdNoAuth)) {
        return RespParser::appendError(out, "NOAUTH Authentication required.");
    }
    // While the AOF can't be written (a full disk), refuse writes, as Redis
    // does: their records couldn't be saved. Reads still work. Each attempt
    // retries first, so writes resume as soon as the file takes data again.
    if((cmd->flags & kCmdWrite) && ctx.aof && !ctx.aof->flushPending()) {
        return RespParser::appendError(out, "MISCONF Errors writing to the AOF file: " +
                                            ctx.aof->lastWriteError());
    }
    cmd->handler(ctx, args, out);
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
AofLoadResult replayAOF(InMemoryStorage& storage, AOFLogger& aof) {
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

    // Parse from a moving position. Erasing each command from the front of the
    // buffer moved the whole rest of the file every time: quadratic in its size.
    size_t pos = 0;
    while(pos < buffer.size()) {
        auto parsed = RespParser::parseInto(std::string_view(buffer).substr(pos), scratch);
        if(parsed.status == RespParser::ParseStatus::NeedMore) {
            // The file ends partway through a record: the server stopped while
            // writing it. Cut it off, or new records would be appended after it
            // and read as its missing part at the next start (Redis also does
            // this, with aof-load-truncated). A length damaged into a larger one
            // looks the same and would cut more; nothing in the file tells them apart.
            if(!aof.truncate(pos)) {
                result.ok = false;
                result.error = "cannot cut off the incomplete record at byte " +
                               std::to_string(pos) + ": " + aof.lastError();
                return result;
            }
            result.truncated_bytes = buffer.size() - pos;
            break;
        }
        if(parsed.status == RespParser::ParseStatus::Error) return damaged(pos, parsed.error);

        Args args(scratch.data(), parsed.argc);
        if(!args.empty()) {
            // Only write commands are logged, so anything else means the file is damaged
            const Command* cmd = lookupCommand(args[0]);
            if(!cmd || !(cmd->flags & kCmdWrite) || !arityMatches(*cmd, args.size())) {
                return damaged(pos, "unexpected command '" + args[0].substr(0, 128) + "'");
            }
            discarded.clear();
            cmd->handler(ctx, args, discarded);
            result.commands++;
        }
        pos += parsed.consumed;
    }
    return result;
}
