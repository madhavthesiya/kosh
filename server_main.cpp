#include <iostream>
#include <string>
#include <optional>
#include <charconv>
#include <cstdint>
#include "InMemoryStorage.h"
#include "AOFLogger.h"
#include "Commands.h"
#include "Server.h"

// Settings that would otherwise mean editing and rebuilding, e.g. a larger
// key limit for benchmarks or a separate port and AOF file for tests.
// Defaults match the server's previous hard-coded behaviour.
struct ServerConfig {
    int port = 6379;
    size_t maxKeys = 10000;
    bool appendOnly = true;
    std::string appendFilename = "appendonly.aof";
};

static void printUsage(std::ostream& out) {
    out << "Usage: mini_redis_server [options]\n"
           "  --port <1-65535>         TCP port to listen on (default 6379)\n"
           "  --maxkeys <n>            Keys to hold before LRU eviction (default 10000)\n"
           "  --appendonly <yes|no>    Log writes to the AOF and replay it at startup (default yes)\n"
           "  --appendfilename <path>  AOF file (default appendonly.aof)\n"
           "  --help                   Show this message\n";
}

// Parses a whole string as an integer in [min, max].
// std::from_chars, unlike std::stoi, rejects trailing garbage such as "80abc"
// and never throws, so bad input can't slip through as a partial number.
template<typename T>
static bool parseNumber(const std::string& text, T min, T max, T& out) {
    T value{};
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if(ec != std::errc() || end != text.data() + text.size()) return false;
    if(value < min || value > max) return false;
    out = value;
    return true;
}

enum class ParseResult { Run, Help, Error };

static ParseResult parseArgs(int argc, char* argv[], ServerConfig& config) {
    for(int i = 1; i < argc; i++) {
        std::string option = argv[i];
        if(option == "--help" || option == "-h") return ParseResult::Help;

        if(option != "--port" && option != "--maxkeys" &&
           option != "--appendonly" && option != "--appendfilename") {
            std::cerr << "Unknown option '" << option << "' (see --help)\n";
            return ParseResult::Error;
        }
        if(i + 1 >= argc) {
            std::cerr << "Missing value for " << option << "\n";
            return ParseResult::Error;
        }
        std::string value = argv[++i];

        bool ok = true;
        if(option == "--port") {
            ok = parseNumber(value, 1, 65535, config.port);
        } else if(option == "--maxkeys") {
            // 0 is rejected: eviction assumes at least one key fits
            ok = parseNumber<size_t>(value, 1, SIZE_MAX, config.maxKeys);
        } else if(option == "--appendonly") {
            ok = (value == "yes" || value == "no");
            if(ok) config.appendOnly = (value == "yes");
        } else {
            ok = !value.empty();
            if(ok) config.appendFilename = value;
        }
        if(!ok) {
            std::cerr << "Invalid value '" << value << "' for " << option << " (see --help)\n";
            return ParseResult::Error;
        }
    }
    return ParseResult::Run;
}

int main(int argc, char* argv[]) {
    ServerConfig config;
    switch(parseArgs(argc, argv, config)) {
        case ParseResult::Help:  printUsage(std::cout); return 0;
        case ParseResult::Error: return 2;
        case ParseResult::Run:   break;
    }

    // AOF persistence is optional. Every write is flushed to the OS, so it
    // survives a process crash; without fsync a power cut can lose recent writes.
    std::optional<AOFLogger> aof;
    if(config.appendOnly) {
        aof.emplace(config.appendFilename);
        if(!aof->isOpen()) {
            std::cerr << "Cannot open AOF file '" << config.appendFilename << "'\n";
            return 1;
        }
    }
    AOFLogger* aofLogger = aof ? &*aof : nullptr;

    InMemoryStorage storage(config.maxKeys, aofLogger);

    // Restore state from previous session
    if(aofLogger) replayAOF(storage, *aofLogger);

    std::cout << "Max keys: " << config.maxKeys << " | AOF: "
              << (aofLogger ? config.appendFilename : "disabled") << "\n";

    Server server(config.port, storage, aofLogger);
    if(!server.start()) {
        std::cerr << "Failed to start server on port " << config.port << "\n";
        return 1;
    }

    // Block forever, handling client connections
    server.run();

    return 0;
}
