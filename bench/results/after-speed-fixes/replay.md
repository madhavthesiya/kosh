# AOF replay time: Mini-Redis

- **Date (UTC):** 2026-11-03 14:07
- **Commit:** `a80e66437776` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Compiler:** c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- **Build:** CMake `RelWithDebInfo` (`-O2 -g -DNDEBUG`)
- **Python:** 3.12.3

- **AOF contents:** N `SET` commands; keys drawn at random from 100,000 (`key:000000012345`), 3-byte values. Byte-for-byte what Mini-Redis writes for redis-benchmark's SETs (checked against a live Mini-Redis when measuring Mini-Redis).
- **Time:** from launching the server to its first `PONG`. Mini-Redis replays before it listens, so this is replay plus the fixed startup cost in the 0-command row. The AOF has just been written, so it is read from the OS page cache, not disk.
- **Check:** after each start, `GET` of the last key written must return its last value, so the whole file was really loaded.
- **Runs:** median of 3; sizes where one run took over 60 s were run once; runs are stopped after 900 s.
- **Server:** `taskset -c 2 build-bench/mini_redis_server --maxkeys 200000 --appendonly yes --appendfilename <generated file>`
- **Growth exponent:** log(time ratio) / log(size ratio) against the previous row, after subtracting the 0-command startup time. 1 = linear, 2 = quadratic.

| Commands | AOF size | Startup time | Runs | Per command | Growth exponent |
|---:|---:|---:|---:|---:|---:|
| 0 | 0 B | 13 ms | 3 | – | – |
| 25,000 | 1.1 MB | 26 ms | 3 | 0.5 µs | – |
| 50,000 | 2.2 MB | 48 ms | 3 | 0.7 µs | 1.49 |
| 100,000 | 4.5 MB | 85 ms | 3 | 0.7 µs | 1.07 |
| 200,000 | 9.0 MB | 165 ms | 3 | 0.8 µs | 1.08 |
| 400,000 | 18.0 MB | 275 ms | 3 | 0.7 µs | 0.78 |
| 1,000,000 | 45.0 MB | 683 ms | 3 | 0.7 µs | 1.03 |
