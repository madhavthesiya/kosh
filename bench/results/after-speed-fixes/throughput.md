# Throughput and latency

- **Date (UTC):** 2026-11-03 14:26
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
| Mini-Redis | on | 1 | SET | 22,701 | 7.1% | 1.8 | 4.0 | 10.0 | 83% | 72% | 143% |
| Redis | on | 1 | SET | 21,044 | 11.9% | 1.9 | 4.2 | 2463.7 | 86% | 60% | 124% |
| Mini-Redis | on | 1 | GET | 23,833 | 19.4% | 1.6 | 3.5 | 2492.4 | 82% | 74% | 157% |
| Redis | on | 1 | GET | 24,217 | 8.4% | 1.7 | 3.9 | 7.8 | 86% | 67% | 141% |
| Mini-Redis | on | 16 | SET | 232,148 | 7.4% | 3.1 | 6.0 | 9.4 | 92% | 30% | 86% |
| Redis | on | 16 | SET | 192,485 | 13.1% | 3.6 | 7.0 | 1434.6 | 104% | 20% | 71% |
| Mini-Redis | on | 16 | GET | 277,316 | 7.7% | 2.4 | 5.2 | 7.7 | 91% | 30% | 118% |
| Redis | on | 16 | GET | 252,653 | 15.8% | 2.7 | 6.1 | 10.6 | 91% | 27% | 98% |
| Mini-Redis | off | 1 | SET | 24,513 | 6.8% | 1.6 | 3.7 | 70.5 | 78% | 76% | 143% |
| Redis | off | 1 | SET | 22,318 | 4.4% | 1.9 | 4.1 | 8.5 | 83% | 64% | 132% |
| Mini-Redis | off | 1 | GET | 23,963 | 10.8% | 1.6 | 3.6 | 2424.8 | 77% | 76% | 144% |
| Redis | off | 1 | GET | 22,851 | 13.0% | 1.7 | 3.9 | 2471.9 | 90% | 66% | 145% |
| Mini-Redis | off | 16 | SET | 262,743 | 56.3% | 2.5 | 5.6 | 9.2 | 93% | 30% | 114% |
| Redis | off | 16 | SET | 262,688 | 17.2% | 2.6 | 5.8 | 8.9 | 94% | 24% | 96% |
| Mini-Redis | off | 16 | GET | 247,011 | 17.6% | 2.3 | 5.1 | 2715.6 | 97% | 31% | 128% |
| Redis | off | 16 | GET | 261,083 | 8.9% | 2.4 | 5.3 | 1380.4 | 102% | 27% | 113% |

## Mini-Redis relative to Redis

| AOF | Pipeline | Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis |
|---|---:|---|---:|---:|---:|
| on | 1 | SET | 22,701 | 21,044 | 1.08x |
| on | 1 | GET | 23,833 | 24,217 | 0.98x |
| on | 16 | SET | 232,148 | 192,485 | 1.21x |
| on | 16 | GET | 277,316 | 252,653 | 1.10x |
| off | 1 | SET | 24,513 | 22,318 | 1.10x |
| off | 1 | GET | 23,963 | 22,851 | 1.05x |
| off | 16 | SET | 262,743 | 262,688 | 1.00x |
| off | 16 | GET | 247,011 | 261,083 | 0.95x |
