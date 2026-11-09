<p align="center">
  <a href="../README.md"><picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark.svg">
    <img alt="Kosh" src="assets/logo-light.svg" height="32">
  </picture></a>
</p>

<p align="center">
  <b>Running</b> &nbsp;·&nbsp;
  <a href="ARCHITECTURE.md">Architecture</a> &nbsp;·&nbsp;
  <a href="DECISIONS.md">Decisions</a> &nbsp;·&nbsp;
  <a href="COMMANDS.md">Commands</a> &nbsp;·&nbsp;
  <a href="COMPATIBILITY.md">Compatibility</a> &nbsp;·&nbsp;
  <a href="TESTING.md">Testing</a> &nbsp;·&nbsp;
  <a href="BENCHMARKS.md">Benchmarks</a> &nbsp;·&nbsp;
  <a href="HISTORY.md">History</a>
</p>

# Running it

Building Kosh, its options, passwords and Docker.

**On this page:** [Build and run](#build-and-run) &nbsp;·&nbsp; [Build and test (CMake)](#build-and-test-cmake) &nbsp;·&nbsp; [Run the server](#run-the-server) &nbsp;·&nbsp; [Run with Docker](#run-with-docker) &nbsp;·&nbsp; [Sanitizers (GCC/Clang)](#sanitizers-gccclang) &nbsp;·&nbsp; [Interactive CLI](#interactive-cli) &nbsp;·&nbsp; [Without CMake](#without-cmake) &nbsp;·&nbsp; [Project structure](#project-structure)

## Build and run

**Prerequisites:**
- A C++17 compiler with `<charconv>`, e.g. GCC 8 or newer. Tested with GCC 13 on Ubuntu 24.04.
- CMake 3.16+.
- Python 3, optional, for the integration tests.

### Build and test (CMake)

```bash
cmake -B build                            # defaults to RelWithDebInfo: -O2 with debug symbols
cmake --build build -j
cd build && ctest --output-on-failure     # unit, thread-safety and integration tests
```

### Run the server

```bash
./build/kosh-server                       # 127.0.0.1:6379, 10,000 keys, AOF in ./appendonly.aof
./build/kosh-server --port 6380 --maxkeys 1000000 --appendonly no
./build/kosh-server --help
./build/kosh-server --version             # kosh-server 4.0.0

# Connect with redis-cli (in another terminal)
redis-cli -p 6379
```

Ctrl+C (or `SIGTERM`) stops the server cleanly: it closes the connections, writes what is left of the AOF (and syncs it, unless `--appendfsync no`), and exits with code 0. With `--appendfsync always`, an AOF that can't be written or synced stops it with exit code 1.

| Option | Default | Meaning |
|--------|---------|---------|
| `--port <1-65535>` | 6379 | TCP port to listen on |
| `--bind <ipv4>` | 127.0.0.1 | Address to listen on; `0.0.0.0` listens on every interface |
| `--protected-mode <yes\|no>` | yes | With no password set, refuse clients on other machines |
| `--requirepass-file <path>` | none | Clients must `AUTH` with the password in this file (one line, at most 512 bytes) before anything else |
| `--requirepass <password>` | none | The same, given directly; other users on the machine can read it in the process list, so prefer the file |
| `--maxkeys <n>` | 10000 | Keys to hold before evicting one |
| `--eviction <sieve\|lru>` | sieve | Which key a full server evicts: SIEVE's choice, or the least recently used |
| `--appendonly <yes\|no>` | yes | Log writes to the AOF and replay it at startup |
| `--appendfilename <path>` | appendonly.aof | AOF file; the server refuses to start if it can't be opened |
| `--appendfsync <always\|everysec\|no>` | everysec | When the AOF reaches the disk: before every reply, once a second, or when the OS decides |
| `--threads <1-256>` | 1 | Event loops, one per thread, each owning a share of the keys and of `--maxkeys`. Above 1 needs Linux and `epoll`. See [Architecture](ARCHITECTURE.md#several-threads) |
| `--maxclients <n>` | 10000 | Clients served at once; more get an error and are disconnected |
| `--poller <epoll\|select>` | epoll on Linux, else select | How the event loop waits for sockets (`select()` serves about 1,000 clients) |

**Who can connect.** By default Kosh listens on 127.0.0.1, so only programs on the same machine can reach it. `--bind 0.0.0.0` listens on every interface. Even then, while no password is set, protected mode (as in Redis) answers clients on other machines with `-DENIED` and disconnects them; `--protected-mode no` turns it off, for a network you trust. Redis started without a config file listens on every interface and relies on protected mode alone; Kosh makes the network an explicit choice.

[SECURITY.md](../SECURITY.md) lists what Kosh defends against, what it doesn't (there is no TLS, for one), and how to report a vulnerability.

**Passwords.** With `--requirepass-file` (or `--requirepass`), a client gets `-NOAUTH` for everything but `AUTH` and `QUIT` until it sends `AUTH <password>` (or `AUTH default <password>`). The replies are Redis's, byte for byte, which matters to client libraries that match on them. Two details guard against a client that doesn't have the password:
- Until it authenticates, a client's commands may have at most 10 arguments of 16 KB each, Redis's limits since 6.2.6. Without them, a client without the password could still make the server buffer up to 1 GB for it on each connection.
- The password check takes the same time however much of a guess is right, so response times can't reveal the password byte by byte. [Measured](../bench/results/password-check/README.md): at most 1.03× between guesses, where a loop that stops at the first wrong byte takes 79× longer for a right 512-byte guess than for one wrong at the first byte.

### Run with Docker

```bash
docker build -t kosh .                    # 37.5 MB: the server, glibc and libstdc++; no shell
docker run -d --name kosh -v kosh-data:/data -p 127.0.0.1:6379:6379 kosh --protected-mode no
docker stop kosh                          # SIGTERM: the AOF is written and synced, exit code 0
```

The image runs as an unprivileged user (uid 65532) and keeps the AOF in the `/data` volume, so the data outlives the container. Options after the image name are passed to the server, as in the table above.

**Why `--protected-mode no`.** Inside a container the server must listen on every interface (the image passes `--bind 0.0.0.0`), and your connections arrive through Docker's network, not from "this machine". So with no password, protected mode refuses them, as it should. Redis's official image turns protected mode off for you; Kosh's doesn't, because a `-p 6379:6379` without an address publishes the port on every interface of the host, which is how many Redis servers ended up open to the internet. Either publish on `127.0.0.1` as above and turn protected mode off yourself, or set a password:

```bash
docker run -d --name kosh -v kosh-data:/data -p 6379:6379 \
  -v "$PWD/password:/run/secrets/kosh-password:ro" kosh --requirepass-file /run/secrets/kosh-password
```

The password file must be readable by uid 65532.

### Sanitizers (GCC/Clang)

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer, all tests
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address,undefined
cmake --build build-asan -j
(cd build-asan && ctest --output-on-failure)

# ThreadSanitizer, concurrency tests
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread
cmake --build build-tsan -j --target test_threads
setarch "$(uname -m)" -R ./build-tsan/test_threads
```

> [!NOTE]
> GCC 13's ThreadSanitizer stops with `unexpected memory mapping` on Linux kernels with high address-space randomisation (e.g. Ubuntu 24.04, WSL2). `setarch -R` disables randomisation for that one process.

### Interactive CLI

```bash
./build/kosh-cli
```

### Without CMake

```bash
# Linux / macOS
g++ -std=c++17 -O2 -pthread src/server_main.cpp src/Server.cpp src/EventLoop.cpp src/Poller.cpp src/Commands.cpp src/InMemoryStorage.cpp src/AOFLogger.cpp src/RedisLite.cpp -o kosh-server
g++ -std=c++17 -O2 -pthread src/main.cpp src/Commands.cpp src/InMemoryStorage.cpp src/AOFLogger.cpp src/RedisLite.cpp -o kosh-cli
g++ -std=c++17 -O2 -pthread tests/test_main.cpp src/Poller.cpp src/Commands.cpp src/InMemoryStorage.cpp src/AOFLogger.cpp src/RedisLite.cpp -o run_tests
g++ -std=c++17 -O2 -pthread tests/test_thread_safety.cpp src/InMemoryStorage.cpp src/AOFLogger.cpp src/RedisLite.cpp -o run_thread_tests
```

On Windows (MinGW), add `-lws2_32` to the server and unit-test commands for Winsock.

## Project structure

```
kosh/
├── src/                          # The server and the CLI
│   ├── server_main.cpp           # Server entry point (TCP mode, command-line flags)
│   ├── main.cpp                  # CLI entry point (interactive mode)
│   ├── Server.h / .cpp           # TCP server: listening socket, accepting clients, start and stop
│   ├── EventLoop.h / .cpp        # One thread's event loop, per-client buffers, RESP I/O, forwarding
│   ├── SpscRing.h                # The queue between two threads
│   ├── Wakeup.h                  # Waking a thread that may be asleep
│   ├── Router.h                  # Which thread owns a key
│   ├── AofManifest.h             # What the AOF files of several threads mean
│   ├── Poller.h / .cpp           # Readiness interface: epoll (Linux) and select()
│   ├── Socket.h                  # Winsock / BSD socket differences (header-only)
│   ├── RespParser.h              # RESP protocol parser + serializer (header-only)
│   ├── Commands.h / .cpp         # Command table: dispatch for the server and AOF replay
│   ├── RedisLite.h / .cpp        # Controller: input validation, command routing (CLI)
│   ├── StorageInterface.h        # Abstract interface (pure virtual base class)
│   ├── InMemoryStorage.h / .cpp  # Storage engine: entries, eviction (SIEVE/LRU), TTL, variant types, AOF
│   ├── KeyTable.h                # Hash table: cache-line buckets, incremental rehashing (header-only)
│   ├── SipHash.h                 # SipHash, the key table's keyed hash (header-only)
│   ├── AOFLogger.h / .cpp        # Append-only file: appends, crash-safe rewrite, failed writes
│   ├── Result.h                  # Result type with error codes
│   ├── Integer.h                 # Integers as Redis reads them (arguments, and INCR's values)
│   ├── Glob.h                    # Redis's glob patterns, for SCAN's MATCH (header-only)
│   └── Version.h                 # Kosh's version (--version, HELLO, INFO)
├── SECURITY.md               # What Kosh defends against, what it doesn't, how to report a flaw
├── CHANGELOG.md              # What changed in each release
├── Dockerfile                # Multi-stage build; runs as a non-root user (.dockerignore: a whitelist)
├── .github/workflows/ci.yml  # CI: compilers, sanitizers, Windows, static analysis, fuzzing, Docker
├── .clang-tidy               # clang-tidy's checks for CI, and why two are off
├── cmake/mingw-w64.cmake     # Toolchain file: cross-compile for Windows with MinGW-w64
├── CMakeLists.txt            # CMake build (C++17, warnings, optional sanitizers)
├── .gitignore
├── bench/                    # Measurement tools: throughput, commit comparison, replay, profile,
│                             #   memory (and against Redis), allocations per request, insert and
│                             #   lookup latency, eviction miss ratios, layout bias, idle clients,
│                             #   password check timing, hash flooding
│   └── results/              # Measured results, one directory per version
├── stress-test-results/      # Earlier Windows benchmark screenshots (superseded)
└── tests/
    ├── test_main.cpp              # 142 unit tests
    ├── test_thread_safety.cpp     # Concurrency stress tests (5 tests x 8 threads)
    ├── test_loadgen.cpp           # kosh-loadgen's histogram, reply scanner and refusal rules
    ├── integration/
    │   ├── test_server.py         # Integration tests against the real server binary
    │   ├── test_clients.py        # redis-py and node-redis against it
    │   └── node_redis_check.cjs   # The node-redis half, run by test_clients.py
    ├── fuzz/                      # Fuzz target for the parser and commands, its driver, seed corpus
    ├── compat/
    │   └── compare_with_redis.py  # The same commands to Redis and Kosh, replies compared
    └── windows/
        └── port_test.ps1          # How the server takes its port, run on Windows
```

<br>

<p align="center">
  <sub><a href="../README.md">← Kosh</a> &nbsp;&nbsp;|&nbsp;&nbsp; <a href="ARCHITECTURE.md">Architecture →</a></sub>
</p>
