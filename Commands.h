#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "InMemoryStorage.h"

// The command table, shared by the server and AOF replay.
//
// Redis runs client commands and the AOF it replays at startup through the same
// table. Kosh used to have two paths, the server's dispatch and an if-chain
// in replay, and they already disagreed: replay matched names case-sensitively
// and ignored extra arguments. With one table, a command can't do one thing when
// a client sends it and another when it is replayed.

// A client's transaction, from MULTI to EXEC or DISCARD. Commands are checked
// and queued, then run together at EXEC: the server runs one command at a
// time, so nothing else runs in between.
struct Transaction {
    bool active = false;        // after MULTI
    bool aborted = false;       // a command was refused while queuing: EXEC refuses (EXECABORT)
    bool writes = false;        // a queued command changes data
    std::vector<std::vector<std::string>> queued;
};

// What INFO reports about the server, which keeps it (see Server.cpp)
struct ServerStats {
    int tcp_port = 0;
    size_t max_clients = 0;
    // On the monotonic clock, so a step of the wall clock doesn't change the uptime
    std::chrono::steady_clock::time_point started{};
    uint64_t connections_received = 0;      // accepted and served
    uint64_t rejected_connections = 0;      // refused: maxclients, protected mode, out of files
    uint64_t commands_processed = 0;        // commands that ran (executeCommand counts them)
    uint64_t ops_per_sec = 0;               // commands per second, recently (see Server.cpp)
};

// What a command can use besides its arguments
struct CommandContext {
    InMemoryStorage& storage;
    AOFLogger* aof = nullptr;       // nullptr when the AOF is off (BGREWRITEAOF)
    size_t clients = 0;             // connected clients (INFO)
    bool close_connection = false;  // set by QUIT: close once its reply is sent
    // The server's password (--requirepass), or nullptr if none is set
    const std::string* password = nullptr;
    // Whether this client may run commands: with a password set, only once
    // AUTH has succeeded. The server keeps it per client; AUTH sets it.
    bool authenticated = true;
    // The protocol this client speaks: RESP2 until HELLO 3 (HELLO sets it).
    // Replies that are a null, a map or a set are written in it.
    int protocol = 2;
    uint64_t client_id = 0;                 // CLIENT ID, HELLO
    std::string* client_name = nullptr;     // CLIENT SETNAME/GETNAME; nullptr: no connection
    Transaction* transaction = nullptr;     // MULTI/EXEC; nullptr: no connection
    ServerStats* server = nullptr;          // INFO, and counting commands; nullptr: no server
};

// A command's arguments (args[0] is its name): a view of strings the caller
// owns. The server parses every command into the same strings, which keep their
// memory from one command to the next, so it needs no vector per command.
class Args {
public:
    Args(const std::string* data, size_t size) : data_(data), size_(size) {}
    Args(const std::vector<std::string>& args) : data_(args.data()), size_(args.size()) {}

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    const std::string& operator[](size_t i) const { return data_[i]; }
    const std::string* begin() const { return data_; }
    const std::string* end() const { return data_ + size_; }

private:
    const std::string* data_;
    size_t size_;
};

// Appends the command's RESP reply to `out`
using CommandHandler = void (*)(CommandContext& ctx, const Args& args, std::string& out);

struct Command {
    const char* name;       // lower case, as Redis's error messages quote it
    int arity;              // arguments including the name; -N means at least N
    unsigned flags;
    CommandHandler handler;
};

// The command changes data. Only these are logged, so only these can be replayed.
constexpr unsigned kCmdWrite = 1u << 0;
// The command runs before the client has authenticated (AUTH, HELLO, QUIT),
// as Redis's no-auth flag
constexpr unsigned kCmdNoAuth = 1u << 1;
// Inside MULTI, the command runs at once instead of being queued (MULTI,
// EXEC, DISCARD, QUIT), as in Redis
constexpr unsigned kCmdNotQueued = 1u << 2;
// Refused inside MULTI, which it then aborts: "Command not allowed inside a
// transaction", Redis's no-multi flag
constexpr unsigned kCmdNoMulti = 1u << 3;

// Longest password accepted (Redis 5's CONFIG_AUTHPASS_MAX_LEN). With a
// bound, the comparison below can always take the same time.
constexpr size_t kMaxPasswordLength = 512;

// Whether `given` equals `secret`, in a time that doesn't depend on how much
// of `given` is right. std::string's == stops at the first differing byte,
// so its timing would tell a client how much of a guess is right, and it
// could find the password a byte at a time. Both are compared as
// kMaxPasswordLength zero-padded bytes, as Redis's time_independent_strcmp
// did. Only the lengths affect the time, through the copies: the client's
// own, and the password's, which is the same on every attempt. A `given`
// longer than kMaxPasswordLength is rejected at once, which also depends only
// on the client's input. bench/auth_compare.cpp measures it. `secret` must
// be at most kMaxPasswordLength bytes.
bool constantTimeEquals(std::string_view given, std::string_view secret);

// Case-insensitive; nullptr if there is no such command
const Command* lookupCommand(std::string_view name);
bool arityMatches(const Command& cmd, size_t argc);

// Runs one command (args[0] is its name) and appends its RESP reply to `out`.
// The server passes the client's output buffer, so a reply is built in place.
// Checks, in Redis's order: unknown command, arity, authentication (NOAUTH),
// a command refused inside MULTI, then whether a write can be logged
// (MISCONF). Inside MULTI, a command that passes is queued (+QUEUED); one
// that doesn't aborts the transaction.
void executeCommand(CommandContext& ctx, const Args& args, std::string& out);

// The same, for arguments in a vector of their own, appending, or returning
// the reply (for tests and tools)
void executeCommand(CommandContext& ctx, const std::vector<std::string>& args, std::string& out);
std::string executeCommand(CommandContext& ctx, const std::vector<std::string>& args);

// What replayAOF() found in the file
struct AofLoadResult {
    bool ok = true;                 // false: the data couldn't all be loaded; don't start
    size_t commands = 0;            // commands run
    size_t truncated_bytes = 0;     // an incomplete last record, cut off the end
    std::string error;              // why it isn't ok
};

// Rebuilds the data from the AOF by running its commands through the same
// table. An incomplete last record (the server stopped while writing it) is
// cut off the file, so new records start at a record boundary. Damage before
// the end, or a command the server never logs, is an error: loading only the
// part before it would silently drop everything after.
AofLoadResult replayAOF(InMemoryStorage& storage, AOFLogger& aof);
