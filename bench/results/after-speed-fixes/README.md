# After the speed fixes (Phase 4)

Measured on 2026-11-03 (UTC) with the scripts in [`bench/`](../../) ([method](../../README.md)), on the same machine as the earlier results: Intel Core Ultra 5 125H laptop, Ubuntu 24.04 under WSL2, GCC 13 `-O2`, real Redis 7.0.15. The laptop was plugged in with other applications closed.

There were two sessions, the same day, both measuring the server at `a80e664`:
- **Session 1** ran everything.
- **Session 2** repeated the comparisons with Redis, and took both profiles, after a flaw in the order of measurements was found (section 8). The tools were fixed in `6b1eadf`; the server code didn't change.

**The goal** (from the plan): less CPU per request than Redis at pipeline 16, at the same durability, with p99 no worse. **Result:**
- **SET with AOF on: 0.73× Redis's CPU per request** (session 2; 0.82× in session 1), and **1.21× its throughput in both sessions**, faster in every run.
- **GET, and SET without AOF: parity** (0.90–1.02×).
- **p99: lower than Redis's in all four pipelined tests, in both sessions.**

| File | Contents |
|---|---|
| [`throughput.md`](throughput.md), [`throughput-runs.csv`](throughput-runs.csv) | Against Redis, `appendfsync no`, session 2 (order alternating) |
| [`fsync-everysec/`](fsync-everysec/throughput.md), [`fsync-always/`](fsync-always/throughput.md) | Against Redis with the other fsync policies, session 2 |
| [`mini-first/`](mini-first/throughput.md) | The same three, session 1, with Mini-Redis always measured first |
| [`commits.md`](commits.md), [`commits-runs.csv`](commits-runs.csv) | Six milestones of Phase 4 compared round by round, session 1 |
| [`profile-before/`](profile-before/profile.md), [`profile/`](profile/profile.md) | Where the CPU goes, before (`v1-correct`) and after, both in session 2 |
| [`idle-load.md`](idle-load.md) | One busy client among 1,000 idle ones, `select()` against `epoll` |
| [`alloc-count.md`](alloc-count.md), [`insert-latency.md`](insert-latency.md) | Allocations per request, and the slowest insert of a new key |
| [`replay.md`](replay.md), [`replay-redis.md`](replay-redis.md), [`memory-layout.md`](memory-layout.md) | Startup time replaying the AOF, and memory per key |

## 1. Against real Redis

`appendfsync no` for both servers, the durability of every earlier result. 1,000,000 requests per test without pipelining, 5,000,000 with it; 100,000 random keys, 50 clients. Median of 3 runs; in session 2 the server measured first alternates from run to run.

| Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis | Session 1 | CPU per request vs Redis | Session 1 | After Phase 3 |
|---|---:|---:|---:|---:|---:|---:|---:|
| SET, pipeline 16, AOF on | 232,148 | 192,485 | **1.21×** | 1.21× | **0.73×** | 0.82× | 0.84× (1.30× CPU) |
| GET, pipeline 16, AOF on | 277,316 | 252,653 | 1.10× | 1.02× | 0.90× | 1.02× | 0.95× (1.14× CPU) |
| SET, pipeline 16, AOF off | 262,743 | 262,688 | 1.00× | 1.02× | 0.99× | 1.01× | 1.00× (1.03× CPU) |
| GET, pipeline 16, AOF off | 247,011 | 261,083 | 0.95× | 1.05× | 1.00× | 0.96× | 1.01× (1.05× CPU) |
| SET, no pipelining, AOF on | 22,701 | 21,044 | 1.08× | 1.07× | | | 0.92× |
| GET, no pipelining, AOF on | 23,833 | 24,217 | 0.98× | 1.05× | | | 1.02× |

- **SET with AOF on was faster than Redis in all 6 runs:**
  - session 1: 1.16, 1.15 and 1.21×;
  - session 2: 1.07, 1.19 and 1.28× (Redis was measured first in the second run).
  - CPU per request was 0.74–0.91× Redis's in every run.
  - The gain is group commit: Mini-Redis used to make one `write()` per command, and now makes one per pass of the event loop, as Redis does (section 2).
- **Everything else is a tie.** The sessions differ by up to ±0.10× with no consistent direction, which is noise between sessions.
- **p99 at pipeline 16 is lower than Redis's in all four tests, in both sessions:**

  | Test | Session 2 | Session 1 |
  |---|---|---|
  | SET, AOF on | 6.0 vs 7.0 ms | 4.5 vs 5.5 ms |
  | GET, AOF on | 5.2 vs 6.1 ms | 4.6 vs 5.1 ms |
  | SET, AOF off | 5.6 vs 5.8 ms | 4.1 vs 4.8 ms |
  | GET, AOF off | 5.1 vs 5.3 ms | 4.6 vs 4.9 ms |
