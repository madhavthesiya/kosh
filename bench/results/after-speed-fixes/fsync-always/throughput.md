# Throughput and latency

- **Date (UTC):** 2026-11-03 14:56
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
taskset -c 2 build-bench/mini_redis_server --port <port> --maxkeys 200000 --appendonly <yes|no> --appendfilename <tmp>/appendonly.aof --appendfsync always
taskset -c 2 redis-server --port <port> --bind 127.0.0.1 --dir <tmp> --save "" --appendonly <yes|no> --appendfsync always --auto-aof-rewrite-percentage 0 --logfile <tmp>/redis.log
taskset -c 4,6 redis-benchmark -h 127.0.0.1 -p <port> -t <set|get> -n <requests> -r 100000 -c 50 -P <pipeline> --csv --threads 2
```

| Server | AOF | Pipeline | Test | Requests/s | Spread | p50 ms | p99 ms | max ms | Server CPU | In kernel | Client CPU |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Mini-Redis | on | 1 | SET | 6,236 | 1.5% | 7.8 | 10.5 | 25.7 | 14% | 78% | 27% |
| Redis | on | 1 | SET | 6,337 | 11.9% | 7.5 | 16.3 | 24.1 | 16% | 67% | 28% |
| Mini-Redis | on | 1 | GET | 22,188 | 21.0% | 1.8 | 4.2 | 7.4 | 78% | 77% | 149% |
| Redis | on | 1 | GET | 24,972 | 20.1% | 1.6 | 3.8 | 8.6 | 81% | 68% | 138% |
| Mini-Redis | on | 16 | SET | 90,711 | 18.3% | 8.4 | 11.6 | 23.6 | 34% | 35% | 30% |
| Redis | on | 16 | SET | 84,908 | 27.9% | 9.0 | 14.3 | 23.8 | 40% | 27% | 26% |
| Mini-Redis | on | 16 | GET | 285,225 | 23.7% | 2.3 | 5.0 | 7.6 | 95% | 31% | 130% |
| Redis | on | 16 | GET | 266,205 | 23.2% | 2.5 | 5.6 | 9.1 | 92% | 26% | 106% |

## Mini-Redis relative to Redis

| AOF | Pipeline | Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis |
|---|---:|---|---:|---:|---:|
| on | 1 | SET | 6,236 | 6,337 | 0.98x |
| on | 1 | GET | 22,188 | 24,972 | 0.89x |
| on | 16 | SET | 90,711 | 84,908 | 1.07x |
| on | 16 | GET | 285,225 | 266,205 | 1.07x |
