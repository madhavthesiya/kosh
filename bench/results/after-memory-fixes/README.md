# After the memory-latency fixes (Phase 4B)

Measured on 2026-11-03 (UTC) with the scripts in [`bench/`](../../) ([method](../../README.md)), at commit `cbae3d9`, on the same machine as the earlier results: Intel Core Ultra 5 125H laptop, Ubuntu 24.04 under WSL2, GCC 13 `-O2`, real Redis 7.0.15. The laptop was plugged in, with other applications closed.

**The goal** (from the plan): Phase 4 left `GET` at parity with Redis, because it waits on memory (the hash table walk and the LRU relink), and each key took 2.6× Redis's memory. **Result:**
- **Against Redis at pipeline 16, Mini-Redis was faster in all 12 paired runs.** Its CPU per request was 0.67–0.77× Redis's (0.66–0.85× run by run). At `v2-fast` (an earlier session) that was 0.73–1.00×.
- **p99 was lower than Redis's in every run.**
- **Memory per key is about Redis's:**
  - 113 bytes against 120 at 800,000 keys;
  - 124 against 116 at 1,000,000, where Mini-Redis's table is in the middle of growing. It was 312 at the start of Phase 4B.
- **SIEVE misses 13–17% less than LRU on Zipf traces, and 1.4–4.7% less on Twitter's.** Every miss count was checked exactly against two independent implementations.
- **The slowest insert while growing to 1,000,000 keys, with nothing sized up front: 272–405 µs.**
- **No request allocates** (0 heap allocations per `GET` or `SET`).

| File | Contents |
|---|---|
| [`throughput.md`](throughput.md), [`throughput-runs.csv`](throughput-runs.csv) | Against Redis, pipeline 16, `appendfsync no`, the order alternating |
| [`commits.md`](commits.md), [`commits-runs.csv`](commits-runs.csv) | Three versions round by round: `v2-fast`, SIEVE with smaller entries, KeyTable |
| [`commits-cpu.md`](commits-cpu.md), [`cpu_paired.py`](cpu_paired.py) | The same rounds, for CPU per request (computed from the CSV) |
| [`lookup-latency.md`](lookup-latency.md) | Lookups in the engine (LRU and SIEVE), in `std::unordered_map` and in KeyTable, 1,000 to 1,000,000 keys |
| [`memory.md`](memory.md), [`memory-800k.md`](memory-800k.md), [`memory-layout.md`](memory-layout.md) | Memory per key against Redis at 1,000,000 and 800,000 keys; sizes of the types |
| [`eviction.md`](eviction.md), [`eviction-check.md`](eviction-check.md) | Miss ratios of LRU and SIEVE on three traces, and the exact check of every count |
| [`insert-latency.md`](insert-latency.md), [`alloc-count.md`](alloc-count.md) | The slowest insert of a new key; allocations per request |

## 1. Against real Redis

Pipeline 16, `appendfsync no` for both, 5,000,000 requests per test, 100,000 random keys, 50 clients. Median of 3 runs; the server measured first alternates from run to run.

| Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis | CPU per request vs Redis | p99, Mini-Redis vs Redis | At `v2-fast` (Phase 4): throughput, CPU |
|---|---:|---:|---:|---:|---|---:|
| SET, AOF on | 350,435 | 254,621 | **1.38×** | **0.67×** | 4.1 vs 5.0 ms | 1.21×, 0.73× |
| GET, AOF on | 407,664 | 332,934 | **1.22×** | **0.75×** | 3.7 vs 4.4 ms | 1.10×, 0.90× |
| SET, AOF off | 399,584 | 312,012 | **1.28×** | **0.71×** | 3.7 vs 4.8 ms | 1.00×, 0.99× |
| GET, AOF off | 391,665 | 338,547 | **1.16×** | **0.77×** | 3.7 vs 4.3 ms | 0.95×, 1.00× |

