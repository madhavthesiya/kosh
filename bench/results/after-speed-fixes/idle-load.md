# One busy client among idle ones: select() against epoll

- **Date (UTC):** 2026-11-03 14:07
- **Commit:** `a80e66437776` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Compiler:** c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- **Build:** CMake `RelWithDebInfo` (`-O2 -g -DNDEBUG`)
- **redis-benchmark:** redis-benchmark 7.0.15
- **taskset:** taskset from util-linux 2.39.3
- **Python:** 3.12.3

- **Server:** `taskset -c 2 build-bench/mini_redis_server --appendonly no --poller <select|epoll>`, with N idle connections open (each answered one PING, then stayed silent).
- **Busy client:** `taskset -c 4 redis-benchmark -c 1 -t get -n 100,000 -r 100,000 --csv` (no pipelining).
- **Runs:** 5 rounds, each measuring every case with a fresh server, the order of the pollers alternating. Medians of the rounds; CPU per request is the server's CPU time over the busy client's requests.

| Idle connections | Poller | Requests/s | p50 ms | p99 ms | Server CPU per request |
|---:|---|---:|---:|---:|---:|
| 0 | select | 3,698 (57%) | 0.215 | 0.607 | 21.2 us |
| 0 | epoll | 3,500 (71%) | 0.223 | 0.711 | 22.4 us |
| 1,000 | select | 1,719 (53%) | 0.535 | 0.927 | 283.8 us |
| 1,000 | epoll | 3,219 (71%) | 0.263 | 0.695 | 24.3 us |
