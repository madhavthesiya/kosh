# After the correctness fixes (Phase 3)

Measured on 2026-11-02 (UTC) with the scripts in [`bench/`](../../) ([method](../../README.md)), on the same machine as [v0-baseline](../v0-baseline/README.md): Intel Core Ultra 5 125H laptop, Ubuntu 24.04 under WSL2, GCC 13 `-O2`, real Redis 7.0.15. The laptop was plugged in with other applications closed.

**Code measured:** `9f39a71`, the last code commit of Phase 3. The commits after it change only results and documentation.

Phase 3 fixed correctness, not speed, so the question here is whether the fixes cost anything. The one change expected to be faster is AOF replay.

| File | Contents |
|---|---|
| [`commits.md`](commits.md) | Before and after Phase 3, measured in one interleaved session |
| [`throughput.md`](throughput.md), [`throughput-runs.csv`](throughput-runs.csv) | Mini-Redis against Redis, all tests, every run |
| [`replay.md`](replay.md), [`replay-redis.md`](replay-redis.md) | Startup time replaying AOFs of 25,000 to 1,000,000 commands, Mini-Redis and Redis on the same files |
| [`memory-layout.md`](memory-layout.md) | `sizeof` of the storage types and heap per key |

## 1. Did the fixes cost throughput?

**No measurable cost.** Two measurements, from the same session, agree.

**Against Redis** (section 2), Mini-Redis's ratios are the same as after the network fixes:

| Pipeline 16 | After network fixes | After correctness fixes |
|---|---:|---:|
| SET, AOF on | 0.81× | 0.84× |
| GET, AOF on | 0.96× | 0.95× |
| SET, AOF off | 1.00× | 1.00× |
| GET, AOF off | 0.97× | 1.01× |

**Before and after, directly** ([`commits.md`](commits.md)): pipeline 16, 5,000,000 requests per test, median of 3 interleaved rounds, with the spread in brackets.

| Test | Before (`ac666b2`) | After (`9f39a71`) | After / before |
|---|---:|---:|---:|
| SET, AOF on | 144,676 (21%) | 176,685 (35%) | 1.22× |
| GET, AOF on | 229,600 (10%) | 273,733 (15%) | 1.19× |
| SET, AOF off | 201,629 (27%) | 214,417 (7%) | 1.06× |
| GET, AOF off | 232,320 (29%) | 224,165 (11%) | 0.96× |

These differences are not gains. They are within the spreads, and the GET rows show the noise directly. A GET does the same work whether the AOF is on or off, yet it measured 1.19× in one row and 0.96× in the other. This part of the session was noisy; see section 5. It shows no slowdown, and it can't resolve a difference smaller than about 20%.

No cost was expected. Phase 3 changed these parts of the hot path:
- **Command lookup:** one hash lookup in the command table, where there used to be a chain of string comparisons.
- **AOF write:** records go out with `write()` directly instead of through `std::ofstream`. Either way, it is one system call per command.
- **Write commands:** one atomic load to check that earlier AOF writes haven't failed (the `MISCONF` check).
- **Expiry:** a `SET` without a TTL never touches the expiry index, and a store that isn't full doesn't read the clock.

## 2. Against real Redis, same session

[`throughput.md`](throughput.md): 1,000,000 requests per test without pipelining, 5,000,000 with. Both servers write their AOF to the OS without `fsync` (or have it off).

| Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis | After network fixes | At v0 |
|---|---:|---:|---:|---:|---:|
| SET, no pipelining, AOF on | 28,395 | 30,734 | 0.92× | 0.97× | 0.97× |
| GET, no pipelining, AOF on | 31,097 | 30,500 | 1.02× | 1.08× | 0.97× |
| SET, pipeline 16, AOF on | 191,666 | 228,990 | **0.84×** | 0.81× | 0.43× |
| GET, pipeline 16, AOF on | 277,577 | 293,686 | **0.95×** | 0.96× | 0.56× |
| SET, pipeline 16, AOF off | 276,855 | 277,393 | **1.00×** | 1.00× | 0.50× |
| GET, pipeline 16, AOF off | 291,630 | 288,934 | **1.01×** | 0.97× | 0.48× |

Mini-Redis ran at 98–100% CPU in the pipelined tests and Redis at 91–95%, so CPU per request (server CPU ÷ requests/s) remains the fairer measure:

