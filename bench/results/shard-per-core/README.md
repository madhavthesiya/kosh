# Several threads, against fair baselines

Measured on 2026-11-09, at commit `7667c0d`, with [`shard_bench.py`](../../shard_bench.py). One session, 3 interleaved rounds, 288 runs, none failed. Other applications closed, on mains power.

**Machine:** Intel Core Ultra 5 125H laptop (4 performance cores and 10 efficiency cores; Windows reports 14 cores, 18 logical CPUs), Ubuntu 24.04 under WSL2, kernel 6.6, 7 GB. WSL presents 18 identical CPUs, so a thread pinned to one may run on a fast or a slow core.

**Servers:** Kosh at `7667c0d`; Kosh at `dd5e505`, where N event loops share one locked map; Kosh at `8ba5b8f`, before threads; Redis 8.10.3 and Valkey 8.1.11 built from source with default options; Redis 7.0.15 from Ubuntu. Kosh is built with `-O2 -g`.

**Method:**
- Servers are pinned to CPUs 0 to C-1, and `redis-benchmark` 7.0.15 to CPUs 8-17, with 8 threads.
- 100,000 keys, 64-byte values, 400 connections at pipeline 1 and 1,600 at pipeline 16.
- `io-threads` is tried at the number of cores and at what the project's config file suggests, and the better result is shown.
- For N Redis processes, one per core, each client talks to one process about its own keys, so no routing cost is paid; the throughputs are added.
- Rounds rotate, so that a machine changing speed affects every server alike. Each number is the median of 3 rounds; the raw values are in [`raw/peak.jsonl`](raw/peak.jsonl).

**Why so many connections:** a connection does one round trip at a time, and on this VM a round trip takes milliseconds. With 48 connections, Kosh at 4 threads did about 290,000 requests/s, as much as at one thread, because the server was never given enough work. With 1,600 it did 520,000 in the same test, so 48 connections hid the effect of threads.

## Peak throughput

Thousands of requests per second.

**GET, pipeline 16**

| Cores | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| Kosh | 412 | 395 | **694** | 600 |
| One locked map | 412 | 358 | 305 | 284 |
| Redis 8 `io-threads` | 341 | 352 | 495 | 563 |
| Valkey 8.1 `io-threads` | 314 | 306 | 278 | 313 |
| N Redis processes | – | 434 | 778 | 750 |

**SET, pipeline 16, no AOF**

| Cores | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| Kosh | 425 | 458 | **514** | 424 |
| One locked map | 398 | 339 | 361 | 298 |
| Redis 8 `io-threads` | 372 | 298 | 409 | 416 |
| Valkey 8.1 `io-threads` | 332 | 218 | 249 | 249 |
| N Redis processes | – | 418 | 571 | 386 |

**SET, pipeline 16, AOF `everysec`**

| Cores | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| Kosh | 269 | 381 | **435** | 405 |
| Redis 8 `io-threads` | 266 | 230 | 260 | 244 |
| Valkey 8.1 `io-threads` | 254 | 187 | 158 | 166 |
| N Redis processes | – | 314 | 338 | 463 |

**GET, pipeline 1**

| Cores | 1 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| Kosh | 46 | 51 | 52 | 42 |
| One locked map | 48 | 54 | 54 | 48 |
| Redis 8 `io-threads` | 41 | 39 | 54 | 46 |
| Valkey 8.1 `io-threads` | 41 | 37 | 56 | 44 |
| N Redis processes | – | 45 | 57 | 56 |

Server CPU per request for Kosh, GET at pipeline 16: 2.6, 4.6, 5.5 and 10.5 µs at 1, 2, 4 and 8 cores.

## What this shows

- **Shards beat a shared lock.** At 4 cores, 2.3× the locked map on GET, and the locked map gets slower as threads are added (412,000 to 284,000).
- **Kosh scales to 4 cores: 1.68× on GET, 1.21× on SET.** At 8 cores it falls back, which fits a laptop with 4 fast cores where the load generator competes for the rest. This wasn't isolated from the VM's effects.
- **Against Redis 8 with `io-threads`** at 4 cores: 1.40× on GET, 1.26× on SET, 1.67× on SET with the AOF on. At 8 cores, 1.07× on GET.
- **N separate Redis processes are the hardest baseline, and beat Kosh** on GET and SET without the AOF at 4 cores (Kosh is 0.89× and 0.90×) and on GET at 8 cores. They pay no routing cost, because the client does the sharding. Kosh is ahead with the AOF on at 4 cores (1.29×) and on SET at 8 cores.
- **At pipeline 1 nothing scales here.** Every server stayed between 37,000 and 57,000. The limit is outside the servers.
- **With one thread nothing got slower.** Against the version before threads (`8ba5b8f`): GET at pipeline 16, 412,000 and 398,000; SET, 425,000 and 411,000; GET at pipeline 1, 46,000 and 44,000.

