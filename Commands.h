#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "InMemoryStorage.h"

// The command table, shared by the server and AOF replay.
//
// Redis runs client commands and the AOF it replays at startup through the same
// table. Mini-Redis used to have two paths, the server's dispatch and an if-chain
// in replay, and they already disagreed: replay matched names case-sensitively
// and ignored extra arguments. With one table, a command can't do one thing when
// a client sends it and another when it is replayed.

// What a command can use besides its arguments
struct CommandContext {
    InMemoryStorage& storage;
    AOFLogger* aof = nullptr;       // nullptr when the AOF is off (BGREWRITEAOF)
    size_t clients = 0;             // connected clients (INFO)
    bool close_connection = false;  // set by QUIT: close once its reply is sent
};

using CommandHandler = std::string (*)(CommandContext& ctx, const std::vector<std::string>& args);

struct Command {
    const char* name;       // lower case, as Redis's error messages quote it
    int arity;              // arguments including the name; -N means at least N
    unsigned flags;
    CommandHandler handler;
};

// The command changes data. Only these are logged, so only these can be replayed.
constexpr unsigned kCmdWrite = 1u << 0;

// Case-insensitive; nullptr if there is no such command
const Command* lookupCommand(std::string_view name);
bool arityMatches(const Command& cmd, size_t argc);

// Runs one command (args[0] is its name) and returns its RESP reply
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