| Pipeline 16 | Mini-Redis | Redis | Mini-Redis needs | After network fixes |
|---|---:|---:|---:|---:|
| SET, AOF on | 5.2 µs | 4.0 µs | 1.30× | 1.33× |
| GET, AOF on | 3.6 µs | 3.1 µs | 1.14× | 1.10× |
| SET, AOF off | 3.5 µs | 3.4 µs | 1.03× | 1.14× |
| GET, AOF off | 3.4 µs | 3.3 µs | 1.05× | 1.14× |

The last column comes from a different session. The lower AOF-off ratios come partly from Redis measuring slower this time, so they don't show an improvement.

- **The largest remaining gap is still the AOF.** Mini-Redis makes one `write()` per command, where Redis writes once per event-loop pass. SET with AOF on spends 47% of its CPU time in the kernel, against Redis's 34%. Group commit (Phase 4) addresses this.
- **Without pipelining, both servers still tie.** As before, that says nothing about the code: neither reaches 100% CPU, and the cost of each round trip sets the pace.
- **p99 latency at pipeline 16:** 5.4–8.5 ms for Mini-Redis and 4.7–6.0 ms for Redis. The 8.5 ms is SET with AOF on.
- **Max latency, not yet explained:** in every run, Mini-Redis's SET max (23–77 ms) was above its GET max (14–22 ms). Redis's SET max mostly wasn't (13–17 ms, with one run at 48 ms).
  - SETs are where new keys arrive. Two candidates are the hash map rehashing all at once as it grows (Redis rehashes incrementally) and memory being allocated.
  - Neither has been measured yet. The network-fixes session doesn't show the pattern, so it may also be specific to this session.

## 3. AOF replay is now linear

[`replay.md`](replay.md), [`replay-redis.md`](replay-redis.md): startup time from launching the server to its first `PONG`, on AOFs of N `SET` commands, median of 3. After each start, a `GET` checks that the whole file was loaded.

| Commands | AOF size | Mini-Redis | Redis | Mini-Redis at v0 |
|---:|---:|---:|---:|---:|
| 100,000 | 4.5 MB | 119 ms | 69 ms | 11.5 s |
| 400,000 | 18.0 MB | 296 ms | 254 ms | 174 s |
| 1,000,000 | 45.0 MB | **724 ms** | 648 ms | over 900 s |

- **Before:** replay erased each command from the front of a buffer holding the rest of the file, so it moved all the remaining bytes once per command. Startup time grew quadratically.
- **Now:** replay parses from a read position over the unchanged buffer, as the server's read path has done since Phase 2.
- **The cost is now linear.** The growth exponent between sizes is 0.6–1.2, and each command costs about 0.7 µs, against Redis's 0.6 µs.
- **The Redis column is from this session.** Redis loaded a 1,000,000-command file of the same kind in 1.19 s in the v0 session: the machine's speed varies that much between sessions.
- **The quick measurement in the replay commit (`26dc03d`) is not comparable.** It took 1.83 s for 1,000,000 commands, but it was one run on a busy machine, labelled as not the official result.

## 4. Memory

[`memory-layout.md`](memory-layout.md): unchanged. **316 bytes of heap per key** for a 16-byte key and a 3-byte value, and `CacheEntry` is still 112 bytes: a flag and an 8-byte time point for the expiry, as before.

Keys with a TTL also have a node in the new expiry index. The benchmark's keys have none, so that cost isn't measured here.

## 5. How far to trust these numbers

- **The full benchmark was quiet, but the commit comparison wasn't.** The comparison ran first, and its spreads reached 35%.
  - Its numbers were also lower than the full benchmark's for the same code. For example, GET with AOF off measured 224k in the comparison and 292k against Redis later in the session.
  - In the full benchmark, the pipelined rows had spreads of 2.5–8.3%. The exception is Redis's GET with AOF off, at 33.4%, because one run measured 219k.
  - So section 1 relies on the ratios against Redis, and uses the direct comparison only to confirm that nothing got slower.
- **Compare within a session, not across sessions.** As in the earlier results, raw numbers move between sessions by more than most code changes.
- **`compare_commits.py` keeps only the median of each commit.** Recording each round's numbers would allow a paired comparison: the two versions measured minutes apart, round by round. That is less sensitive to the machine drifting during a session. Worth adding before Phase 4, whose speed changes are smaller and need it.
- **No pauses of about 1 s showed up this session**, unlike the network-fixes session's. The largest max latency in any run was 229 ms, in Redis's unpipelined GET with AOF on.
