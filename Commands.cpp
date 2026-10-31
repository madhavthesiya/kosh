#include "Commands.h"
#include "RespParser.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <unordered_map>

namespace {

using Args = std::vector<std::string>;

std::string toUpper(std::string_view s) {
    std::string upper(s);
    // std::toupper needs an unsigned char value: passing a plain char that is
    // negative (a byte >= 0x80 where char is signed) is undefined behaviour.
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return upper;
}

std::string ok() { return RespParser::simpleString("OK"); }

std::string wrongType() {
    return RespParser::error("WRONGTYPE Operation against a key holding the wrong kind of value");
}

// Redis's error replies, word for word
std::string notAnInteger() { return RespParser::error("ERR value is not an integer or out of range"); }
std::string syntaxError() { return RespParser::error("ERR syntax error"); }
std::string invalidExpireTime(const char* command) {
    return RespParser::error(std::string("ERR invalid expire time in '") + command + "' command");
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
// in 64 bits. Returns an error reply, or an empty string if it is valid.
std::string parseExpiry(const std::string& text, bool seconds, bool relative,
                        const char* command, int64_t& unix_time_ms) {
    int64_t value;
    if(!parseInt64(text, value)) return notAnInteger();
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

std::string cmdPing(CommandContext&, const Args&) { return RespParser::simpleString("PONG"); }

std::string cmdQuit(CommandContext& ctx, const Args&) {
    ctx.close_connection = true;
    return ok();
}

// redis-cli and redis-benchmark send these at startup
std::string cmdCommand(CommandContext&, const Args&) { return RespParser::emptyArray(); }
std::string cmdClient(CommandContext&, const Args&) { return ok(); }

std::string cmdConfig(CommandContext&, const Args& args) {
    if(equalsIgnoreCase(args[1], "GET")) return RespParser::emptyArray();
    return RespParser::error("ERR unknown subcommand '" + args[1].substr(0, 128) + "'");
}

std::string cmdInfo(CommandContext& ctx, const Args&) {
    return RespParser::bulkString("# Server\r\nmini_redis_version:2.0\r\n"
                                  "# Clients\r\nconnected_clients:"
                                  + std::to_string(ctx.clients) + "\r\n");
}

// ---- Strings ----

// SET key value [EX seconds | PX milliseconds | EXAT unix-seconds | PXAT unix-milliseconds]
// The AOF records every SET with a TTL as PXAT. Redis's other options (NX, XX,
// GET, KEEPTTL) aren't supported and are a syntax error: ignoring NX would
// let "SET lock me NX" take a lock that someone else holds.
std::string cmdSet(CommandContext& ctx, const Args& args) {
    // As Redis does: check the options' syntax first, then the number
    const std::string* option = nullptr;
    const std::string* number = nullptr;
    for(size_t i = 3; i < args.size(); i += 2) {
        bool known = equalsIgnoreCase(args[i], "EX") || equalsIgnoreCase(args[i], "PX") ||
                     equalsIgnoreCase(args[i], "EXAT") || equalsIgnoreCase(args[i], "PXAT");
        // Each option needs a value, and only one expiry may be given
        if(!known || option || i + 1 == args.size()) return syntaxError();
        option = &args[i];
        number = &args[i + 1];
    }
    if(!option) {
        auto r = ctx.storage.set(args[1], args[2]);
        return r.success ? ok() : RespParser::error("ERR set failed");
    }
    bool seconds = equalsIgnoreCase(*option, "EX") || equalsIgnoreCase(*option, "EXAT");
    bool relative = equalsIgnoreCase(*option, "EX") || equalsIgnoreCase(*option, "PX");
    int64_t at;
    std::string error = parseExpiry(*number, seconds, relative, "set", at);
    if(!error.empty()) return error;
    auto r = ctx.storage.setExpiringAt(args[1], args[2], at);
    return r.success ? ok() : RespParser::error("ERR set failed");
}

std::string cmdGet(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.get(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return r.success ? RespParser::bulkString(r.value) : RespParser::nullBulk();
}

std::string cmdSetex(CommandContext& ctx, const Args& args) {
    int64_t at;
    std::string error = parseExpiry(args[2], true, true, "setex", at);
    if(!error.empty()) return error;
    auto r = ctx.storage.setExpiringAt(args[1], args[3], at);
    return r.success ? ok() : RespParser::error("ERR setex failed");
}

// ---- Keys ----

std::string cmdDel(CommandContext& ctx, const Args& args) {
    return RespParser::integer(ctx.storage.del(args[1]).success ? 1 : 0);
}

std::string cmdExists(CommandContext& ctx, const Args& args) {
    return RespParser::integer(ctx.storage.exists(args[1]).success ? 1 : 0);
}

std::string cmdType(CommandContext& ctx, const Args& args) {
    return RespParser::simpleString(ctx.storage.type(args[1]).value);
}

// Unlike SET, a TTL of 0 or less is allowed: the key expires at once (Redis
// deletes it). Only a time that doesn't fit in 64 bits is an error.
std::string cmdExpire(CommandContext& ctx, const Args& args) {
    int64_t seconds;
    if(!parseInt64(args[2], seconds)) return notAnInteger();
    int64_t now = InMemoryStorage::unixTimeMs();
    if(seconds > (INT64_MAX - now) / 1000 || seconds < INT64_MIN / 1000) {
        return invalidExpireTime("expire");
    }
    return RespParser::integer(ctx.storage.expireAt(args[1], now + seconds * 1000).success ? 1 : 0);
}

// The AOF records every expiry as PEXPIREAT: an absolute Unix time in milliseconds
std::string cmdPexpireat(CommandContext& ctx, const Args& args) {
    int64_t at;
    if(!parseInt64(args[2], at)) return notAnInteger();
    return RespParser::integer(ctx.storage.expireAt(args[1], at).success ? 1 : 0);
}

std::string cmdTtl(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.ttl(args[1]);
    if(!r.success) return RespParser::integer(-2);
    // 64-bit: an expiry set with PEXPIREAT can be further away than INT_MAX
    // seconds, and std::stoi would throw out of the event loop
    return RespParser::integer(std::stoll(r.value));
}

// ---- Lists ----

std::string cmdPush(CommandContext& ctx, const Args& args, bool left) {
    std::vector<std::string> values(args.begin() + 2, args.end());
    auto r = left ? ctx.storage.lpush(args[1], values) : ctx.storage.rpush(args[1], values);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::integer(std::stoi(r.value));
}
std::string cmdLpush(CommandContext& ctx, const Args& args) { return cmdPush(ctx, args, true); }
std::string cmdRpush(CommandContext& ctx, const Args& args) { return cmdPush(ctx, args, false); }

std::string cmdPop(CommandContext& ctx, const Args& args, bool left) {
    auto r = left ? ctx.storage.lpop(args[1]) : ctx.storage.rpop(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return r.success ? RespParser::bulkString(r.value) : RespParser::nullBulk();
}
std::string cmdLpop(CommandContext& ctx, const Args& args) { return cmdPop(ctx, args, true); }
std::string cmdRpop(CommandContext& ctx, const Args& args) { return cmdPop(ctx, args, false); }

std::string cmdLrange(CommandContext& ctx, const Args& args) {
    int64_t start, stop;
    if(!parseInt64(args[2], start) || !parseInt64(args[3], stop)) return notAnInteger();
    auto r = ctx.storage.lrange(args[1], clampToInt(start), clampToInt(stop));
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::array(r.values);
}

std::string cmdLlen(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.llen(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::integer(std::stoi(r.value));
}

// ---- Sets ----

std::string cmdSadd(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.sadd(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::integer(std::stoi(r.value));
}

std::string cmdSrem(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.srem(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::integer(std::stoi(r.value));
}

std::string cmdSmembers(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.smembers(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::array(r.values);
}

std::string cmdSismember(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.sismember(args[1], args[2]);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::integer(std::stoi(r.value));
}

std::string cmdScard(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.scard(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::integer(std::stoi(r.value));
}

// ---- Hashes ----

std::string cmdHset(CommandContext& ctx, const Args& args) {
    if(args.size() % 2 != 0) {      // field without a value
        return RespParser::error("ERR wrong number of arguments for 'hset' command");
    }
    std::vector<std::pair<std::string, std::string>> fields;
    for(size_t i = 2; i < args.size(); i += 2) fields.emplace_back(args[i], args[i + 1]);
    auto r = ctx.storage.hset(args[1], fields);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::integer(std::stoi(r.value));
}

std::string cmdHget(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.hget(args[1], args[2]);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return r.success ? RespParser::bulkString(r.value) : RespParser::nullBulk();
}

std::string cmdHdel(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.hdel(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::integer(std::stoi(r.value));
}

std::string cmdHgetall(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.hgetall(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::array(r.values);
}

std::string cmdHlen(CommandContext& ctx, const Args& args) {
    auto r = ctx.storage.hlen(args[1]);
    if(r.error == ErrorCode::WRONG_TYPE) return wrongType();
    return RespParser::integer(std::stoi(r.value));
}

// ---- Persistence ----
// There is no SAVE: the AOF is the server's only persistence. (SAVE used to
// write string keys to dump.rdb, which nothing loaded.)

std::string cmdRewriteAof(CommandContext& ctx, const Args&) {
    if(!ctx.aof) return RespParser::error("ERR AOF is disabled (start the server with --appendonly yes)");
    ctx.aof->rewrite(ctx.storage.dumpState());
    return ok();
}

const Command kCommands[] = {
    // name           arity  flags      handler
    {"ping",            -1,  0,         cmdPing},
    {"quit",            -1,  0,         cmdQuit},
    {"command",         -1,  0,         cmdCommand},
    {"client",          -1,  0,         cmdClient},
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

bool arityMatches(const Command& cmd, size_t argc) {
    if(cmd.arity >= 0) return argc == static_cast<size_t>(cmd.arity);
    return argc >= static_cast<size_t>(-cmd.arity);
}

std::string executeCommand(CommandContext& ctx, const std::vector<std::string>& args) {
    if(args.empty()) return RespParser::error("ERR empty command");
    const Command* cmd = lookupCommand(args[0]);
    if(!cmd) {
        // Quote at most 128 bytes of the name, as Redis does
        return RespParser::error("ERR unknown command '" + args[0].substr(0, 128) + "'");
    }
    if(!arityMatches(*cmd, args.size())) {
        return RespParser::error(std::string("ERR wrong number of arguments for '")
                                 + cmd->name + "' command");
    }
    return cmd->handler(ctx, args);
}

// Runs every complete command in the AOF through the command table. While
// loading, the storage doesn't log the commands again.
Result replayAOF(InMemoryStorage& storage, AOFLogger& aof) {
    std::string buffer = aof.readRaw();
    CommandContext ctx{storage};
    LoadingGuard loading(storage);

    while(!buffer.empty()) {
        auto parsed = RespParser::parse(buffer);
        if(parsed.status != RespParser::ParseStatus::Ok) break;    // incomplete or invalid tail
        buffer.erase(0, parsed.consumed);
        const auto& args = parsed.args;
        if(args.empty()) continue;

        // Only write commands are logged, so anything else can't have come from
        // the server: skip it
        const Command* cmd = lookupCommand(args[0]);
        if(!cmd || !(cmd->flags & kCmdWrite) || !arityMatches(*cmd, args.size())) continue;
        cmd->handler(ctx, args);
    }
    return Result::ok();
}
