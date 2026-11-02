# Throughput and latency

- **Date (UTC):** 2026-11-02 20:00
- **Commit:** `9f39a7180251` (clean)
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
taskset -c 2 build-bench/mini_redis_server --port <port> --maxkeys 200000 --appendonly <yes|no> --appendfilename <tmp>/appendonly.aof
taskset -c 2 redis-server --port <port> --bind 127.0.0.1 --dir <tmp> --save "" --appendonly <yes|no> --appendfsync no --auto-aof-rewrite-percentage 0 --logfile <tmp>/redis.log
taskset -c 4,6 redis-benchmark -h 127.0.0.1 -p <port> -t <set|get> -n <requests> -r 100000 -c 50 -P <pipeline> --csv --threads 2
```

| Server | AOF | Pipeline | Test | Requests/s | Spread | p50 ms | p99 ms | max ms | Server CPU | In kernel | Client CPU |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Mini-Redis | on | 1 | SET | 28,395 | 24.1% | 1.5 | 3.8 | 23.6 | 84% | 75% | 110% |
| Redis | on | 1 | SET | 30,734 | 6.8% | 1.4 | 3.4 | 21.7 | 84% | 72% | 102% |
| Mini-Redis | on | 1 | GET | 31,097 | 10.3% | 1.4 | 3.4 | 17.1 | 78% | 79% | 112% |
| Redis | on | 1 | GET | 30,500 | 16.7% | 1.4 | 3.3 | 12.2 | 75% | 76% | 106% |
| Mini-Redis | on | 16 | SET | 191,666 | 6.1% | 3.5 | 8.5 | 76.9 | 99% | 47% | 61% |
| Redis | on | 16 | SET | 228,990 | 2.5% | 3.4 | 6.0 | 13.9 | 91% | 34% | 49% |
| Mini-Redis | on | 16 | GET | 277,577 | 5.9% | 2.7 | 5.4 | 15.8 | 99% | 50% | 87% |
| Redis | on | 16 | GET | 293,686 | 7.3% | 2.5 | 4.7 | 10.2 | 92% | 41% | 74% |
| Mini-Redis | off | 1 | SET | 29,528 | 11.2% | 1.4 | 3.8 | 22.2 | 80% | 78% | 115% |
| Redis | off | 1 | SET | 29,798 | 12.2% | 1.5 | 3.5 | 19.3 | 74% | 73% | 106% |
| Mini-Redis | off | 1 | GET | 30,051 | 4.7% | 1.4 | 3.4 | 28.5 | 84% | 82% | 115% |
| Redis | off | 1 | GET | 29,602 | 18.6% | 1.5 | 3.5 | 9.8 | 77% | 77% | 104% |
| Mini-Redis | off | 16 | SET | 276,855 | 8.3% | 2.6 | 6.1 | 50.5 | 98% | 38% | 95% |
| Redis | off | 16 | SET | 277,393 | 2.9% | 2.7 | 5.2 | 12.9 | 95% | 42% | 72% |
| Mini-Redis | off | 16 | GET | 291,630 | 6.9% | 2.5 | 5.8 | 15.7 | 100% | 45% | 99% |
| Redis | off | 16 | GET | 288,934 | 33.4% | 2.6 | 4.8 | 33.3 | 94% | 45% | 76% |

## Mini-Redis relative to Redis

| AOF | Pipeline | Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis |
|---|---:|---|---:|---:|---:|
| on | 1 | SET | 28,395 | 30,734 | 0.92x |
| on | 1 | GET | 31,097 | 30,500 | 1.02x |
| on | 16 | SET | 191,666 | 228,990 | 0.84x |
| on | 16 | GET | 277,577 | 293,686 | 0.95x |
| off | 1 | SET | 29,528 | 29,798 | 0.99x |
| off | 1 | GET | 30,051 | 29,602 | 1.02x |
| off | 16 | SET | 276,855 | 277,393 | 1.00x |
| off | 16 | GET | 291,630 | 288,934 | 1.01x |
