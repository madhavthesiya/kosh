// Fuzz target: bytes from a client, through the RESP parser and the command
// table, the way Server::runCommands handles them.
//
// libFuzzer's interface, so clang can build it coverage-guided:
//   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DKOSH_LIBFUZZER=ON
//         -DSANITIZE=address,undefined
//   cmake --build build-fuzz --target fuzz_commands
//   ./build-fuzz/fuzz_commands tests/fuzz/corpus
// With GCC, standalone_main.cpp replays a corpus or mutates it at random
// (CMake target fuzz_commands).
//
// Input: byte 0 holds options (bit 0: the server has a password, so the
// client starts unauthenticated), byte 1 where to split the rest into two
// reads; the rest is what the client sends.
//
// Besides crashes, leaks and undefined behaviour (with sanitizers), it checks:
// - the parser never claims more bytes than it was given, or more arguments
//   than it filled, and every command gets a reply;
// - the same bytes in two reads parse into exactly the same commands, and end
//   the same way, as in one read. The server keeps unparsed bytes and parses
//   again when more arrive, so where a read ends must not change a command.
// Commands run in the one-read pass only: their replies can depend on the
// clock (TTL), so comparing replies would flag time, not bugs. The two-read
// pass takes AUTH's and QUIT's effects from it, in order.

#include "../../src/Commands.h"
#include "../../src/InMemoryStorage.h"
#include "../../src/RespParser.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool ok, const char* what) {
    if(!ok) {
        std::fprintf(stderr, "fuzz_commands: invariant broken: %s\n", what);
        std::abort();
    }
}

// What the server parsed from a client: every command's arguments, and how
// parsing ended
struct Parsed {
    std::vector<std::vector<std::string>> commands;
    std::string ending;     // "need more", "closed" (QUIT), or the protocol error
    bool operator==(const Parsed& o) const { return commands == o.commands && ending == o.ending; }
};

// What running the commands did that changes how later bytes are handled
struct Outcomes {
    std::vector<bool> authenticated_after;  // per command
    bool closed = false;                    // the last command was QUIT
};

enum class Step { Command, NeedMore, Error };

// Parses the next command from `in` at `pos`, as runCommands does, and
// records it (or how parsing ended) in `result`
Step parseNext(const std::string& in, size_t& pos, std::vector<std::string>& args,
               bool authenticated, Parsed& result, size_t& argc) {
    while(true) {
        std::string_view rest = std::string_view(in).substr(pos);
        auto parsed = RespParser::parseInto(rest, args, authenticated);
        if(parsed.status == RespParser::ParseStatus::NeedMore) {
            result.ending = "need more";
            return Step::NeedMore;
        }
        if(parsed.status == RespParser::ParseStatus::Error) {
            result.ending = "protocol error: " + parsed.error;
            return Step::Error;
        }
        check(parsed.consumed > 0 && parsed.consumed <= rest.size(), "consumed within the input");
        check(parsed.argc <= args.size(), "argc within the arguments filled");
        pos += parsed.consumed;
        if(parsed.argc == 0) continue;      // "*0" or a blank line: nothing to run
        argc = parsed.argc;
        result.commands.emplace_back(args.begin(), args.begin() + static_cast<long>(argc));
        return Step::Command;
    }
}

// All the bytes in one read, running every command
Parsed runInOneRead(const std::string& data, bool password_set, Outcomes& outcomes) {
    InMemoryStorage storage(16);
    const std::string password = "fuzz-password";
    // A connection's state, as the server gives each client: without them,
    // MULTI and the client name would be refused, and their code not fuzzed
    Transaction transaction;
    std::string client_name;
    CommandContext ctx{storage};
    if(password_set) ctx.password = &password;
    ctx.authenticated = !password_set;
    ctx.transaction = &transaction;
    ctx.client_name = &client_name;
    ctx.client_id = 1;

    Parsed result;
    std::vector<std::string> args;
    size_t pos = 0, argc = 0;
    while(parseNext(data, pos, args, ctx.authenticated, result, argc) == Step::Command) {
        std::string out;
        executeCommand(ctx, Args(args.data(), argc), out);
        check(!out.empty(), "every command gets a reply");
        outcomes.authenticated_after.push_back(ctx.authenticated);
        if(ctx.close_connection) {
            result.ending = "closed";
            outcomes.closed = true;
            break;
        }
    }
    return result;
}

// The same bytes in two reads, split at `split`
Parsed runInTwoReads(const std::string& data, size_t split, bool password_set,
                     const Outcomes& outcomes) {
    Parsed result;
    std::vector<std::string> args;
    bool authenticated = !password_set;
    std::string in;
    size_t pos = 0, argc = 0, done = 0;
    for(int read = 0; read < 2; read++) {
        in += read == 0 ? data.substr(0, split) : data.substr(split);
        while(true) {
            Step step = parseNext(in, pos, args, authenticated, result, argc);
            if(step == Step::NeedMore) break;
            if(step == Step::Error) return result;
            check(done < outcomes.authenticated_after.size(), "no more commands than in one read");
            authenticated = outcomes.authenticated_after[done++];
            if(outcomes.closed && done == outcomes.authenticated_after.size()) {
                result.ending = "closed";
                return result;
            }
        }
    }
    return result;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if(size < 2) return 0;
    const bool password_set = bytes[0] & 1;
    const std::string data(reinterpret_cast<const char*>(bytes) + 2, size - 2);
    const size_t split = data.size() * bytes[1] / 256;

    Outcomes outcomes;
    Parsed one = runInOneRead(data, password_set, outcomes);
    Parsed two = runInTwoReads(data, split, password_set, outcomes);
    check(one == two, "two reads parse into the same commands as one");
    return 0;
}