## Latency

Open loop, from [`kosh-loadgen`](../open-loop/README.md), 2 generator threads, 64 connections, 10% SET. Latency counts from each request's intended send time. "N/A" is the tool refusing a percentile because the generator was late by more than a tenth of it. All values are in [`raw/open-loop.txt`](raw/open-loop.txt).

At 20,000 requests/s:

| | p50 | p99 |
|---|---:|---:|
| Kosh, 1 core | 631 µs | 1.47 ms |
| Kosh, 4 cores | 967 µs | 2.28 ms |
| Kosh, 8 cores | 1.11 ms | N/A |
| Redis 8, 1 core | 614 µs | 1.59 ms |
| Redis 8, 4 cores | 1.06 ms | 8.16 ms (from 1 second of the 10) |
| Valkey 8.1, 1 core | 741 µs | 1.85 ms |
| Valkey 8.1, 4 cores | 877 µs | N/A |
| 4 Redis processes | 705 µs | N/A |

At 90% of each server's own peak, one core: Kosh p99 20.7 ms, Redis 8 16.5 ms, Valkey 18.9 ms. Kosh at 4 cores: 23.7 ms.

So a request that crosses threads costs latency at a fixed low load: p99 rises 1.55× from 1 core to 4. Most multi-core percentiles can't be compared, because the generator was too late on this machine for the tool to trust them. "Scales without hurting p99" is not claimed.

## The fix the first trial found

The first trial showed more threads making things slower. Each reply of a pipeline was sent as it arrived from another thread, at about 4× the system calls per request of one thread. Replies now wait for the commands outstanding when the last ones left. Measured separately, 5 rounds, 1,600 connections, pipeline 16:

| | without | with |
|---|---:|---:|
| GET, 4 threads | 453,000 | 691,000 |
| GET, 8 threads | 348,000 | 644,000 |
| SET, 4 threads | 302,000 | 527,000 |
| SET, 8 threads | 278,000 | 461,000 |

Spinning for 50 µs before sleeping made no measurable difference, and polling for 1 ms after recent work gave no steady gain and nearly doubled the CPU per request at 8 threads. Neither was kept.

## The queue between threads

[`ring_bench`](../../ring_bench.cpp), 15 rounds ([raw](raw/ring_bench.txt)). ns per message, one-way:

| shared line | padded | remembered counters only | both (`SpscRing`) | a locked inbox |
|---:|---:|---:|---:|---:|
| 7.2 | 7.0 | 8.6 | 6.1 | 195.9 |

In the all-to-all test at 2 threads, where a thread both sends and receives: 23.5, 38.3, 29.4, 15.6 and 128.3. The locked inbox costs about 30× the ring per message one-way and 4× a round trip. Padding alone and remembered counters alone did not help consistently; together they were the cheapest of the 64-byte layouts in all five tests. 128-byte spacing was cheaper again in three tests (one-way, a round trip, and all-to-all at 2 threads, by 16–23% with overlapping ranges) and level in the other two. An earlier, noisier run showed no difference. It is not used: the ring carries batches rather than single commands, and 128-byte spacing was not tried in the whole server.

## Sharing out the key limit

[`eviction_sim --threads`](raw/eviction-threads.md), exact rather than timed. SIEVE's miss ratio with the capacity split across 1, 2, 4 and 8 threads, on Zipf 0.99 and Twitter's cluster 52, moved by under 1% in every case: splitting the limit costs no hit rate on these traces.

## How far to trust this

- One laptop VM, one session. Numbers from another session wouldn't be comparable. Bare metal is untested.
- Runs vary: the same one-thread GET test gave 995,000, 398,000 and 412,000 in its three rounds, and Kosh's AOF row at one core 628,000, 269,000 and 240,000. Medians are shown, and small differences mean nothing.
- The load generator is on the same machine and closed-loop, so these are peaks under this setup, not capacity.
- 8 cores needs 8 equal cores plus room for the generator, which this machine doesn't have.
