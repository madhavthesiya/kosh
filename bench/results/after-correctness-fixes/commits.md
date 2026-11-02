# Throughput by commit

- **Date (UTC):** 2026-11-02 19:59
- **Commit:** `9f39a7180251` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Python:** 3.12.3

- **Method:** each commit's own `bench.py --servers mini --pipelines 16 --aof on,off --runs 1 --requests 5000000`, commits measured in turn, 3 rounds. Requests/s is the median of the rounds, with (max - min) / median in brackets.

| Version | Commit |
|---|---|
| after-network-fixes | `ac666b2 docs: describe the Phase 2 network changes and their results` |
| after-correctness-fixes | `9f39a71 fix(aof): keep and retry failed writes; refuse writes meanwhile (N5)` |

| AOF | Pipeline | Test | after-network-fixes | after-correctness-fixes | after-correctness-fixes / after-network-fixes |
|---|---:|---|---:|---:|---:|
| on | 16 | SET | 144,676 (21%) | 176,685 (35%) | 1.22x |
| on | 16 | GET | 229,600 (10%) | 273,733 (15%) | 1.19x |
| off | 16 | SET | 201,629 (27%) | 214,417 (7%) | 1.06x |
| off | 16 | GET | 232,320 (29%) | 224,165 (11%) | 0.96x |

Median server CPU per row (100% = one core busy):

| AOF | Pipeline | Test | after-network-fixes | after-correctness-fixes |
|---|---:|---|---:|---:|
| on | 16 | SET | 99% | 100% |
| on | 16 | GET | 99% | 100% |
| off | 16 | SET | 99% | 99% |
| off | 16 | GET | 99% | 99% |
