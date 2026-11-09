# Measurement tools

Scripts that produce every performance number quoted for Kosh. Each one builds the server itself, so a result always comes from a known commit and build type, and each writes its results together with the machine, compiler, flags and commit it ran on.

| Script | Measures | Output |
|---|---|---|
| [`bench.py`](bench.py) | Requests/s and latency for SET and GET, pipelined and not, AOF on and off, against Kosh and real Redis | `throughput.md`, `throughput-runs.csv` |
| [`compare_commits.py`](compare_commits.py) | Kosh at several commits in one session, compared round by round with a sign test and a 95% confidence interval for each ratio: the before/after numbers for a change | `commits.md`, `commits-runs.csv` |
| [`replay_bench.py`](replay_bench.py) | Startup time for AOFs of increasing size, and how it grows; `--server redis` times real Redis loading the same files | `replay.md`, `replay-redis.md` |
| [`cpu_profile.py`](cpu_profile.py) | Where the server's CPU time goes (`perf` + flame graphs, kernel included) | `profile.md`, `*.svg`, `*-top.txt` |
| [`memory_layout.cpp`](memory_layout.cpp) | `sizeof` of the storage types, and heap bytes per key | stdout |
| [`insert_latency.cpp`](insert_latency.cpp) | How long one insert of a new key can take in the storage engine, and every rehash of the hash table (no network) | stdout |
| [`lookup_latency.cpp`](lookup_latency.cpp) | ns per GET and SET in the storage engine, and per `find()` in a plain `std::unordered_map` and in `KeyTable`, for tables of 1,000 to 4,000,000 keys, with uniform and Zipf keys; lookups independent (as in a pipeline) and one at a time (full latency); `--eviction both` measures both policies interleaved in one process. The knee where the table outgrows the cache shows the cost of memory latency without hardware counters (no network) | stdout |
| [`eviction_sim.cpp`](eviction_sim.cpp) | Miss ratio of the engine's eviction on cache traces (Zipf, Zipf with scans, or a CSV such as Twitter's), at capacities given as fractions of the trace's distinct keys, under `--eviction lru`, `sieve` or `both`. Each request is a GET; a miss SETs the key (no network) | stdout |
| [`eviction_reference.py`](eviction_reference.py) | Replays the requests `eviction_sim --dump-trace` ran through a textbook implementation of each policy; `--expect` fails unless every miss count is exactly the engine's. `--policy redis-lru` simulates Redis 7.0.15's `allkeys-lru` as its source does it (5 samples, a pool of 16, a 1-second clock), one run per `--seeds` seed, from a dump or a CSV with times (`csv:PATH`) | stdout |
| [`memory_compare.py`](memory_compare.py) | Memory per key, Kosh against real Redis for the same keys: growth of resident memory (RSS), with Kosh also under jemalloc to separate the allocator from the data layout | `memory.md` |
| [`layout_bias.py`](layout_bias.py) | How much `lookup_latency` moves from code and stack layout alone: every link order of its objects, each under several environment sizes, in shuffled order. The spread (with run-to-run noise) is the floor a microbenchmark gain must clear | stdout |
| [`hash_flood.cpp`](hash_flood.cpp) | Hash flooding: finds keys that collide under `std::hash` (timing the attacker's work), then the time to insert them into `KeyTable` with `std::hash` and with the keyed SipHash, as the count doubles; and SipHash's cost for ordinary keys, per hash and per lookup (independent and one at a time) at 1,000 to 1,000,000 keys | stdout |
| [`auth_compare.cpp`](auth_compare.cpp) | ns per password check by where the guess goes wrong (first byte, middle, last byte, right, too short), for `std::string`'s `==`, a loop that stops at the first difference, and the `constantTimeEquals` that `AUTH` uses: whether timing reveals how much of a guess is right | stdout |
| [`loadgen.cpp`](loadgen.cpp) (`kosh-loadgen`) | Open-loop latency of any Redis-compatible server: requests go out on a fixed schedule whether or not earlier replies have arrived, and latency counts from each request's intended send time (wrk2's method, over RESP). Prints a percentile only if the run can support it. Linux only | stdout, `--json` |
| [`ring_bench.cpp`](ring_bench.cpp) | ns per message for six cross-thread queues, one-way, there and back, and every thread to every other at 2, 4 and 8 pinned threads: what `SpscRing`'s cache-line spacing and remembered counters each buy, and a locked inbox for comparison. Linux only | stdout |
| [`shard_bench.py`](shard_bench.py) | Kosh at 1, 2, 4 and 8 cores against Redis 8 and Valkey 8.1 with `io-threads`, N separate Redis processes, and N threads sharing one locked map: peak throughput with `redis-benchmark`, then latency with `kosh-loadgen`, with rounds interleaved. Linux only | stdout, `peak.jsonl` |
| [`alloc_count.cpp`](alloc_count.cpp) | Heap allocations per request, layer by layer: parsing, the engine, the command and its reply, AOF logging. Exact counts, unaffected by machine noise | stdout |
| [`idle_clients.py`](idle_clients.py) | Whether the server survives N open connections (the `select()` limit) | stdout, server log |
| [`idle_load.py`](idle_load.py) | One busy client's latency and the server's CPU per request while N other connections sit idle, `select()` against epoll | `idle-load.md` |