- **Without pipelining, both servers still tie, and that still says nothing about the code.** Neither reaches 100% CPU: the network round trip sets the pace.
- **Raw numbers are lower in session 2** (232k against 285k for the same SET): the machine was slower. Only ratios within a session are compared.

## 2. What each change did

### Measured exactly (no noise)

| Change | Commit | Measured | Before | After |
|---|---|---|---:|---:|
| Hash table sized for `--maxkeys` | `0519778` | slowest insert of a new key ([`insert_latency`](../../insert_latency.cpp)) | 19.3–21.8 ms | 0.15–0.33 ms |
| LRU node moved with `splice`, one lookup | `6e6ee4c` | allocations, engine GET ([`alloc_count`](../../alloc_count.cpp)) | 2 | 0 |
| AOF records built in place | `bfed2a4` | allocations, engine SET with AOF | 6 | 0 |
| Group commit | `7eb0987` | `write()` calls for 100,000 pipelined SETs (`strace -c`) | 100,002 | 132 |
| Replies built in place (P3) | `f91e9a3` | allocations, GET of a 100-byte value | 3 | 0 |
| Parser fills reused strings | `af1ee97` | allocations, a whole request (GET or SET with AOF) | 3 | **0** |
| `final` | `93a6044` | call into storage (objdump) | through the vtable | direct |
| `--appendfsync always` with group commit | `148078b` | SETs per `fdatasync()`, 50 clients | 1 without group commit | 391 at pipeline 16, 25 without |
| `epoll` | `67e30c7` | server CPU per busy request, 1,000 idle clients (section 4) | 284 µs (`select()`) | 24 µs |

Every request now allocates nothing: [`alloc-count.md`](alloc-count.md) is all zeros, for GET and SET, with the AOF on, with 3-byte and 100-byte values. An insert of a new key now takes 0.17–0.22 µs at the median (0.9–1.2 µs before Phase 4) and 28–112 µs at most ([`insert-latency.md`](insert-latency.md)).

### Throughput, milestone by milestone

[`commits.md`](commits.md): pipeline 16, 5,000,000 requests per test, 5 rounds in session 1. Each cell is the median of the per-round ratios, with the lowest and highest:

| Pipeline 16 | storage | group commit | no allocations | `final` | `epoll` | All of Phase 4 |
|---|---:|---:|---:|---:|---:|---:|
| SET, AOF on | 1.08× | **1.48× (1.42–1.71)** | 1.13× | 0.96× | 0.99× | **1.55× (1.40–2.41)** |
| GET, AOF on | 1.08× | 1.03× | 1.05× | 1.02× | 0.92× | 1.14× (1.00–1.24) |
| SET, AOF off | 1.18× | 1.07× | 1.03× | 0.94× | 1.02× | **1.40× (1.08–1.52)** |
| GET, AOF off | 1.00× | 0.98× | 0.89× | 1.13× | 0.97× | 1.03× (0.75–1.55) |

- **Group commit is the change that shows:** SET with AOF on was 1.42–1.71× faster in every round.
- **Over all of Phase 4, pipelined SET got 1.55× faster with AOF on and 1.40× without, and its p99 went from 9.7 ms to 4.8 ms** (AOF on).
- **GET changed by less than this comparison can resolve.** Its cost is mostly the hash lookup and the LRU relink, which Phase 4 didn't change (section 5).
- **The small effects in this table are not resolved,** and no claim is made for them:
  - the rounds ran the milestones in an order that kept neighbours in the same order in 4 or 5 of the 5 rounds (section 8);
  - the table makes 24 comparisons, so a few apparently consistent results of 5–10% are expected by chance alone.
  - `final` measuring 4% slower on one test and `epoll` 8% slower on another fall in that range. Each one's four tests point in different directions (`final`: 0.96, 1.02, 0.94, 1.13; `epoll`: 0.99, 0.92, 1.02, 0.97).

## 3. Durability: the fsync policies, against Redis

Both servers with the same policy, AOF on; 200,000 requests without pipelining, 2,000,000 with.

| Policy | Test | Session 2 | Session 1 | CPU per request vs Redis (session 2) |
|---|---|---:|---:|---:|
| `everysec` | SET, pipeline 16 | 1.30× | 1.18× | 0.80× |
| `everysec` | SET, no pipelining | 1.03× | 1.00× | 0.90× |
| `always` | SET, pipeline 16 | 1.07× | 1.09× | 0.79× |
| `always` | SET, no pipelining | 0.98× | 1.00× | 0.85× |

- **With `always`, both servers wait for the disk, and tie:** 5,800–8,300 SETs/s without pipelining and 71,000–105,000 with it, over both sessions. Both sync once per pass of their event loop, not once per command: Mini-Redis now has the group commit Redis already had.
- **Mini-Redis uses 0.79–0.90× of Redis's CPU per request** in every row above.
- **GET results under these policies aren't claimed.** GET is not affected by the fsync policy, and in session 2 each of Redis's three GET runs with `everysec` had one stall of 2.4–2.9 s, where Mini-Redis's had none. That made Mini-Redis look 1.38× faster; the cause wasn't investigated.

