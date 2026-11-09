# Benchmarks

What has been measured, how, and how to run it. Everything is on one laptop under WSL2, so read the limits stated with each result.

## Several threads

Kosh at 1, 2, 4 and 8 cores against Redis 8 and Valkey 8.1 with `io-threads` and N separate Redis processes, with the method and the limits: [`bench/results/shard-per-core`](../bench/results/shard-per-core/README.md).

## Performance on one thread

Kosh against real Redis 7.0.15 on the same machine, in the same session, with the same durability (both with `appendfsync no`). Pipeline 16, 100,000 random keys, 50 clients, middle of 3 runs, the server measured first alternating from run to run. Intel Core Ultra 5 125H laptop, Ubuntu 24.04 under WSL2, GCC 13 `-O2`. Full results, commands and caveats: [after the memory-latency fixes](../bench/results/after-memory-fixes/README.md) (Phase 4B). Earlier stages: [after the speed fixes](../bench/results/after-speed-fixes/README.md) (Phase 4), [after the correctness fixes](../bench/results/after-correctness-fixes/README.md), [after the network fixes](../bench/results/after-network-fixes/README.md), and the starting point, [v0-baseline](../bench/results/v0-baseline/README.md).

| Test, pipeline 16 | Kosh | Redis | Kosh / Redis | CPU per request vs Redis | At Phase 4 (`v2-fast`) | At v0-baseline |
|---|---:|---:|---:|---:|---:|---:|
| SET, AOF on | 350,435 req/s | 254,621 req/s | **1.38×** | **0.67×** | 1.21× | 0.43× |
| GET, AOF on | 407,664 req/s | 332,934 req/s | **1.22×** | **0.75×** | 1.10× | 0.56× |
| SET, AOF off | 399,584 req/s | 312,012 req/s | **1.28×** | **0.71×** | 1.00× | 0.50× |
| GET, AOF off | 391,665 req/s | 338,547 req/s | **1.16×** | **0.77×** | 0.95× | 0.48× |

What the measurements show:
- **Kosh was faster than Redis in all 12 paired runs,** with lower CPU per request in every one (0.66–0.85×) and a lower p99 in every one (3.7–4.1 ms against 4.3–5.0, medians).
  - CPU per request is the fairer measure here: Kosh ran at 81–91% of a core, so the client and the loopback network limited its throughput too.
  - Ratios to Redis move by up to ±0.10 between sessions on this machine, so the change since Phase 4 is certain where it is larger than that: `GET`, and both tests without AOF, which were at parity.
- **Phase 4B removed the waits on memory that kept `GET` at parity:**
  - **a hash table of its own** finds a key in half the time of `std::unordered_map` at 100,000 and 1,000,000 keys (Challenge 13);
  - **one allocation per key:** memory per key fell from 312 bytes to 113, against Redis's 120 at 800,000 keys;
  - **SIEVE eviction:** 13–17% fewer misses than LRU on Zipf traces.
  - Commit by commit, this session's rounds were too noisy to separate most changes; KeyTable alone resolved at 9% less CPU per request for `SET` without AOF.
- **Phase 4 made it competitive:**
  - **group commit:** the AOF records of every command in a pass of the event loop go out in one `write()`, before any of their replies; 100,000 pipelined `SET`s made 132 `write()` calls where they made 100,002;
  - **no request allocates memory:** commands are parsed into reused strings, and replies and AOF records are built straight into their buffers. [`bench/alloc_count`](../bench/alloc_count.cpp) counts 0 allocations per `GET` or `SET`, where Phase 3's code made 5 and 11.
- **No freeze while the table grows:** growing to 1,000,000 keys with nothing sized up front, the slowest insert took 272–405 µs. A table that grew all at once used to freeze every client for 20 ms at 85,000 keys (Challenges 13 and 15).
- **From Phase 4's session:**
  - **with 1,000 idle clients, `epoll` uses 9–14× less CPU per busy request than `select()`**, which rebuilds and scans every socket on each wakeup;
  - **with `--appendfsync always`, both servers wait for the disk and tie**: each syncs once per pass of its event loop, about 390 `SET`s per `fdatasync()` at pipeline 16;
  - **without pipelining, the two tie** (1.08× for `SET`, 0.98× for `GET`): neither server reaches 100% CPU, and the network round trip sets the pace for both;
  - **startup, replaying a 1,000,000-command AOF (45 MB):** 0.68 s against Redis's 0.64 s.

Raw numbers vary between sessions on this machine (WSL2 on a hybrid CPU) by more than most code changes, so comparisons are made within one session, with the order of measurements alternating, and reported with their spread. The full results also say which differences were too small to resolve.

> **Why does pipelining help so much?** Without pipelining, every command waits a full network round trip. With P=16 the client sends 16 commands at once, the server reads them with a single `recv()` and sends all 16 replies with a single `send()`, and the round trip is shared by 16 commands.

Earlier numbers from Windows (one key for every request, unoptimised build) are kept in [stress-test-results/](../stress-test-results/); they are not comparable.


### Benchmark

The numbers in this README come from the scripts in [`bench/`](../bench/). They build an optimised binary, run Kosh and real Redis the same way, repeat each test 3 times, and record the machine, compiler and commit next to the results. [bench/README.md](../bench/README.md) explains the method.

```bash
python3 bench/bench.py --out bench/results/my-run      # Linux; about 25 minutes
```

For a quick manual run:

```bash
rm -f appendonly.aof                      # the AOF keeps growing across runs
./build/kosh-server --maxkeys 1000000
redis-benchmark -p 6379 -t set,get -n 1000000 -r 100000 -c 50
```

- `-r 100000` spreads requests over 100,000 random keys; without it, every request hits one key.
- `--maxkeys` must be at least that many, or most `SET`s also evict.
- Leave out `-q` to see latency percentiles.


### Open-loop latency

`redis-benchmark` waits for each reply before sending the next request, so when a server stalls, the requests that should have been sent meanwhile are never measured. This is coordinated omission. [`kosh-loadgen`](../bench/loadgen.cpp) applies [wrk2](https://github.com/giltene/wrk2)'s method to RESP: requests follow a fixed schedule, and latency counts from each one's intended send time. It works with any Redis-compatible server, on Linux, and prints N/A rather than a percentile the run can't support. YCSB and Dragonfly's `dfly_bench` reached similar designs first.

With Redis frozen for 1 second during a 30-second run at 10,000 requests/s (loopback, WSL2, one run each; [details](../bench/results/open-loop/README.md)):

| | `memtier_benchmark --rate-limiting` | `kosh-loadgen` | Arithmetic |
|---|---:|---:|---:|
| p99 | 1.43 ms | 713 ms | 700 ms |
| p99.9 | 2.01 ms | 981 ms | 970 ms |
| Max | 1003.5 ms | 1.010 s | 1000 ms |

```bash
cmake --build build --target kosh-loadgen
./build/kosh-loadgen --port 6380 --rate 20000 --duration 30
```