Results live in [`results/`](results/), one directory per measured version (for example `results/v0-baseline/`).

## Running

Linux only (they read CPU times from `/proc`). Python 3.8+, standard library only.

```bash
sudo apt install redis-tools redis-server   # redis-benchmark, and real Redis for comparison
sudo systemctl disable --now redis-server   # the package starts a service on port 6379

python3 bench/bench.py        --out bench/results/<name>
python3 bench/compare_commits.py --out bench/results/<name>/commits.md before=<commit> after=<commit>
python3 bench/replay_bench.py --out bench/results/<name>
python3 bench/replay_bench.py --out bench/results/<name> --server redis
cmake -B build && cmake --build build --target memory_layout
./build/memory_layout > bench/results/<name>/memory-layout.md
cmake --build build --target insert_latency && taskset -c 2 ./build/insert_latency 100000 5
cmake --build build --target lookup_latency && taskset -c 2 ./build/lookup_latency   # about 3 minutes

# Eviction: the miss ratio on traces, then an exact check against a textbook policy.
# Twitter's trace (CC-BY 4.0) as libCacheSim ships it: 1,000,000 requests, 143,542 keys.
curl -sSfLo ~/traces/twitter_cluster52.csv --create-dirs \
    https://raw.githubusercontent.com/cacheMon/libCacheSim/develop/data/twitter_cluster52.csv
cmake --build build --target eviction_sim
./build/eviction_sim --dump-trace /tmp/trace.txt zipf:0.99 zipfscan:0.99 csv:$HOME/traces/twitter_cluster52.csv > sim.md
python3 bench/eviction_reference.py /tmp/trace.txt --expect sim.md

# Memory per key against real Redis (1,000,000 keys, 3 runs; about 2 minutes)
python3 bench/memory_compare.py --out bench/results/<name>/memory.md

# The layout-and-noise floor for lookup_latency's numbers (about 10 minutes)
python3 bench/layout_bias.py > bench/results/<name>/layout-bias.md
cmake --build build --target alloc_count && ./build/alloc_count

# Open-loop latency: 20,000 requests/s for 30 s against a server on port 6380.
# Each thread spins on a core. See results/open-loop/ for what a stall looks like.
cmake --build build --target kosh-loadgen && ./build/kosh-loadgen --port 6380 --rate 20000 --duration 30

# Hash flooding and the keyed hash's cost (about a minute)
cmake --build build --target hash_flood && taskset -c 2 ./build/hash_flood

# Password check timing, a typical length and the longest (about a minute)
cmake --build build --target auth_compare && taskset -c 2 ./build/auth_compare --length 32
taskset -c 2 ./build/auth_compare --length 512 --calls 50000

# Profiling also needs perf, the FlameGraph scripts, and kernel symbols:
sudo apt install linux-tools-generic
git clone https://github.com/brendangregg/FlameGraph ~/tools/FlameGraph
sudo sysctl -w kernel.perf_event_paranoid=1 kernel.kptr_restrict=0   # see below
python3 bench/cpu_profile.py  --out bench/results/<name>/profile

# The select() limit: 1,100 connections against a build (and against real Redis)
python3 bench/idle_clients.py --server-bin build-bench/kosh-server --connections 1100
python3 bench/idle_clients.py --redis-server redis-server --connections 1100

# A busy client among 1,000 idle ones, select() against epoll
python3 bench/idle_load.py --out bench/results/<name>/idle-load.md
```