- **Mini-Redis was faster in every one of the 12 paired runs** (1.04–1.46×), used less CPU per request in every one (0.66–0.85×), and had the lower p99 in every one.
- **CPU per request is the fairer measure here:**
  - Mini-Redis ran at 81–91% of a core, not 100%, so its throughput was partly limited by the client and the loopback network;
  - Redis ran at 92–102%.
- **The `v2-fast` column is from an earlier session.** Ratios to Redis have moved by up to ±0.10 between sessions on this machine. So the change is certain where it is larger than that: `GET`, and both tests without AOF.

## 2. Commit by commit

Six rounds, each measuring all three versions, the order rotating and reversing; each version runs with its own defaults:
- `v2-fast` = `0c17b5a`, the end of Phase 4;
- `sieve-default` = `ecf73d5`: SIEVE as the default, and entries 312 → 184 bytes;
- `keytable` = `cbae3d9`: the own hash table, 110 bytes per key.

**This session was noisy between runs:** a version's per-round ratio ranged as low as 0.46 in one round. So most changes are **not resolved** here. (`commits-cpu.md` gives requests per CPU-second, so a ratio of 1.10 there is 1 − 1/1.10 ≈ 9% less CPU per request.)

| Change | Throughput ([`commits.md`](commits.md)) | CPU per request ([`commits-cpu.md`](commits-cpu.md)) |
|---|---|---|
| SIEVE with smaller entries, against `v2-fast` | not resolved (0.98–1.06×) | not resolved (0.98–1.07× the requests per CPU-second) |
| KeyTable, against SIEVE with smaller entries | 1.03–1.07×, 4–6 of 6 rounds; not resolved | **9% less CPU per request for SET without AOF: 6 of 6 rounds, p = 0.03, interval 4–12% less**; the other three tests 8–10% less, in 5 of 6 rounds each (not resolved) |
| KeyTable, against `v2-fast` | not resolved | 9–13% less, 5 of 6 rounds each; not resolved (one wild round) |

- **The engine-level gains in section 3 are larger** (lookups 2× faster), but the engine is only part of a request: Phase 4's profile put about half of each request in kernel networking, which neither version changes.
- **Against Redis (section 1), Mini-Redis was ahead in every run.** The commit-by-commit question, *which* change did how much end to end, is answered only for KeyTable on `SET` without AOF.

## 3. Lookups in the engine

[`lookup-latency.md`](lookup-latency.md): ns per lookup, median of 5 repeats, uniform keys. Every column is measured in one process, interleaved; the machine was quiet (narrow ranges). Engine memory: 100.3 MB for 1,000,000 keys.

| Keys | `find()` in `std::unordered_map` | `find()` in KeyTable | Engine `GET`, LRU | Engine `GET`, SIEVE |
|---:|---:|---:|---:|---:|
| 1,000 (in cache) | 28.0 | 31.8 | 46.3 | 44.3 |
| 100,000 (10 MB) | 103.4 | **50.3** | 74.9 | 73.6 |
| 1,000,000 (100 MB) | 222.9 | **106.9** | 187.7 | **168.0** |

- **KeyTable finds a key in half the time** at 100,000 and 1,000,000 keys: two dependent memory reads (the bucket, then the entry) where `std::unordered_map` needs three or four. Zipf keys: 87 → 46 ns and 151 → 79 ns.
- **It is 14% slower when everything is in cache.** With no misses to save, instruction count decides, and its tag scan is a loop over 7 slots.
- **The whole engine `GET`** (lock, lookup, eviction bookkeeping, the value) **is now faster than a bare `std::unordered_map` lookup was:** 75 against 103 ns at 100,000 keys.
- **SIEVE against LRU, in the engine:** 10% faster at 1,000,000 keys, the same at 100,000. With the LRU links inside the entries and 100,000 keys in cache, LRU's relink is cheap there. One lookup at a time, they are the same (250 against 251 ns). An earlier, busy-machine run showed SIEVE 19–33% faster on the previous layout; that is superseded.

