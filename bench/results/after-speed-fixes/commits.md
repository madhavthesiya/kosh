# Throughput by commit

- **Date (UTC):** 2026-11-03 13:38
- **Commit:** `a80e66437776` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Python:** 3.12.3

- **Method:** each commit's own `bench.py --servers mini --pipelines 16 --aof on,off --runs 1 --requests 1000000 --pipelined-requests 5000000`. 5 rounds; each round measures every commit, in an order that rotates from round to round.
- **Raw numbers:** every run is in [`commits-runs.csv`](commits-runs.csv).

| Version | Commit |
|---|---|
| v1-correct | `5e9722c Merge pull request #4 from madhavthesiya/phase-3-persistence` |
| storage | `6e6ee4c perf(storage): move LRU nodes with splice, and look each key up once` |
| group-commit | `7eb0987 perf(server): group commit: one AOF write per pass of the event loop` |
| no-allocations | `af1ee97 perf(parser): parse into reused argument strings: no allocation per request` |
| final | `93a6044 perf(storage): mark InMemoryStorage final (change 4): direct calls, no measurable gain` |
| epoll | `67e30c7 feat(server): epoll on Linux (change 8), maxclients, --poller` |

## Requests/s

Median of the rounds, with (max - min) / median in brackets.

| AOF | Pipeline | Test | v1-correct | storage | group-commit | no-allocations | final | epoll |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| on | 16 | SET | 170,783 (34%) | 171,703 (27%) | 259,296 (16%) | 307,182 (30%) | 285,453 (32%) | 258,211 (29%) |
| on | 16 | GET | 251,927 (31%) | 279,705 (25%) | 285,421 (38%) | 298,205 (21%) | 338,593 (21%) | 307,333 (18%) |
| off | 16 | SET | 235,927 (50%) | 255,611 (17%) | 285,372 (10%) | 298,045 (20%) | 293,720 (16%) | 327,354 (23%) |
| off | 16 | GET | 285,453 (30%) | 302,773 (15%) | 298,027 (17%) | 292,415 (31%) | 316,997 (25%) | 312,110 (42%) |

## Paired comparison

Each cell compares two versions round by round: the median of the per-round ratios of requests/s, [lowest, highest] ratio, the rounds in which the second version was faster, and the two-sided sign-test p-value. Treat a change as real only when the rounds agree (a small p).

| AOF | Pipeline | Test | storage / v1-correct | group-commit / storage | no-allocations / group-commit | final / no-allocations | epoll / final | epoll / v1-correct |
|---|---:|---|---|---|---|---|---|---|
| on | 16 | SET | 1.081x [1.000, 1.144] 5/5 p=0.06 | 1.484x [1.420, 1.711] 5/5 p=0.06 | 1.129x [0.890, 1.231] 4/5 p=0.38 | 0.956x [0.915, 0.969] 0/5 p=0.06 | 0.987x [0.901, 1.061] 2/5 p=1.00 | 1.545x [1.395, 2.408] 5/5 p=0.06 |
| on | 16 | GET | 1.084x [1.014, 1.218] 5/5 p=0.06 | 1.034x [0.894, 1.050] 3/5 p=1.00 | 1.045x [0.813, 1.421] 4/5 p=0.38 | 1.017x [0.957, 1.297] 4/5 p=0.38 | 0.921x [0.840, 0.936] 0/5 p=0.06 | 1.144x [1.000, 1.239] 4/5 p=0.38 |
| off | 16 | SET | 1.176x [0.844, 1.386] 3/5 p=1.00 | 1.074x [1.029, 1.185] 5/5 p=0.06 | 1.029x [0.972, 1.283] 3/5 p=0.62 | 0.936x [0.927, 1.077] 1/5 p=0.62 | 1.016x [0.883, 1.205] 4/5 p=0.38 | 1.397x [1.081, 1.516] 5/5 p=0.06 |
| off | 16 | GET | 1.000x [0.939, 1.407] 3/5 p=1.00 | 0.984x [0.916, 1.082] 2/5 p=1.00 | 0.893x [0.832, 1.081] 2/5 p=1.00 | 1.128x [0.957, 1.375] 4/5 p=0.38 | 0.969x [0.748, 1.087] 2/5 p=1.00 | 1.031x [0.748, 1.552] 3/5 p=1.00 |

## Server CPU per request

Median of the rounds, in µs: server CPU time divided by requests. This is the measure to compare when the server is the bottleneck (CPU near 100%, next table).

| AOF | Pipeline | Test | v1-correct | storage | group-commit | no-allocations | final | epoll |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| on | 16 | SET | 5.66 | 6.11 | 3.52 | 3.15 | 3.50 | 3.88 |
| on | 16 | GET | 3.96 | 3.64 | 3.63 | 3.18 | 2.97 | 3.32 |
| off | 16 | SET | 4.32 | 3.86 | 3.26 | 3.16 | 3.48 | 3.05 |
| off | 16 | GET | 3.57 | 3.43 | 3.15 | 3.45 | 2.93 | 3.10 |

## Server CPU

Median of the rounds; 100% = one core busy.

| AOF | Pipeline | Test | v1-correct | storage | group-commit | no-allocations | final | epoll |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| on | 16 | SET | 99% | 100% | 92% | 95% | 98% | 95% |
| on | 16 | GET | 100% | 99% | 96% | 95% | 100% | 96% |
| off | 16 | SET | 107% | 100% | 94% | 94% | 96% | 97% |
| off | 16 | GET | 105% | 107% | 94% | 95% | 96% | 97% |

## p99 latency

Median of the rounds, in ms.

| AOF | Pipeline | Test | v1-correct | storage | group-commit | no-allocations | final | epoll |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| on | 16 | SET | 9.7 | 9.9 | 5.0 | 4.5 | 4.8 | 4.8 |
| on | 16 | GET | 5.6 | 5.3 | 4.9 | 4.4 | 4.2 | 4.4 |
| off | 16 | SET | 6.3 | 5.8 | 4.7 | 4.7 | 4.5 | 4.3 |
| off | 16 | GET | 5.4 | 5.1 | 4.5 | 4.7 | 4.5 | 4.6 |
