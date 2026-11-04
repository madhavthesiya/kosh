#include <iostream>
#include <string>
#include <optional>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include "InMemoryStorage.h"
#include "AOFLogger.h"
#include "Commands.h"
#include "Server.h"

// Settings that would otherwise mean editing and rebuilding, e.g. a larger
// key limit for benchmarks or a separate port and AOF file for tests.
struct ServerConfig {
    int port = 6379;
    // Loopback: a server started with no options can't be reached from the
    // network. (Redis listens on every interface unless its config file says
    // otherwise, and relies on protected mode alone.)
    std::string bindAddress = "127.0.0.1";
    bool protectedMode = true;
    size_t maxKeys = 10000;
    // SIEVE: fewer misses than LRU on every trace measured, and a hit
    // writes nothing outside the entry (bench/results/after-memory-fixes)
    EvictionPolicy eviction = EvictionPolicy::SIEVE;
    bool appendOnly = true;
    std::string appendFilename = "appendonly.aof";
    // Redis's default: a power cut loses at most about a second of writes
    AOFLogger::FsyncPolicy appendFsync = AOFLogger::FsyncPolicy::EverySecond;
    std::string poller = defaultPollerName();
    size_t maxClients = Server::kDefaultMaxClients;
};

static void printUsage(std::ostream& out) {
    out << "Usage: kosh-server [options]\n"
           "  --port <1-65535>         TCP port to listen on (default 6379)\n"
           "  --bind <ipv4>            Address to listen on (default 127.0.0.1: this machine\n"
           "                           only); 0.0.0.0 listens on every interface\n"
           "  --protected-mode <yes|no>\n"
           "                           With no password set, refuse clients on other\n"
           "                           machines (default yes)\n"
           "  --maxkeys <n>            Keys to hold before evicting one (default 10000)\n"
           "  --eviction <sieve|lru>   Which key a full server evicts: SIEVE's choice\n"
           "                           (NSDI 2024), or the least recently used (default sieve)\n"
           "  --appendonly <yes|no>    Log writes to the AOF and replay it at startup (default yes)\n"
           "  --appendfilename <path>  AOF file (default appendonly.aof)\n"
           "  --appendfsync <always|everysec|no>\n"
           "                           When the AOF is synced to disk: before every reply,\n"
           "                           once a second, or when the OS decides (default everysec)\n"
           "  --maxclients <n>         Clients served at once; more are refused (default 10000)\n"
           "  --poller <epoll|select>  How to wait for sockets (default: epoll on Linux,\n"
           "                           select elsewhere; select serves about 1,000 clients)\n"
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

        if(option != "--port" && option != "--bind" && option != "--protected-mode" &&
           option != "--maxkeys" && option != "--eviction" &&
           option != "--appendonly" && option != "--appendfilename" && option != "--appendfsync" &&
           option != "--maxclients" && option != "--poller") {
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
        } else if(option == "--bind") {
            // Numeric only, so a host name can't resolve to an address nobody meant
            in_addr address;
            ok = parseIPv4(value.c_str(), address);
            if(ok) config.bindAddress = value;
        } else if(option == "--protected-mode") {
            ok = (value == "yes" || value == "no");
            if(ok) config.protectedMode = (value == "yes");
        } else if(option == "--maxkeys") {
            // 0 is rejected: eviction assumes at least one key fits
            ok = parseNumber<size_t>(value, 1, SIZE_MAX, config.maxKeys);
        } else if(option == "--eviction") {
            if(value == "lru") config.eviction = EvictionPolicy::LRU;
            else if(value == "sieve") config.eviction = EvictionPolicy::SIEVE;
            else ok = false;
        } else if(option == "--appendonly") {
            ok = (value == "yes" || value == "no");
            if(ok) config.appendOnly = (value == "yes");
        } else if(option == "--appendfsync") {
            if(value == "always") config.appendFsync = AOFLogger::FsyncPolicy::Always;
            else if(value == "everysec") config.appendFsync = AOFLogger::FsyncPolicy::EverySecond;
            else if(value == "no") config.appendFsync = AOFLogger::FsyncPolicy::No;
            else ok = false;
        } else if(option == "--maxclients") {
            ok = parseNumber<size_t>(value, 1, 1000000, config.maxClients);
        } else if(option == "--poller") {
            // Rejected here if this system has no such poller (no epoll outside Linux)
            ok = makePoller(value) != nullptr;
            if(ok) config.poller = value;
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
    // The log goes out a line at a time, as Redis's does, even when stdout is a
    // file or a pipe (systemd, docker logs), which the C library would
    // otherwise buffer a few KB at a time: a line would show up only once
    // dozens more had followed it, or never if the server were killed. A
    // write per log line costs little: lines are per connection, never per
    // command. (std::cout writes through stdout while synced with stdio, the
    // default.) Windows's C library treats line buffering as full buffering,
    // so there it is unbuffered.
#ifdef _WIN32
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
#endif

    ServerConfig config;
    switch(parseArgs(argc, argv, config)) {
        case ParseResult::Help:  printUsage(std::cout); return 0;
        case ParseResult::Error: return 2;
        case ParseResult::Run:   break;
    }

    // AOF persistence is optional. Every write reaches the OS before its reply,
    // so it survives a process crash; --appendfsync decides when it reaches
    // the disk, which is what survives a power cut.
    std::optional<AOFLogger> aof;
    if(config.appendOnly) {
        aof.emplace(config.appendFilename);
        if(!aof->isOpen()) {
            std::cerr << "Cannot open AOF file '" << config.appendFilename << "'\n";
            return 1;
        }
    }
    AOFLogger* aofLogger = aof ? &*aof : nullptr;

    InMemoryStorage storage(config.maxKeys, aofLogger, config.eviction);

    // Restore state from previous session
    if(aofLogger) {
        AofLoadResult loaded = replayAOF(storage, *aofLogger);
        if(!loaded.ok) {
            std::cerr << "Cannot start: " << loaded.error << "\n";
            return 1;
        }
        if(loaded.truncated_bytes > 0) {
            std::cerr << "Warning: the AOF ended with an incomplete record (the server stopped "
                         "while writing it); cut off its " << loaded.truncated_bytes << " bytes\n";
        }
        std::cout << "Loaded " << loaded.commands
                  << (loaded.commands == 1 ? " command" : " commands") << " from the AOF\n";
        // After the replay, which writes nothing worth syncing
        aofLogger->setFsyncPolicy(config.appendFsync);
    }

    const char* fsync_name = config.appendFsync == AOFLogger::FsyncPolicy::Always ? "always"
                           : config.appendFsync == AOFLogger::FsyncPolicy::EverySecond ? "everysec" : "no";
    std::cout << "Max keys: " << config.maxKeys
              << " (eviction: " << (config.eviction == EvictionPolicy::SIEVE ? "sieve" : "lru") << ")"
              << " | AOF: "
              << (aofLogger ? config.appendFilename + " (fsync: " + fsync_name + ")" : "disabled") << "\n";

    Server server(config.port, storage, aofLogger, makePoller(config.poller), config.maxClients);
    server.setBindAddress(config.bindAddress);
    server.setProtectedMode(config.protectedMode);
    if(!server.start()) return 1;   // start() said why

    // Serve until a stop signal (exit code 0), or until the AOF fails for good
    // with --appendfsync always (exit code 1)
    server.run();
    return server.exitCode();
}
