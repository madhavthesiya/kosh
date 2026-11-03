# Throughput and latency

- **Date (UTC):** 2026-11-03 13:38
- **Commit:** `a80e66437776` (clean)
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
- **Runs:** 3 per row, interleaved. The row shows the run with the median throughput; *spread* is (max - min) / median over all runs.
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
| Mini-Redis | on | 1 | SET | 29,820 | 5.0% | 1.5 | 3.3 | 30.7 | 74% | 79% | 108% |
| Redis | on | 1 | SET | 27,753 | 15.6% | 1.6 | 3.5 | 15.2 | 81% | 71% | 102% |
| Mini-Redis | on | 1 | GET | 31,469 | 5.5% | 1.4 | 3.2 | 7.4 | 68% | 79% | 106% |
| Redis | on | 1 | GET | 30,049 | 3.2% | 1.4 | 3.4 | 8.7 | 76% | 77% | 108% |
| Mini-Redis | on | 16 | SET | 285,242 | 11.2% | 2.6 | 4.5 | 13.5 | 89% | 44% | 62% |
| Redis | on | 16 | SET | 234,874 | 10.1% | 3.3 | 5.5 | 13.9 | 89% | 34% | 51% |
| Mini-Redis | on | 16 | GET | 302,663 | 11.3% | 2.4 | 4.6 | 7.9 | 96% | 49% | 91% |
| Redis | on | 16 | GET | 298,063 | 20.0% | 2.4 | 5.1 | 10.3 | 93% | 41% | 74% |
| Mini-Redis | off | 1 | SET | 30,341 | 12.6% | 1.3 | 3.1 | 2439.2 | 70% | 80% | 108% |
| Redis | off | 1 | SET | 31,841 | 15.7% | 1.3 | 3.0 | 2388.0 | 78% | 75% | 107% |
| Mini-Redis | off | 1 | GET | 32,230 | 17.7% | 1.4 | 3.1 | 22.1 | 70% | 79% | 108% |
| Redis | off | 1 | GET | 29,388 | 30.4% | 1.5 | 3.4 | 7.2 | 76% | 77% | 106% |
| Mini-Redis | off | 16 | SET | 295,508 | 11.4% | 2.2 | 4.1 | 1903.6 | 101% | 46% | 90% |
| Redis | off | 16 | SET | 289,436 | 14.6% | 2.6 | 4.8 | 8.0 | 98% | 44% | 72% |
| Mini-Redis | off | 16 | GET | 302,645 | 19.3% | 2.4 | 4.6 | 8.0 | 94% | 48% | 93% |
| Redis | off | 16 | GET | 289,452 | 12.0% | 2.5 | 4.9 | 9.6 | 94% | 45% | 81% |

## Mini-Redis relative to Redis

| AOF | Pipeline | Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis |
|---|---:|---|---:|---:|---:|
| on | 1 | SET | 29,820 | 27,753 | 1.07x |
| on | 1 | GET | 31,469 | 30,049 | 1.05x |
| on | 16 | SET | 285,242 | 234,874 | 1.21x |
| on | 16 | GET | 302,663 | 298,063 | 1.02x |
| off | 1 | SET | 30,341 | 31,841 | 0.95x |
| off | 1 | GET | 32,230 | 29,388 | 1.10x |
| off | 16 | SET | 295,508 | 289,436 | 1.02x |
| off | 16 | GET | 302,645 | 289,452 | 1.05x |
