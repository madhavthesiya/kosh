# Throughput by commit

- **Date (UTC):** 2026-11-03 20:57
- **Commit:** `cbae3d9b1315` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Python:** 3.12.3

- **Method:** each commit's own `bench.py --servers mini --pipelines 16 --aof on,off --runs 1 --requests 1000000 --pipelined-requests 5000000`. 6 rounds; each round measures every commit, in an order that rotates from round to round and is reversed every other round.
- **Raw numbers:** every run is in [`commits-runs.csv`](commits-runs.csv).

| Version | Commit |
|---|---|
| v2-fast | `0c17b5a Merge pull request #5 from madhavthesiya/phase-4-speed` |
| sieve-default | `ecf73d5 feat(server): SIEVE is the default eviction policy` |
| keytable | `cbae3d9 perf(storage): one allocation per key, in KeyTable (B3, part 2)` |

## Requests/s

Median of the rounds, with (max - min) / median in brackets.

| AOF | Pipeline | Test | v2-fast | sieve-default | keytable |
|---|---:|---|---:|---:|---:|
| on | 16 | SET | 324,781 (71%) | 335,828 (29%) | 338,626 (22%) |
| on | 16 | GET | 330,175 (128%) | 330,089 (17%) | 360,356 (32%) |
| off | 16 | SET | 338,547 (57%) | 330,132 (25%) | 350,398 (32%) |
| off | 16 | GET | 335,718 (59%) | 341,474 (23%) | 381,290 (23%) |

## Paired comparison

Each cell compares two versions round by round: the median of the per-round ratios of requests/s, [lowest, highest] ratio, the rounds in which the second version was faster, the two-sided sign-test p-value, and a 95% confidence interval for the ratio (a t-interval on the logs of the per-round ratios). Treat a change as real only when the rounds agree (a small p) and the interval leaves out 1.00; the interval then says how large the change is.

| AOF | Pipeline | Test | sieve-default / v2-fast | keytable / sieve-default | keytable / v2-fast |
|---|---:|---|---|---|---|
| on | 16 | SET | 0.977x [0.663, 1.125] 3/6 p=1.00, CI 0.73-1.14 | 1.032x [0.950, 1.198] 4/6 p=0.69, CI 0.95-1.15 | 1.008x [0.630, 1.236] 3/6 p=1.00, CI 0.75-1.22 |
| on | 16 | GET | 0.989x [0.459, 1.107] 3/6 p=1.00, CI 0.63-1.25 | 1.065x [0.932, 1.271] 5/6 p=0.22, CI 0.96-1.23 | 0.999x [0.583, 1.245] 3/6 p=1.00, CI 0.73-1.27 |
| off | 16 | SET | 1.042x [0.635, 1.180] 3/6 p=1.00, CI 0.77-1.23 | 1.061x [0.980, 1.178] 5/6 p=0.22, CI 1.00-1.14 | 1.105x [0.666, 1.311] 5/6 p=0.22, CI 0.81-1.33 |
| off | 16 | GET | 1.056x [0.835, 1.193] 3/6 p=1.00, CI 0.86-1.18 | 1.060x [1.017, 1.359] 6/6 p=0.03, CI 0.98-1.24 | 1.081x [0.873, 1.621] 4/6 p=0.69, CI 0.89-1.40 |

## Server CPU per request

Median of the rounds, in µs: server CPU time divided by requests. This is the measure to compare when the server is the bottleneck (CPU near 100%, next table).

| AOF | Pipeline | Test | v2-fast | sieve-default | keytable |
|---|---:|---|---:|---:|---:|
| on | 16 | SET | 3.06 | 2.90 | 2.76 |
| on | 16 | GET | 2.69 | 2.68 | 2.47 |
| off | 16 | SET | 2.80 | 2.73 | 2.51 |
| off | 16 | GET | 2.83 | 2.65 | 2.38 |

## Server CPU

Median of the rounds; 100% = one core busy.

| AOF | Pipeline | Test | v2-fast | sieve-default | keytable |
|---|---:|---|---:|---:|---:|
| on | 16 | SET | 99% | 96% | 93% |
| on | 16 | GET | 92% | 88% | 88% |
| off | 16 | SET | 92% | 90% | 89% |
| off | 16 | GET | 92% | 91% | 90% |

## p99 latency

Median of the rounds, in ms.

| AOF | Pipeline | Test | v2-fast | sieve-default | keytable |
|---|---:|---|---:|---:|---:|
| on | 16 | SET | 4.4 | 4.6 | 4.4 |
| on | 16 | GET | 4.1 | 4.4 | 3.8 |
| off | 16 | SET | 4.5 | 4.2 | 3.9 |
| off | 16 | GET | 4.6 | 3.9 | 3.6 |
