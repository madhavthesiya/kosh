# Throughput and latency

- **Date (UTC):** 2026-11-03 20:57
- **Commit:** `cbae3d9b1315` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Compiler:** c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- **Build:** CMake `RelWithDebInfo` (`-O2 -g -DNDEBUG`)
- **redis-server:** Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d064f9f48c901a92
- **redis-benchmark:** redis-benchmark 7.0.15
- **taskset:** taskset from util-linux 2.39.3
- **Python:** 3.12.3

- **Load:** `-n 1,000,000` requests per test without pipelining and `-n 5,000,000` with it (redis-benchmark `--threads` rounds each run time up to its 250 ms timer, so fast runs need to be long), `-r 100,000` random keys, `-c 50` clients, 3-byte values; SET then GET on the same fresh server.
- **Pinning:** server on CPU 2, redis-benchmark on CPU 4,6 (2 threads).
- **Runs:** 3 per row, interleaved, with the server measured first alternating from run to run. The row shows the run with the median throughput; *spread* is (max - min) / median over all runs.
- **Server CPU:** 100% = one core fully busy. *In kernel* is the share of that CPU time spent in system calls (`stime` / (`utime` + `stime`)).
- **Client CPU:** redis-benchmark's CPU use (200% = all 2 threads busy). If the client is near its limit while the server is not, the row measures the client.

Commands (port and directory differ per run):

```
taskset -c 2 build-bench/mini_redis_server --port <port> --maxkeys 200000 --appendonly <yes|no> --appendfilename <tmp>/appendonly.aof --appendfsync no
taskset -c 2 redis-server --port <port> --bind 127.0.0.1 --dir <tmp> --save "" --appendonly <yes|no> --appendfsync no --auto-aof-rewrite-percentage 0 --logfile <tmp>/redis.log
taskset -c 4,6 redis-benchmark -h 127.0.0.1 -p <port> -t <set|get> -n <requests> -r 100000 -c 50 -P <pipeline> --csv --threads 2
```

| Server | AOF | Pipeline | Test | Requests/s | Spread | p50 ms | p99 ms | max ms | Server CPU | In kernel | Client CPU |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Mini-Redis | on | 16 | SET | 350,435 | 14.8% | 2.0 | 4.1 | 8.9 | 90% | 35% | 105% |
| Redis | on | 16 | SET | 254,621 | 6.1% | 2.9 | 5.0 | 609.8 | 97% | 19% | 62% |
| Mini-Redis | on | 16 | GET | 407,664 | 7.6% | 1.7 | 3.7 | 6.9 | 84% | 39% | 122% |
| Redis | on | 16 | GET | 332,934 | 11.1% | 2.1 | 4.4 | 11.4 | 92% | 25% | 100% |
| Mini-Redis | off | 16 | SET | 399,584 | 26.1% | 1.7 | 3.7 | 7.8 | 85% | 36% | 121% |
| Redis | off | 16 | SET | 312,012 | 6.1% | 2.3 | 4.8 | 13.4 | 93% | 24% | 96% |
| Mini-Redis | off | 16 | GET | 391,665 | 10.4% | 1.7 | 3.7 | 13.1 | 83% | 39% | 124% |
| Redis | off | 16 | GET | 338,547 | 1.7% | 2.0 | 4.3 | 9.1 | 93% | 26% | 104% |

## Mini-Redis relative to Redis

| AOF | Pipeline | Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis |
|---|---:|---|---:|---:|---:|
| on | 16 | SET | 350,435 | 254,621 | 1.38x |
| on | 16 | GET | 407,664 | 332,934 | 1.22x |
| off | 16 | SET | 399,584 | 312,012 | 1.28x |
| off | 16 | GET | 391,665 | 338,547 | 1.16x |
