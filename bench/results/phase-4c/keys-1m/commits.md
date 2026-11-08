# Throughput by commit

- **Date (UTC):** 2026-11-07 22:42
- **Commit:** `da7362ebca4b` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Python:** 3.12.3

- **Method:** each commit's own `bench.py --servers mini --pipelines 1,16 --aof off --runs 1 --keyspace 1000000 --requests 1000000 --pipelined-requests 5000000`. 5 rounds; each round measures every commit, in an order that rotates from round to round and is reversed every other round.
- **Raw numbers:** every run is in [`commits-runs.csv`](commits-runs.csv).

| Version | Commit |
|---|---|
| v2.5 | `b5a65a9 Merge pull request #6 from madhavthesiya/phase-4b-memory` |
| pre-siphash | `b997903 bench: record password-check timing (C3b)` |
| siphash | `8ca785d feat: hash keys with SipHash-1-3 under a random key per process (C3c)` |
| tip | `da7362e bench: compare_commits.py takes --keyspace, passed to each commit's bench.py` |

## Requests/s

Median of the rounds, with (max - min) / median in brackets.

| AOF | Pipeline | Test | v2.5 | pre-siphash | siphash | tip |
|---|---:|---|---:|---:|---:|---:|
| off | 1 | SET | 31,725 (15%) | 30,743 (15%) | 32,768 (14%) | 31,472 (5%) |
| off | 1 | GET | 31,966 (16%) | 31,231 (15%) | 30,989 (8%) | 32,225 (9%) |
| off | 16 | SET | 338,662 (7%) | 322,331 (17%) | 322,123 (8%) | 322,352 (12%) |
| off | 16 | GET | 338,845 (11%) | 338,730 (12%) | 338,708 (5%) | 344,495 (12%) |

## Paired comparison

Each cell compares two versions round by round: the median of the per-round ratios of requests/s, [lowest, highest] ratio, the rounds in which the second version was faster, the two-sided sign-test p-value, and a 95% confidence interval for the ratio (a t-interval on the logs of the per-round ratios). Treat a change as real only when the rounds agree (a small p) and the interval leaves out 1.00; the interval then says how large the change is.

| AOF | Pipeline | Test | pre-siphash / v2.5 | siphash / pre-siphash | tip / siphash | tip / v2.5 |
|---|---:|---|---|---|---|---|
| off | 1 | SET | 0.939x [0.907, 1.016] 1/5 p=0.38, CI 0.89-1.01 | 1.090x [0.954, 1.122] 4/5 p=0.38, CI 0.99-1.16 | 0.953x [0.920, 1.032] 2/5 p=1.00, CI 0.91-1.03 | 1.000x [0.929, 1.038] 3/5 p=1.00, CI 0.93-1.04 |
| off | 1 | GET | 1.007x [0.968, 1.043] 3/5 p=1.00, CI 0.96-1.05 | 1.039x [0.969, 1.078] 3/5 p=1.00, CI 0.97-1.08 | 0.992x [0.895, 1.066] 2/5 p=1.00, CI 0.91-1.08 | 1.017x [0.918, 1.169] 4/5 p=0.38, CI 0.92-1.14 |
| off | 16 | SET | 0.952x [0.849, 1.054] 2/5 p=1.00, CI 0.86-1.06 | 0.999x [0.918, 1.050] 2/5 p=1.00, CI 0.92-1.05 | 1.034x [0.969, 1.053] 4/5 p=0.38, CI 0.98-1.07 | 0.953x [0.904, 1.052] 1/5 p=0.38, CI 0.89-1.04 |
| off | 16 | GET | 0.983x [0.890, 1.089] 2/5 p=1.00, CI 0.89-1.09 | 1.034x [0.934, 1.068] 3/5 p=1.00, CI 0.94-1.08 | 0.983x [0.936, 1.071] 1/5 p=0.62, CI 0.94-1.06 | 1.000x [0.889, 1.089] 3/5 p=1.00, CI 0.90-1.08 |

## Server CPU per request

Median of the rounds, in µs: server CPU time divided by requests. This is the measure to compare when the server is the bottleneck (CPU near 100%, next table).

| AOF | Pipeline | Test | v2.5 | pre-siphash | siphash | tip |
|---|---:|---|---:|---:|---:|---:|
| off | 1 | SET | 22.62 | 22.38 | 21.01 | 23.03 |
| off | 1 | GET | 21.88 | 20.68 | 21.59 | 21.85 |
| off | 16 | SET | 2.67 | 2.90 | 2.89 | 2.87 |
| off | 16 | GET | 2.72 | 2.65 | 2.68 | 2.71 |

## Server CPU

Median of the rounds; 100% = one core busy.

| AOF | Pipeline | Test | v2.5 | pre-siphash | siphash | tip |
|---|---:|---|---:|---:|---:|---:|
| off | 1 | SET | 72% | 69% | 70% | 72% |
| off | 1 | GET | 71% | 66% | 67% | 70% |
| off | 16 | SET | 91% | 92% | 92% | 92% |
| off | 16 | GET | 90% | 90% | 92% | 93% |

## p99 latency

Median of the rounds, in ms.

| AOF | Pipeline | Test | v2.5 | pre-siphash | siphash | tip |
|---|---:|---|---:|---:|---:|---:|
| off | 1 | SET | 3.2 | 3.4 | 3.3 | 3.3 |
| off | 1 | GET | 3.3 | 3.4 | 3.3 | 3.2 |
| off | 16 | SET | 4.4 | 4.6 | 4.9 | 4.7 |
| off | 16 | GET | 4.3 | 4.3 | 4.2 | 4.3 |
