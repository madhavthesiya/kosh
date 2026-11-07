# SIEVE against Redis's own LRU

Kosh's README says SIEVE misses less than LRU. But Redis has never used exact LRU: its `allkeys-lru` samples keys. This compares SIEVE with what Redis 7.0.15 actually does.

## How

- **Redis's policy, simulated from its source** (`bench/eviction_reference.py --policy redis-lru`; tag 7.0.15, every step cited by file and line in the script):
  - each eviction samples 5 keys (`maxmemory-samples`) into a pool of 16 sorted by idle time (`evict.c:146-240`), and evicts the pool's best candidate that still exists (`evict.c:593-633`);
  - keys are sampled as `dictGetSomeKeys` does (`dict.c:711-788`): a run of consecutive buckets from a random start, not uniformly;
  - idle time comes from a 1-second LRU clock (`server.h:849`). A pool entry keeps the idle time it was sampled with, and ties go to the left (a strict `<`);
  - the order is Redis's: a `SET` takes the cache over its limit, and the next command evicts first, so the new key is a candidate.
- **The same traces and capacities as B1** ([`../after-memory-fixes/eviction.md`](../after-memory-fixes/eviction.md)). Exact LRU over this run's input reproduced all 9 of B1's LRU counts, so B1's LRU and SIEVE columns below are on identical requests.
- **The clock:** Twitter's trace has its own times, in seconds (1,000,000 requests over 509 s, about 1,965 a second). The Zipf traces have none, so they assume 2,000 requests a second, about the same.
- **5 seeds per row** (hash seed and sampling): the median, and the range in the raw files. Miss counts are deterministic per seed, so no quiet machine is needed.
- **Also with an exact clock** (a tick per request, no ties), to separate the cost of sampling from the cost of the 1-second clock.

## Results

Misses ([`redis-lru-trace.md`](redis-lru-trace.md), [`redis-lru-exact.md`](redis-lru-exact.md)):

| Trace | Capacity | Exact LRU | Redis LRU | Redis LRU, exact clock | SIEVE | SIEVE against Redis LRU |
|---|---:|---:|---:|---:|---:|---:|
| Zipf 0.99 | 0.1% | 6,355,494 | 6,545,297 (+3.0%) | 6,377,361 (+0.3%) | 5,281,637 | 19.3% fewer |
| | 1% | 4,543,724 | 4,565,675 (+0.5%) | 4,564,438 (+0.5%) | 3,754,102 | 17.8% fewer |
| | 10% | 2,581,993 | 2,599,974 (+0.7%) | 2,600,074 (+0.7%) | 2,178,173 | 16.2% fewer |
| Zipf 0.99 with scans | 0.1% | 6,176,494 | 6,224,297 (+0.8%) | 6,195,553 (+0.3%) | 5,250,630 | 15.6% fewer |
| | 1% | 4,656,362 | 4,672,381 (+0.3%) | 4,672,244 (+0.3%) | 3,892,385 | 16.7% fewer |
| | 10% | 2,944,615 | 2,958,391 (+0.5%) | 2,958,613 (+0.5%) | 2,570,244 | 13.1% fewer |
| Twitter cluster52 | 0.1% | 495,485 | 544,446 (+9.9%) | 499,370 (+0.8%) | 488,354 | 10.3% fewer |
| | 1% | 337,587 | 340,706 (+0.9%) | 339,143 (+0.5%) | 321,586 | 5.6% fewer |
| | 10% | 206,786 | 207,699 (+0.4%) | 207,667 (+0.4%) | 197,254 | 5.0% fewer |

Percentages in the Redis columns are against exact LRU. Across the 5 seeds, no row's misses spread more than 0.6%.

- **SIEVE misses less than Redis's LRU in every row and every seed:** 13–19% on the Zipf traces, 5–10% on Twitter's.
- **Sampling costs almost nothing.** With an exact clock, Redis's 5 samples and pool of 16 come within 0.3–0.8% of exact LRU. This is why Redis never paid for exact LRU (two pointers per key, and pointer writes on every hit).
- **The 1-second clock costs more, in small caches.** At 0.1% capacity, the cache turns over in less than a second, so most sampled keys tie on idle time and the choice among them is close to arbitrary. On Twitter that adds 9.9% misses over exact LRU, where the exact clock adds 0.8%. At 1% and 10% it makes no difference.

## Limits

- **A simulation, not the server.** It follows Redis's code step by step, and a self-test checks the cases worked out by hand: the stale pool entry that evicts a key read two seconds before, ties going left, `dictGetSomeKeys` reaching every key. It hasn't been compared with a running Redis, whose `maxmemory` is in bytes, not keys.
- **Not simulated:** the hash table's incremental rehash (it only grows while the cache fills), and the 24-bit clock wrapping (after 194 days).
- **The Zipf rows depend on the assumed rate:** more requests a second means more ties, and more misses at small capacities.
- **Not measured: `allkeys-lfu`,** an optional part of this item. It needs its own assumptions (counter decay per minute against an assumed rate).