## 4. Memory per key

`memory_compare.py`: the growth of each server's resident memory (RSS) after loading N keys of 16 bytes with 3-byte values; 3 runs each, identical to within 1 byte.

| Keys | Mini-Redis (glibc) | Mini-Redis (jemalloc) | Redis (jemalloc) | Redis's own count (`used_memory`) |
|---:|---:|---:|---:|---:|
| 800,000 | **113** | 111 | 120 | 107 |
| 1,000,000 | 124 | 121 | **116** | 104 |

- **The two points differ because of where each table is in its growth:**
  - KeyTable grows at 917,504 keys, so at 1,000,000 it is in the middle of moving into a table twice the size, and holds part of both;
  - Redis's dict grows at 1,048,576 keys.
  - Between growths, Mini-Redis uses about 6% less memory per key than Redis; in the middle of one, about 7% more.
- **At the start of Phase 4B, Mini-Redis took 312 bytes per key** (glibc mallinfo2): four allocations per key, the key stored twice.
- **[`memory-layout.md`](memory-layout.md):** each key is now one 72-byte entry with the key's bytes after it, in one allocation, plus its share of 64-byte buckets. glibc's own count is 100 bytes per key at 1,000,000 keys.

## 5. Eviction

[`eviction.md`](eviction.md): each request is a `GET`, and a miss `SET`s the key. The capacity is in keys, as a fraction of each trace's distinct keys. Synthetic traces have 10,000,000 requests.

| Trace | Distinct keys | Capacity | LRU miss ratio | SIEVE miss ratio | Fewer misses |
|---|---:|---:|---:|---:|---:|
| Zipf 0.99 | 780,666 | 0.1% | 0.636 | 0.528 | 17% |
| | | 1% | 0.454 | 0.375 | 17% |
| | | 10% | 0.258 | 0.218 | 16% |
| Zipf 0.99 with scans | 1,752,419 | 0.1% | 0.618 | 0.525 | 15% |
| | | 1% | 0.466 | 0.389 | 16% |
| | | 10% | 0.294 | 0.257 | 13% |
| Twitter cluster52 (1,000,000 requests) | 143,542 | 0.1% | 0.496 | 0.488 | 1.4% |
| | | 1% | 0.338 | 0.322 | 4.7% |
| | | 10% | 0.207 | 0.197 | 4.6% |

- **Every miss count is exact:** [`eviction-check.md`](eviction-check.md) replays the same requests through a textbook LRU and through SIEVE written from the paper's pseudocode, and all 18 counts are identical.
- **Not tested here:** the authors report that SIEVE loses on some old block-storage traces, where scans mix with popular keys. These traces don't include that case, so the claim is "on these traces".

## 6. The slowest insert, and allocations

- **[`insert-latency.md`](insert-latency.md): 1,000,000 new keys, with the table sized for nothing.**
  - Median 0.13 µs, p99.9 4.9–6.1 µs, slowest 286, 405 and 272 µs in 3 repeats, at scattered positions.
  - With `std::unordered_map`, growing all at once, one insert took 20 ms at 85,000 keys (Challenge 13), and more for bigger tables.
  - The first version of KeyTable still had 2.9–4.1 ms stalls at fixed positions; commit `619ab23` explains each cause and its fix.
- **[`alloc-count.md`](alloc-count.md):** 0 heap allocations per `GET` or `SET` request, at every layer.

## Caveats

- **One machine:** a laptop under WSL2, with virtual CPUs that Windows moves between core types; pinning can't pick the physical core.
- **Commit-by-commit noise:** the rounds of section 2 varied by up to 2×, which is why most commit-by-commit changes are not resolved.
- **The comparison with Redis is one session of 3 runs per test.** It agreed in every run, but its ratios are not comparable with other sessions' to better than about ±0.10.
- **Redis is version 7.0.15,** as in every earlier result. Redis 8 and Valkey 8.1 are newer and have their own memory and speed work; comparing against them is planned.
