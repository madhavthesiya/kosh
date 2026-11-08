# Throughput by commit

- **Date (UTC):** 2026-11-07 22:09
- **Commit:** `da7362ebca4b` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Python:** 3.12.3

- **Method:** each commit's own `bench.py --servers mini --pipelines 1,16 --aof on,off --runs 1 --keyspace 100000 --requests 1000000 --pipelined-requests 5000000`. 5 rounds; each round measures every commit, in an order that rotates from round to round and is reversed every other round.
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
| on | 1 | SET | 30,515 (52%) | 33,310 (5%) | 31,001 (8%) | 32,233 (11%) |
| on | 1 | GET | 28,338 (12%) | 30,755 (10%) | 29,822 (15%) | 30,520 (11%) |
| on | 16 | SET | 298,080 (34%) | 338,570 (7%) | 312,188 (20%) | 316,817 (13%) |
| on | 16 | GET | 350,680 (23%) | 356,964 (13%) | 377,045 (19%) | 369,905 (20%) |
| off | 1 | SET | 30,287 (11%) | 31,724 (9%) | 31,406 (5%) | 31,011 (35%) |
| off | 1 | GET | 30,513 (14%) | 31,235 (10%) | 30,741 (17%) | 31,466 (27%) |
| off | 16 | SET | 344,448 (25%) | 377,045 (18%) | 356,710 (16%) | 363,161 (38%) |
| off | 16 | GET | 384,025 (13%) | 370,151 (9%) | 356,405 (4%) | 376,903 (38%) |

## Paired comparison

Each cell compares two versions round by round: the median of the per-round ratios of requests/s, [lowest, highest] ratio, the rounds in which the second version was faster, the two-sided sign-test p-value, and a 95% confidence interval for the ratio (a t-interval on the logs of the per-round ratios). Treat a change as real only when the rounds agree (a small p) and the interval leaves out 1.00; the interval then says how large the change is.

| AOF | Pipeline | Test | pre-siphash / v2.5 | siphash / pre-siphash | tip / siphash | tip / v2.5 |
|---|---:|---|---|---|---|---|
| on | 1 | SET | 1.049x [1.017, 1.986] 5/5 p=0.06, CI 0.87-1.77 | 0.930x [0.875, 0.992] 0/5 p=0.06, CI 0.88-0.99 | 1.016x [0.948, 1.057] 3/5 p=1.00, CI 0.96-1.07 | 1.057x [0.962, 1.938] 3/5 p=1.00, CI 0.82-1.69 |
| on | 1 | GET | 1.041x [1.008, 1.137] 5/5 p=0.06, CI 0.99-1.13 | 0.974x [0.926, 1.074] 1/5 p=0.38, CI 0.91-1.05 | 1.038x [0.946, 1.075] 4/5 p=0.38, CI 0.97-1.09 | 1.068x [0.992, 1.109] 4/5 p=0.38, CI 1.01-1.12 |
| on | 16 | SET | 1.136x [1.032, 1.611] 5/5 p=0.06, CI 0.97-1.52 | 0.969x [0.853, 1.052] 2/5 p=1.00, CI 0.84-1.07 | 0.985x [0.967, 1.095] 2/5 p=1.00, CI 0.95-1.08 | 1.103x [0.985, 1.585] 4/5 p=0.38, CI 0.92-1.46 |
| on | 16 | GET | 1.018x [0.893, 1.135] 4/5 p=0.38, CI 0.92-1.14 | 1.074x [0.838, 1.078] 4/5 p=0.38, CI 0.89-1.17 | 1.035x [0.932, 1.041] 3/5 p=1.00, CI 0.94-1.07 | 1.067x [0.926, 1.142] 3/5 p=1.00, CI 0.94-1.16 |
| off | 1 | SET | 1.064x [0.976, 1.099] 4/5 p=0.38, CI 0.99-1.11 | 0.992x [0.931, 1.030] 1/5 p=0.38, CI 0.94-1.03 | 0.987x [0.742, 1.050] 2/5 p=1.00, CI 0.79-1.13 | 1.024x [0.719, 1.118] 3/5 p=1.00, CI 0.78-1.23 |
| off | 1 | GET | 1.022x [0.938, 1.132] 3/5 p=1.00, CI 0.93-1.12 | 0.964x [0.833, 1.032] 1/5 p=0.38, CI 0.86-1.05 | 1.000x [0.809, 1.182] 2/5 p=1.00, CI 0.84-1.17 | 1.008x [0.797, 1.070] 3/5 p=1.00, CI 0.84-1.11 |
| off | 16 | SET | 1.095x [1.020, 1.142] 5/5 p=0.06, CI 1.03-1.14 | 1.035x [0.833, 1.076] 3/5 p=1.00, CI 0.86-1.11 | 0.966x [0.733, 1.177] 1/5 p=0.38, CI 0.77-1.17 | 1.000x [0.809, 1.205] 2/5 p=1.00, CI 0.84-1.20 |
| off | 16 | GET | 0.964x [0.891, 1.057] 1/5 p=0.38, CI 0.88-1.04 | 0.947x [0.930, 1.035] 2/5 p=1.00, CI 0.92-1.03 | 1.075x [0.719, 1.121] 3/5 p=1.00, CI 0.78-1.24 | 0.999x [0.667, 1.077] 2/5 p=1.00, CI 0.72-1.16 |

