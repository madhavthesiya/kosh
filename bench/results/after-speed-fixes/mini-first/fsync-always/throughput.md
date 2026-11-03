# Throughput and latency

- **Date (UTC):** 2026-11-03 14:01
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

- **Load:** `-n 200,000` requests per test without pipelining and `-n 2,000,000` with it (redis-benchmark `--threads` rounds each run time up to its 250 ms timer, so fast runs need to be long), `-r 100,000` random keys, `-c 50` clients, 3-byte values; SET then GET on the same fresh server.
- **Pinning:** server on CPU 2, redis-benchmark on CPU 4,6 (2 threads).
- **Runs:** 3 per row, interleaved. The row shows the run with the median throughput; *spread* is (max - min) / median over all runs.
- **Server CPU:** 100% = one core fully busy. *In kernel* is the share of that CPU time spent in system calls (`stime` / (`utime` + `stime`)).
- **Client CPU:** redis-benchmark's CPU use (200% = all 2 threads busy). If the client is near its limit while the server is not, the row measures the client.

Commands (port and directory differ per run):

```
taskset -c 2 build-bench/mini_redis_server --port <port> --maxkeys 200000 --appendonly <yes|no> --appendfilename <tmp>/appendonly.aof --appendfsync always
taskset -c 2 redis-server --port <port> --bind 127.0.0.1 --dir <tmp> --save "" --appendonly <yes|no> --appendfsync always --auto-aof-rewrite-percentage 0 --logfile <tmp>/redis.log
taskset -c 4,6 redis-benchmark -h 127.0.0.1 -p <port> -t <set|get> -n <requests> -r 100000 -c 50 -P <pipeline> --csv --threads 2
```

| Server | AOF | Pipeline | Test | Requests/s | Spread | p50 ms | p99 ms | max ms | Server CPU | In kernel | Client CPU |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Mini-Redis | on | 1 | SET | 7,910 | 8.6% | 5.4 | 7.4 | 2738.2 | 15% | 78% | 24% |
| Redis | on | 1 | SET | 7,904 | 3.9% | 6.2 | 8.2 | 12.5 | 16% | 69% | 23% |
| Mini-Redis | on | 1 | GET | 29,603 | 16.1% | 1.4 | 3.3 | 10.8 | 69% | 83% | 112% |
| Redis | on | 1 | GET | 31,969 | 43.7% | 1.4 | 3.2 | 7.9 | 69% | 74% | 101% |
| Mini-Redis | on | 16 | SET | 103,675 | 17.7% | 7.5 | 10.1 | 17.3 | 30% | 42% | 24% |
| Redis | on | 16 | SET | 95,007 | 4.9% | 8.1 | 11.1 | 35.6 | 40% | 45% | 23% |
| Mini-Redis | on | 16 | GET | 252,717 | 42.6% | 2.4 | 4.7 | 1161.2 | 103% | 44% | 108% |
| Redis | on | 16 | GET | 332,890 | 7.7% | 2.2 | 4.3 | 7.7 | 105% | 42% | 97% |

## Mini-Redis relative to Redis

| AOF | Pipeline | Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis |
|---|---:|---|---:|---:|---:|
| on | 1 | SET | 7,910 | 7,904 | 1.00x |
| on | 1 | GET | 29,603 | 31,969 | 0.93x |
| on | 16 | SET | 103,675 | 95,007 | 1.09x |
| on | 16 | GET | 252,717 | 332,890 | 0.76x |
