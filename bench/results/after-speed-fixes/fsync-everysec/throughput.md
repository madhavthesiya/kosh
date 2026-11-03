# Throughput and latency

- **Date (UTC):** 2026-11-03 14:52
- **Commit:** `6b1eadf6e589` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Compiler:** c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- **Build:** CMake `RelWithDebInfo` (`-O2 -g -DNDEBUG`)
- **redis-server:** Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d064f9f48c901a92
- **redis-benchmark:** redis-benchmark 7.0.15
- **taskset:** taskset from util-linux 2.39.3
- **Python:** 3.12.3

- **Load:** `-n 200,000` requests per test without pipelining and `-n 2,000,000` with it (redis-benchmark `--threads` rounds each run time up to its 250 ms timer, so fast runs need to be long), `-r 100,000` random keys, `-c 50` clients, 3-byte values; SET then GET on the same fresh server.
- **Pinning:** server on CPU 2, redis-benchmark on CPU 4,6 (2 threads).
- **Runs:** 3 per row, interleaved, with the server measured first alternating from run to run. The row shows the run with the median throughput; *spread* is (max - min) / median over all runs.
- **Server CPU:** 100% = one core fully busy. *In kernel* is the share of that CPU time spent in system calls (`stime` / (`utime` + `stime`)).
- **Client CPU:** redis-benchmark's CPU use (200% = all 2 threads busy). If the client is near its limit while the server is not, the row measures the client.

Commands (port and directory differ per run):

```
taskset -c 2 build-bench/mini_redis_server --port <port> --maxkeys 200000 --appendonly <yes|no> --appendfilename <tmp>/appendonly.aof --appendfsync everysec
taskset -c 2 redis-server --port <port> --bind 127.0.0.1 --dir <tmp> --save "" --appendonly <yes|no> --appendfsync everysec --auto-aof-rewrite-percentage 0 --logfile <tmp>/redis.log
taskset -c 4,6 redis-benchmark -h 127.0.0.1 -p <port> -t <set|get> -n <requests> -r 100000 -c 50 -P <pipeline> --csv --threads 2
```

| Server | AOF | Pipeline | Test | Requests/s | Spread | p50 ms | p99 ms | max ms | Server CPU | In kernel | Client CPU |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Mini-Redis | on | 1 | SET | 21,596 | 39.1% | 1.9 | 4.3 | 7.7 | 79% | 72% | 130% |
| Redis | on | 1 | SET | 21,026 | 30.8% | 2.0 | 4.4 | 11.5 | 86% | 59% | 120% |
| Mini-Redis | on | 1 | GET | 22,828 | 22.1% | 1.8 | 4.0 | 8.5 | 74% | 77% | 142% |
| Redis | on | 1 | GET | 22,826 | 25.5% | 1.8 | 4.0 | 7.9 | 81% | 67% | 133% |
| Mini-Redis | on | 16 | SET | 242,014 | 21.9% | 2.9 | 6.0 | 10.2 | 94% | 30% | 91% |
| Redis | on | 16 | SET | 185,701 | 13.4% | 3.9 | 7.9 | 11.3 | 90% | 20% | 66% |
| Mini-Redis | on | 16 | GET | 257,699 | 3.3% | 2.5 | 5.6 | 9.1 | 89% | 30% | 118% |
| Redis | on | 16 | GET | 186,445 | 23.1% | 2.8 | 5.9 | 2467.8 | 92% | 28% | 100% |

## Mini-Redis relative to Redis

| AOF | Pipeline | Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis |
|---|---:|---|---:|---:|---:|
| on | 1 | SET | 21,596 | 21,026 | 1.03x |
| on | 1 | GET | 22,828 | 22,826 | 1.00x |
| on | 16 | SET | 242,014 | 185,701 | 1.30x |
| on | 16 | GET | 257,699 | 186,445 | 1.38x |