## Server CPU per request

Median of the rounds, in µs: server CPU time divided by requests. This is the measure to compare when the server is the bottleneck (CPU near 100%, next table).

| AOF | Pipeline | Test | v2.5 | pre-siphash | siphash | tip |
|---|---:|---|---:|---:|---:|---:|
| on | 1 | SET | 24.38 | 21.70 | 23.87 | 23.48 |
| on | 1 | GET | 25.62 | 23.09 | 23.94 | 23.73 |
| on | 16 | SET | 3.02 | 2.78 | 3.02 | 2.96 |
| on | 16 | GET | 2.57 | 2.43 | 2.41 | 2.31 |
| off | 1 | SET | 24.12 | 21.79 | 21.81 | 22.12 |
| off | 1 | GET | 23.04 | 22.57 | 22.89 | 22.62 |
| off | 16 | SET | 2.63 | 2.35 | 2.53 | 2.48 |
| off | 16 | GET | 2.32 | 2.39 | 2.49 | 2.31 |

## Server CPU

Median of the rounds; 100% = one core busy.

| AOF | Pipeline | Test | v2.5 | pre-siphash | siphash | tip |
|---|---:|---|---:|---:|---:|---:|
| on | 1 | SET | 74% | 73% | 74% | 73% |
| on | 1 | GET | 72% | 70% | 73% | 72% |
| on | 16 | SET | 94% | 93% | 94% | 93% |
| on | 16 | GET | 89% | 88% | 89% | 89% |
| off | 1 | SET | 72% | 68% | 69% | 70% |
| off | 1 | GET | 68% | 70% | 70% | 71% |
| off | 16 | SET | 91% | 89% | 89% | 90% |
| off | 16 | GET | 88% | 89% | 87% | 89% |

## p99 latency

Median of the rounds, in ms.

| AOF | Pipeline | Test | v2.5 | pre-siphash | siphash | tip |
|---|---:|---|---:|---:|---:|---:|
| on | 1 | SET | 3.4 | 3.2 | 3.4 | 3.3 |
| on | 1 | GET | 3.6 | 3.4 | 3.4 | 3.3 |
| on | 16 | SET | 5.2 | 4.5 | 4.6 | 4.7 |
| on | 16 | GET | 4.5 | 4.2 | 4.0 | 3.9 |
| off | 1 | SET | 3.5 | 3.3 | 3.3 | 3.3 |
| off | 1 | GET | 3.4 | 3.3 | 3.3 | 3.2 |
| off | 16 | SET | 4.5 | 4.1 | 4.3 | 4.1 |
| off | 16 | GET | 3.8 | 4.2 | 4.1 | 4.0 |