## 4. `select()` against `epoll`: one busy client among 1,000 idle ones

[`idle-load.md`](idle-load.md): `redis-benchmark -c 1` (GET, no pipelining) against Mini-Redis with N idle connections open; 5 rounds, the pollers' order alternating.

| Idle connections | Poller | Busy client's requests/s | p50 | p99 | Server CPU per request |
|---:|---|---:|---:|---:|---:|
| 0 | `select()` | 3,698 | 0.215 ms | 0.607 ms | 21.2 µs |
| 0 | `epoll` | 3,500 | 0.223 ms | 0.711 ms | 22.4 µs |
| 1,000 | `select()` | 1,719 | 0.535 ms | 0.927 ms | **283.8 µs** |
| 1,000 | `epoll` | 3,219 | 0.263 ms | 0.695 ms | **24.3 µs** |

- **With 1,000 idle connections, `select()` used 9–14× the CPU per request of `epoll`** in every round (240–366 µs against 20–37 µs). It rebuilds and scans all 1,001 sockets on every wakeup.
- **The busy client's p99 is lower with `epoll` in all 5 rounds.**
- **With no idle connections, the two cost the same,** which is why the 50-client benchmarks show no difference.
- The spreads of requests/s are wide (53–71%), so the per-round CPU numbers are the reliable measure here.

## 5. Where the CPU goes now

[`profile-before/`](profile-before/profile.md) (`v1-correct`) and [`profile/`](profile/profile.md) (the tip), both taken in session 2, so their shares can be compared:

| Share of the server's CPU (inclusive) | SET, pipeline 16: before | after | GET, pipeline 16: before | after |
|---|---:|---:|---:|---:|
| All AOF logging | **39.7%** | **10.7%** | – | – |
| of which `write()` | 24.9% | 7.4% | – | – |
| `send()` | 10.6% | 14.8% | 21.8% | 20.2% |
| RESP parsing | 6.3% | 9.7% | 9.8% | 7.2% |

- **Gone from the top of the profile:** `malloc`/`free` (about 5% of SET before), the reply and record builders (`bulkString`, 2.5–3.7%) and `_M_unhook` (the old LRU erase).
- **What's left at the top is memory latency:**
  - the hash table's bucket walk: 24% of SET (`_M_find_before_node`). In GET it is now inlined into `cmdGet`, whose own time is 31%;
  - the LRU list relink (`_M_transfer`): 9–11%, because it touches the neighbouring nodes, which are cache misses with 100,000 keys.
- **Both come from node-based containers.** The next steps are one allocation per key with the LRU links inside it (plan 5.2), or an approximate LRU like Redis's, which samples keys instead of keeping a list.
- Other shares rise because the total is smaller. The server's CPU under the profiler and its kernel share are not benchmark numbers: the kernel's share of the same code measured 55% in session 1 and 31% in session 2.

## 6. Replay and memory

- **Replay of 1,000,000 commands:** 0.68 s against Redis's 0.64 s, 1.07× (1.12× in Phase 3's session). The parser no longer allocates per command.
- **Memory:** 312 bytes of heap per key (316 after Phase 3). The table sized at startup has fewer buckets than one grown by doubling.

## 7. What Phase 4 didn't change

- **GET throughput:** the same as Redis's before and after, at about 3.1–3.3 µs of CPU per request; the lookup dominates.
- **Unpipelined throughput:** both servers wait on the round trip.
- **Durability, unless asked:** the default is now `everysec`, as in Redis. Every comparison here passes the same policy to both servers.

## 8. How far to trust these numbers

- **The order of measurements was biased until session 2, and that turned out not to matter much.**
  - **The flaw:** `bench.py` always measured Mini-Redis before Redis, and `compare_commits.py` kept neighbouring milestones in the same order in 4 or 5 of 5 rounds. Anything that drifts within a round, such as a CPU's boost budget, then favours the same side. Both tools now alternate (`6b1eadf`).
  - **Its size:** session 2 repeated the Redis comparisons with the order alternating. The ratios match session 1 within ±0.10, with no consistent direction, and the headline is the same (1.21× for SET with AOF on).
  - **What isn't redone:** the milestone comparison wasn't repeated, so its small effects stay unresolved (section 2).
- **VM stalls.** Both servers had stalls of 1–3 s in some runs, in both sessions: the max latency column. They lower that run's throughput; p99 and the median of 3 runs are barely affected. One run of Mini-Redis's SET without AOF in session 2 measured 150k against its usual 260–300k because of one.
- **Rounding of run times.** With `--threads`, redis-benchmark rounds run times up to a 250 ms tick, so pipelined results fall on steps about 1.5% apart (285,453, 298,045, …). That is far below the noise.
- **Sessions differ.** The same SET measured 285k in session 1 and 232k in session 2, so only ratios within one session are compared. The kernel's share of CPU differs by session too, which is why both profiles were taken in session 2.
