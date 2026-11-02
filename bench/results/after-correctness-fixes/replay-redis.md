# AOF load time: real Redis, same files

- **Date (UTC):** 2026-11-02 20:21
- **Commit:** `9f39a7180251` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **redis-server:** Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d064f9f48c901a92
- **Python:** 3.12.3

- **AOF contents:** N `SET` commands; keys drawn at random from 100,000 (`key:000000012345`), 3-byte values. Byte-for-byte what Mini-Redis writes for redis-benchmark's SETs (checked against a live Mini-Redis when measuring Mini-Redis).
- **Time:** from launching the server to its first `PONG`. Redis listens while loading and answers `-LOADING` until it is done; that counts as not ready. Redis 7 converts the single-file AOF to its multi-part format as it loads it, so each run starts from a fresh copy. The AOF has just been written, so it is read from the OS page cache, not disk.
- **Check:** after each start, `GET` of the last key written must return its last value, so the whole file was really loaded.
- **Runs:** median of 3; sizes where one run took over 60 s were run once; runs are stopped after 900 s.
- **Server:** `taskset -c 2 redis-server --port <port> --bind 127.0.0.1 --dir <tmp> --save "" --appendonly yes --appendfsync no --auto-aof-rewrite-percentage 0 --logfile <tmp>/redis.log`
- **Growth exponent:** log(time ratio) / log(size ratio) against the previous row, after subtracting the 0-command startup time. 1 = linear, 2 = quadratic.

| Commands | AOF size | Startup time | Runs | Per command | Growth exponent |
|---:|---:|---:|---:|---:|---:|
| 0 | 0 B | 17 ms | 3 | – | – |
| 25,000 | 1.1 MB | 32 ms | 3 | 0.6 µs | – |
| 50,000 | 2.2 MB | 46 ms | 3 | 0.6 µs | 0.95 |
| 100,000 | 4.5 MB | 69 ms | 3 | 0.5 µs | 0.85 |
| 200,000 | 9.0 MB | 143 ms | 3 | 0.6 µs | 1.27 |
| 400,000 | 18.0 MB | 254 ms | 3 | 0.6 µs | 0.91 |
| 1,000,000 | 45.0 MB | 648 ms | 3 | 0.6 µs | 1.07 |
