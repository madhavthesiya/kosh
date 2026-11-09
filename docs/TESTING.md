<p align="center">
  <a href="../README.md"><picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark.svg">
    <img alt="Kosh" src="assets/logo-light.svg" height="32">
  </picture></a>
</p>

<p align="center">
  <a href="RUNNING.md">Running</a> &nbsp;·&nbsp;
  <a href="ARCHITECTURE.md">Architecture</a> &nbsp;·&nbsp;
  <a href="DECISIONS.md">Decisions</a> &nbsp;·&nbsp;
  <a href="COMMANDS.md">Commands</a> &nbsp;·&nbsp;
  <a href="COMPATIBILITY.md">Compatibility</a> &nbsp;·&nbsp;
  <b>Testing</b> &nbsp;·&nbsp;
  <a href="BENCHMARKS.md">Benchmarks</a> &nbsp;·&nbsp;
  <a href="HISTORY.md">History</a>
</p>

# Testing

The test suites, continuous integration and sanitizers.

**On this page:** [Tests](#tests) &nbsp;·&nbsp; [Several threads](#several-threads) &nbsp;·&nbsp; [Continuous integration](#continuous-integration)

## Tests

**142 unit tests** (`tests/test_main.cpp`):

| Area | Tests | What they cover |
|---|---:|---|
| Basic Operations | 7 |  |
| LRU Eviction | 7 | includes: the table starts empty and grows with the keys |
| Key Table | 10 | against std::unordered_map at random, SCAN's cursor while the table grows, every key in one chain, rehash bounded, a mapped table given back mid-rehash; SipHash against the reference vectors and CPython, each table and storage with its own key |
| SIEVE Eviction | 5 | a case where SIEVE and LRU differ, the hand on a deleted key, 50,000 random operations per policy against a model |
| TTL Expiration | 5 |  |
| TTL > LRU Priority | 2 |  |
| Persistence | 4 | the CLI's SAVE/LOAD |
| Controller Validation | 1 |  |
| List Operations | 6 |  |
| Set Operations | 5 |  |
| Hash Operations | 5 |  |
| Type Safety | 4 |  |
| LRU with Mixed Types | 1 |  |
| AOF Persistence | 26 | absolute expiry, logged removals, randomized replay check under both eviction policies, damaged files, failed writes, group commit, fsync policies |
| Command Table | 13 | arity, SET options, integers read as Redis reads them, error replies, PING's message, replies appended in place |
| Authentication | 4 | constant-time comparison, NOAUTH and AUTH as Redis replies, QUIT before AUTH |
| Transactions | 4 | Redis's replies, every way to abort, the AOF's MULTI/EXEC records, an incomplete transaction cut off at replay |
| Counters and Several Keys | 7 | the edges of 64 bits, one AOF record per command, MSET's evictions after its record under SIEVE and LRU |
| PTTL, PEXPIRE, PERSIST, DBSIZE | 2 | Redis's replies; a time already past deletes at once, uncounted; DBSIZE removes keys that ran out; PERSIST and DEL in the AOF, replayed |
| SCAN | 2 | glob patterns as Redis matches them, an exponential pattern ending fast, the 1,000-level depth limit; a full iteration, COUNT, MATCH, TYPE counted as reads, expired keys removed |
| FLUSHALL, FLUSHDB | 2 | Redis's replies; eviction works after, under SIEVE and LRU; logged as given and replayed |
| INFO | 2 | hits and misses on every read path, expiries and evictions, sections, which commands count |
| RESP3 and HELLO | 4 | each reply type in RESP2 and RESP3, every HELLO error, HELLO AUTH, CLIENT names and ids |
| Pollers | 2 | select() and epoll pass the same checks |
| RESP Parser | 12 | malformed input, size limits, limits before AUTH, every split point, reused argument strings |

The randomized replay test runs 4 seeds per eviction policy by default; `RANDOM_TEST_SEEDS=300 ./build/tests` runs more. It compares wall-clock expiry times, so a seed whose results differ while the wall clock was stepped (WSL2 steps it every 30 s, by up to about a third of a second) is run again; a difference on a steady clock fails at once.

**Thread-safety stress tests** (`tests/test_thread_safety.cpp`):
- 5 tests, each running 8 threads at once against one storage instance (20,000 operations in total).
- They check for crashes and invalid results. Build with `-DSANITIZE=thread` to have ThreadSanitizer check for data races.

**Integration tests** (`tests/integration/test_server.py`, Python standard library only):
- 58 tests that start the real server binary on a free port and speak RESP over TCP. Each test starts once the server has dropped the connection that checked it was listening, so tests that count clients see only their own.
- They cover:
  - basic commands, command-line flags (including which policy `--eviction` picks, and the default), and AOF replay after a restart, `MSET`, several-key `DEL` and the counters included;
  - the log reaching its file while the server runs, as under systemd or Docker;
  - who can connect: loopback only by default, `--bind`, and protected mode, tested by connecting from this machine's own network address as a client on another machine would;
  - passwords: `NOAUTH` until `AUTH`, `AUTH` in a pipeline, the limits before `AUTH`, `--requirepass-file`, and the password never reaching the log, the AOF or an error message;
  - `HELLO` and RESP3: the protocol switches for its own connection only, `HELLO 3 AUTH` as client libraries send it, client ids and names;
  - `INFO`'s server counters: connections received and refused, the ops-per-second rate under load;
  - transactions: a pipelined `MULTI`/`EXEC` surviving a restart, a queue no other connection sees, a disconnect dropping it, an incomplete one in the AOF cut off at startup, and `EXEC` refused whole while the AOF can't be written;
  - many clients pipelining at once, and `BGREWRITEAOF` in the middle of a pipeline (group commit);
  - a damaged AOF at startup, failed AOF writes under a file size limit (Challenge 12), and `--appendfsync always` stopping the server when they fail;
  - protocol errors and error replies;
  - a client that never reads its replies (Challenge 8);
  - 1,100 connections: refused beyond `select()`'s limit, served with `epoll`; `--maxclients`; running out of file descriptors (Challenge 10);
  - shutdown on `SIGINT`/`SIGTERM`;
  - the client-disconnect regression (Challenge 7).
- `ctest` runs them twice: with the default poller, and with `--poller select`, so the path `epoll` replaced as the default stays tested.

**The load generator** (`tests/test_loadgen.cpp` and `tests/integration/test_loadgen.py`, Linux): the histogram, the RESP reply scanner across every read boundary, and the rules for refusing a percentile; then a real server frozen for a known time, with the reported percentiles checked against the arithmetic.

**Client libraries** (`tests/integration/test_clients.py`):
- redis-py 8.1 and node-redis 6.3, the current versions, each through its own API, in RESP2 and RESP3, with and without a password.
- They cover every supported command, pipelines, `MULTI`/`EXEC` transactions, authentication failures, and errors raised as errors.
- The libraries aren't part of the repository: `pip install redis`, and `npm install redis` with `NODE_PATH` pointing at its `node_modules`. A missing one skips its tests and says so; `KOSH_REQUIRE_CLIENTS=1` makes that a failure, for CI.

**Fuzzing** (`tests/fuzz/`):
- A fuzz target in libFuzzer's interface sends arbitrary bytes through the parser and the command table, as the server does, with and without a password set.
- Besides crashes and sanitizer reports, it checks that the same bytes in two reads parse into exactly the same commands as in one read. Where a read happens to end must not change a command.
- GCC has no libFuzzer, so a small driver replays the 27 seed inputs (a `ctest`) or mutates them at random. With clang, `-DKOSH_LIBFUZZER=ON` builds the coverage-guided version.
- The fuzzer was checked first, with three planted bugs: a header split across reads, an inline limit set too low, and `AUTH` reading past its arguments. It found each within 1.2 s.
- Then 10 minutes of random mutation of the real code under ASan + UBSan: 22.4 million inputs, no failures. Random mutation explores less than coverage-guided fuzzing, which CI runs on every push (below).

All 17 `ctest` suites pass with AddressSanitizer + UndefinedBehaviorSanitizer, the client libraries' and the comparison with Redis included. The thread-safety and unit tests also pass under ThreadSanitizer, and so does the integration suite against a ThreadSanitizer build of the server (where the `everysec` sync thread runs alongside the event loop). The Windows build is compile-checked with MinGW-w64 (GCC 13). Only `tests/windows/port_test.ps1` runs it, on Windows 11: a second server on a port in use is refused, a restart right after closing connections works, and a failed bind reports its real error.

### Several threads

- **`tests/test_spsc_ring.cpp`, `tests/test_wakeup.cpp`, `tests/test_router.cpp`:** the queue, the wakeup protocol and routing. The wakeup test proves no wakeup is lost without relying on timing.
- **`tests/integration/test_threads.py`:** replies checked against a model, in random pipelines across threads, by many clients at once; split commands, transactions, `SCAN`; clients leaving with replies in flight; a reused socket number; a thread held busy; garbage and random commands.
- **`tests/integration/test_shard_aof.py`:** every data type across a restart; killing the server under load; writes made to fail at a chosen point; every wrong layout refused.
- **With `KOSH_THREADS=4`** the integration suite, the client libraries and the comparison with Redis run again at 4 threads. 10 of the 58 integration tests say why they need one thread.
- **Planted bugs:** over 70 for the threading work, each built and run against these suites. The memory-ordering ones pass a normal run on x86 and are caught only by TSan or by the wakeup test.

### Continuous integration

Every push runs [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) on GitHub Actions:

| Job | What it runs |
|-----|--------------|
| Linux, GCC and Clang | Build with warnings as errors; every `ctest` suite, under `epoll` and `select()`, with redis-py, node-redis and Redis 7.0.15 installed (a missing one fails instead of skipping) |
| Sanitizers | Every suite under ASan + UBSan; unit, thread-safety and integration tests under TSan |
| Windows | Cross-compiled with MinGW-w64 (warnings as errors, linked statically), then `port_test.ps1` on a Windows runner with that `.exe` |
| Static analysis | clang-tidy (bugprone and performance checks; [`.clang-tidy`](../.clang-tidy) says why two are off) and cppcheck; any finding fails |
| Fuzz | 2 minutes of coverage-guided libFuzzer under ASan + UBSan, with a [RESP dictionary](../tests/fuzz/resp.dict) ([measured](../bench/results/fuzz-dictionary/README.md): 0.1–1.8% more code edges at 50,000 inputs, every seed; small because, most likely, the seed inputs already contain all 43 command names); then the coverage of the corpus, uploaded as an HTML report |
| Docker | Builds the image; checks `--version`, protected mode refusing by default, `PING` with `--protected-mode no`, the non-root user, and a clean `docker stop` |

Each job was first run in a local copy of GitHub's Ubuntu 24.04 runner. The checks new with CI (static analysis, the Docker smoke test) were each shown to fail when a fix is undone, as the test suites were when they were written. That first run found two bugs: a test race (a test could start before the server had dropped the connection that checked it was up; 22 of 200 runs failed) and a CI step that failed at random (`ctest -N | grep -q` under `pipefail`: `grep` exits at the first match and `ctest` gets `SIGPIPE`).

Not in CI: clang's static analyzer, which ran past 10 minutes on `InMemoryStorage.cpp` and on `Commands.cpp` without finishing (it found nothing in the other sources).

<br>

<p align="center">
  <sub><a href="COMPATIBILITY.md">← Compatibility</a> &nbsp;&nbsp;|&nbsp;&nbsp; <a href="BENCHMARKS.md">Benchmarks →</a></sub>
</p>
