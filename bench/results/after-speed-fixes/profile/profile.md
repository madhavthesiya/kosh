# CPU profile

- **Date (UTC):** 2026-11-03 14:25
- **Commit:** `6b1eadf6e589` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Compiler:** c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- **Build:** CMake `RelWithDebInfo` (`-O2 -g -DNDEBUG`)
- **Python:** 3.12.3
- **perf:** perf version 6.8.12
- **FlameGraph:** `41fee1f99f92`

- **Sampling:** `perf record -e cpu-clock -F 499 --call-graph dwarf,16384 -p <server pid> -- sleep 10`, after 2 s of load.
- **Server:** `taskset -c 2 build-bench/mini_redis_server --maxkeys 200000 --appendonly yes`
- **Load:** `taskset -c 4,6 redis-benchmark -t <test> -r 100000 -c 50 -P <pipeline> -l --threads 2` (GET runs on keys written by a SET pass first).
- Throughput under the profiler is not a benchmark result: sampling with stack copies slows the server down.
- **Server CPU** comes from tick-based accounting (`/proc/<pid>/stat`), which in a VM can read a few percent above 100% for one thread; read ~100% as saturated.
- **Inclusive** columns: share of samples with that call anywhere on the stack, i.e. the call and everything it does in the kernel.
- Without hardware counters (as in a VM), perf samples on a timer interrupt, which cannot fire while the kernel has interrupts disabled. Such time is charged to the function that re-enables them, so `_raw_spin_unlock_irqrestore` in the tables stands for kernel work done with interrupts off, not for unlocking itself.

| Workload | Server CPU | In kernel | send() | write(), the AOF | recv() | Waiting: select() or epoll_wait() | All AOF logging (InMemoryStorage::logAOF) | RESP parsing | Files |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| set-p1 | 86% | 76% | 50.0% | 6.9% | 11.9% | 8.0% | 2.2% | 2.4% | [flame graph](set-p1.svg), [top functions](set-p1-top.txt) |
| set-p16 | 79% | 31% | 14.8% | 7.4% | 5.3% | 2.2% | 10.7% | 9.7% | [flame graph](set-p16.svg), [top functions](set-p16-top.txt) |
| get-p16 | 66% | 30% | 20.2% | 0.1% | 7.0% | 3.3% | 0.0% | 7.2% | [flame graph](get-p16.svg), [top functions](get-p16-top.txt) |
