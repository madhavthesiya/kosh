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
  <a href="TESTING.md">Testing</a> &nbsp;·&nbsp;
  <a href="BENCHMARKS.md">Benchmarks</a> &nbsp;·&nbsp;
  <b>History</b>
</p>

# From Mini-Redis to Kosh

How the project grew, stage by stage. Every number links to or comes from the results kept in [`bench/results`](../bench/results). Releases are in the [changelog](../CHANGELOG.md), and the reasoning behind each change is in [Decisions](DECISIONS.md).

**On this page:** [The original Mini-Redis](#the-original-mini-redis) &nbsp;·&nbsp; [The hardening](#the-hardening) &nbsp;·&nbsp; [What was left out on purpose](#what-was-left-out-on-purpose)

## The original Mini-Redis

**January 2026: a command-line cache.** About 500 lines of C++: `SET`, `GET`, `DEL` and `EXISTS` in O(1), LRU eviction, TTLs with lazy expiry (an expired key is removed before an unexpired one is evicted), and `SAVE`/`LOAD`. The layers were already separate: the CLI, a controller (`RedisLite`), a `StorageInterface`, and `InMemoryStorage`. That CLI still exists as `kosh-cli`.

**April 2026: the engine overhaul.** Critical bugs fixed, TTLs in the CLI, a reader-writer lock for thread safety, unit tests and a stress test, and a CMake build.

**May 2026: a real server.** A TCP server speaking RESP with an event loop, an append-only file, lists, sets and hashes, then `TCP_NODELAY` and the first benchmarks with pipelining. In June came stress tests, kept in [`stress-test-results`](../stress-test-results).

## The hardening

From October 2026 the project went through a plan with one rule: nothing is claimed unless it is measured, and every bug starts with a test that reproduces it.

**Make the tests honest.** The thread-safety test could not actually fail; now it can. Builds became optimised by default with warnings on, sanitizers became a build option, the server gained command-line flags and an integration test harness, `SIGPIPE` stopped being able to kill the server, and the README was corrected to match the code.

**Measure first (`v0-baseline`).** Before changing anything: a repeatable benchmark against real Redis, AOF replay timing, CPU profiles and flame graphs, memory per key, and many-client tests. [Results](../bench/results/v0-baseline/README.md).

**Network correctness.** A strict RESP parser with limits and proper error replies, non-blocking sockets with per-client reply buffers so a client that doesn't read can't stall the others, a poller interface, a fix for the 1,021st client overflowing `select()`, a spare file descriptor for running out of them, and clean shutdown on signals. Pipelined throughput rose 1.45–1.71×. [Results](../bench/results/after-network-fixes/README.md).

**Persistence correctness (`v1-correct`).** One command table shared by the server and replay, absolute expiry times, `SET` options, logged deletions, an expiry index, crash-safe `BGREWRITEAOF` through a temporary file and an atomic rename, damage detection at startup, and writes refused while the disk is full. Replaying 1,000,000 commands went from over 15 minutes, when the run was stopped, to 0.72 s. [Results](../bench/results/after-correctness-fixes/README.md).

**Speed (`v2-fast`).** `epoll`, group commit (100,002 `write()` calls down to 132 for 100,000 `SET`s), zero heap allocations per request once warmed up, `--appendfsync always|everysec|no`, `--maxclients` and `--poller`, and paired benchmarks that alternate the servers. Pipelined `SET` with the AOF on reached 1.21× Redis in both sessions, at 0.73× and 0.82× its CPU per request. [Results](../bench/results/after-speed-fixes/README.md).

**Memory (`v2.5-memory`).** SIEVE eviction, one allocation per key with the eviction list inside the entry, and `KeyTable`, a hash table with 64-byte buckets, one-byte tags and incremental rehashing, ending the freeze when the table grew. Memory per key went from 312 bytes to 113 at 800,000 keys (124 at 1,000,000, where the table is in the middle of growing). Pipelined, Kosh was faster than Redis 7.0.15 in all 12 paired runs: 1.16–1.38× by test, 1.04–1.46× run by run. [Results](../bench/results/after-memory-fixes/README.md).

**Usable by other people (3.0.0).** The rename to Kosh, the MIT license, listening on 127.0.0.1 by default with protected mode, `AUTH` with a constant-time comparison, a keyed SipHash against hash flooding, a fuzz target, a security policy, RESP3 and `HELLO`, `MULTI`/`EXEC`, `INFO`, the `INCR` family, multi-key `DEL` and `EXISTS`, `MGET` and `MSET`, `--version`, a Docker image, GitHub Actions CI, tests with redis-py and node-redis, a byte-for-byte comparison with Redis, and release binaries for Linux and Windows. [Hash flooding](../bench/results/hash-flooding/README.md), [password timing](../bench/results/password-check/README.md).

**More of Redis (3.1.0).** `PTTL`, `PEXPIRE`, `PERSIST`, `DBSIZE`, `SCAN` with Redis's glob matching, `FLUSHALL` and `FLUSHDB`, and an independent compatibility suite in CI.

**An honest latency tool.** `kosh-loadgen`, which measures from each request's intended send time, and refuses to print a percentile it can't support. When Redis was frozen for a second, `memtier` reported a p99 of 1.43 ms; `kosh-loadgen` reported 713 ms, where the arithmetic says 700. [Results](../bench/results/open-loop/README.md).

**Several threads (4.0.0).** Shard-per-core with `--threads N`, in small steps: a lock-free ring, one event loop per thread, a wakeup protocol that can't lose a wakeup, forwarding with replies kept in order, one AOF per thread with a manifest, commands that span threads, and every test suite run at 4 threads. A first benchmark showed threads making things slower, which led to two findings: 48 connections hid the effect of threads, and sending a pipeline's replies in pieces wasted most of the gain. [Results](../bench/results/shard-per-core/README.md), [how it works](ARCHITECTURE.md#several-threads).

## What was left out on purpose

- A background `fork()` for `BGREWRITEAOF`, and the multi-part AOF that goes with it.
- `io_uring`. Replication, Lua, Pub/Sub, sorted sets and streams.
- Cross-thread atomic transactions, and changing the thread count between restarts.

The [known limitations](COMPATIBILITY.md#known-limitations) list what is missing and what a fix would take.

<br>

<p align="center">
  <sub><a href="BENCHMARKS.md">← Benchmarks</a> &nbsp;&nbsp;|&nbsp;&nbsp; <a href="../README.md">Kosh →</a></sub>
</p>
