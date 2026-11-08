# Open-loop latency: what a one-second stall looks like to three tools

Measured on 2026-11-08, Redis 7.0.15 and Kosh on loopback, WSL2 on Windows 11 (18 logical CPUs), mains power, heavy apps closed. Each case ran once. `kosh-loadgen` is built from the commit before the one that adds this file.

The scripts as run are in [`scripts/`](scripts/) and their output is in [`raw/`](raw/). `memtier_benchmark` (GPL-2.0, run only, not in this repository) was built at commit `ec95f3f`.

## The result

Redis was frozen for 1 second with `DEBUG SLEEP 1`, in the middle of a run. Workload for both tools: 8 connections, 10% SET, 64-byte values, 100,000 keys, target 10,000 requests/s.

| | memtier `--rate-limiting` | `kosh-loadgen` | Arithmetic |
|---|---:|---:|---:|
| Requests/s reached | 8,672 | 10,000 | |
| p50 | 0.823 ms | 0.737 ms | |
| p90 | 1.119 ms | 1.13 ms | |
| **p99** | **1.431 ms** | **713 ms** | 700 ms |
| **p99.9** | **2.007 ms** | **981 ms** | 970 ms |
| p99.99 | 10.18 ms | 1.010 s | 997 ms |
| Max | 1003.5 ms | 1.010 s | 1000 ms |

Both tools saw the stall: memtier's maximum is 1003.5 ms. The difference is how many requests were counted as waiting. memtier's source stops at one outstanding request per connection, which allows at most 8 here, and doesn't make up the sends it missed. `kosh-loadgen` kept to its schedule, so all of the roughly 10,000 requests due in that second waited.

**The arithmetic.** A request due at time *t* into a stall of *S* seconds waits *S − t*, so latencies spread evenly from 0 to *S*. In *T* measured seconds, percentile *p* is about *S − (1 − p)·T*, here with *S* = 1 and *T* = 30. `kosh-loadgen`'s p99 and p99.9 are within 2% above it, which fits the stall running about 10 ms long and the server still having to clear the backlog; those two weren't measured apart. memtier ran for 32 s with no warm-up, and reached 87% of its target.

## redis-benchmark, the closed-loop baseline

The same stall, 1,000,000 GETs over 8 connections, 13,011 requests/s:

| p50 | p95 | p99 | Max | Requests at about 1 s |
|---:|---:|---:|---:|---:|
| 0.535 ms | 0.863 ms | 1.071 ms | 1001.5 ms | 8 of 1,000,000 (one per connection) |

About 30 further requests took between 217 and 431 ms. They are not from this stall, and I have not found their cause.

## The same stall against Kosh

Kosh has no `DEBUG SLEEP`, so it was frozen with `SIGSTOP` and `SIGCONT` for 1 s, at 20,000 requests/s. Redis's run at the same rate is beside it.

| | Kosh | Redis |
|---|---:|---:|
| p50 | 790 µs | 705 µs |
| p90 | 1.53 ms | 1.26 ms |
| p99 | 713 ms | 721 ms |
| p99.9 | 981 ms | 986 ms |
| Max | 1.011 s | 1.012 s |

This compares how each behaves around the stall, nothing more: p50 and p90 from one run each are not a speed comparison between the two.

## Overload: timing from the send hides the wait

At a target of 400,000 requests/s, far above what Redis served here, requests queue in the socket and then in the generator. Timing from when the kernel accepted a request, as a tool that starts its timer at send does, misses the wait before that.

| | From the intended send | From the actual send |
|---|---:|---:|
| p50 | 1.141 s | 380 ms |
| p90 | 1.443 s | 1.141 s |
| p99 | 1.485 s | 1.325 s |
| Max | 1.520 s | 1.411 s |

The gap is 3× at p50 and 12% at p99.

## When the tool refused to give a number

A percentile is printed only if at least 10 samples lie above it, if no unanswered request could change it, and if the generator's own lateness at that percentile is at most 1/10 of the latency. With no stall, at 20,000 requests/s:

| | Latency | Generator's lateness | Printed |
|---|---:|---:|---|
| p50 | 913 µs | 0.2 µs | yes |
| p90 | 1.53 ms | | yes |
| p99 | 2.69 ms | 317 µs | N/A |
| p99.9 | 6.03 ms | 1.06 ms | N/A |
| p99.99 | 10.29 ms | 5.57 ms | N/A |

So on this virtual machine, millisecond-scale tails without a stall can't be told apart from the generator's own scheduling jitter. Tails hundreds of times larger, as in the stall above, can. Whether the same limit applies on bare metal is untested.

**Treadmill's advice was not confirmed here.** At 20,000 requests/s, 4 generator threads did worse than 1: p50 1.18 ms against 0.86 ms, and a p99 of 4.08 ms against 2.09 ms (both refused). The cause wasn't investigated.

## Limits of this result

- One machine, on loopback, in a VM. Each case ran once.
- Redis 7.0.15 only; Redis 8 and Valkey 8.1 were not measured.
- A stall is one event. The tool marks a tail that came from a single second, because such a run can't say how often the event recurs.
- The schedule is evenly spaced. Poisson arrivals were not tried. GET and SET only, with uniform keys.

## Prior work

The method is [wrk2](https://github.com/giltene/wrk2)'s, applied to RESP. [YCSB](https://github.com/brianfrankcooper/YCSB) with `measurement.interval=intended` and Dragonfly's `dfly_bench` with a negative `--qps` reached similar designs first; neither was run here. The refusal to print is from [Lancet](https://www.usenix.org/conference/atc19/presentation/kogias-lancet) (ATC 2019).