Ubuntu's `/etc/sysctl.d/10-kernel-hardening.conf` sets `kptr_restrict` back to 1 whenever systemd starts. Under WSL2 that includes the distribution restarting after it has been idle, so run the `sysctl` right before profiling, in the same session.

`bench.py` takes about 25 minutes with the defaults; `--runs 1 --requests 100000` gives a quick check of the setup.

## Method, and why

- **A fresh server for every run,** in an empty temporary directory. Kosh replays its AOF at startup and the file grows with every write, so reusing one would measure a different server each time.
- **The same durability for both servers.** Kosh hands each AOF record to the OS without `fsync`; Redis runs with `--appendfsync no`, which is the same guarantee. Both are also measured with AOF off. RDB snapshots and automatic AOF rewrites are off for Redis, so neither can start in the middle of a run.
- **Random keys** (`-r 100000`). Without `-r`, every request uses one key, the best case for hash lookups and CPU caches.
- **Server and client pinned to different CPUs** with `taskset`, so they never compete for one CPU.
- **Two `redis-benchmark` threads.** The client makes about as many system calls per request as the server, so a single client thread can run out of CPU before the server does, and the result would then measure the client.
- **Middle of 3 runs, with the spread shown.** One run can be off for reasons unrelated to the code. The spread ((max − min) / median) says how much to trust a row: compare two versions only when the difference is clearly larger than both spreads.
- **Runs interleaved:** every combination once, then all again. Slow drift, such as a laptop heating up and throttling, then affects all combinations alike instead of whichever ran last.
- **The order alternates.** `bench.py` swaps which server goes first from run to run, and `compare_commits.py` reverses its order every other round. Interleaving balances slow drift, but drift *within* a run (a CPU's boost budget running out, for instance) favours whichever is measured first. Until Phase 4 the order never changed: Kosh always went before Redis, and neighbouring commits kept their order in all but one round (see [after-speed-fixes](results/after-speed-fixes/README.md)).
- **Commits compared in pairs, round by round** (`compare_commits.py`). Each round measures every version, in an order that rotates from round to round.
  - The ratio within a round compares two versions measured minutes apart, so drift across the session cancels out.
  - The report gives the median of those ratios, their range, and a two-sided sign test: how likely it is that the rounds would agree this much if the change made no difference.
  - With 5 rounds (the default) that all agree, p = 0.0625; with 6, p = 0.031.
  - Phase 3's comparison, which compared each version's median instead, had spreads up to 35%, too wide for small changes.
- **CPU use recorded for both sides.** A single-threaded server at 100% of a core is the bottleneck, so the row measures the server. If the server is well below 100% while the client is near its limit, the row measures `redis-benchmark`.
- **Optimised build enforced.** The scripts build `RelWithDebInfo` (`-O2 -g`) into `build-bench/` and refuse Debug or sanitizer builds.

## Limits of these numbers

- **WSL2 and hybrid CPUs.** Under WSL2, Linux sees virtual CPUs; pinning keeps the server and client apart, but Windows still decides which physical core runs each one. On a CPU with performance and efficiency cores, that adds noise. Numbers from different sessions or machines are not comparable; compare versions measured in the same session.
- **Loopback only.** Client and server share one machine, so there is no real network latency, and the client's own CPU use competes with the server's.
- **`redis-benchmark` is closed-loop:** each client waits for a reply before sending the next request. When the server stalls, fewer requests are sent, so the stall shows up in fewer samples than it should (coordinated omission). Its p99 is therefore optimistic. An open-loop load generator is planned.
- **`redis-benchmark` 7.0 has no random seed,** so each run uses a different random key sequence. That is part of the run-to-run spread.
- **With `--threads`, `redis-benchmark` rounds each run time up to a 250 ms tick.** It notices that a run has finished only on its 250 ms timer. With two threads, five 200,000-request runs ended 3–10 ms after a tick; with one thread, 15–131 ms after. Requests/s is therefore low by up to 250 ms ÷ run time. `bench.py` runs pipelined tests with 5,000,000 requests (20–60 s here), which keeps that under about 1%. Before this was found, pipelined runs used 1,000,000 requests (4–13 s), an error of up to 2–6%; that affects [v0-baseline](results/v0-baseline/README.md).
