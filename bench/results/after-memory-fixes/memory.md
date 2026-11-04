# Memory per key

- **Date (UTC):** 2026-11-03 21:05
- **Commit:** `cbae3d9b1315` (clean)
- **CPU:** Intel(R) Core(TM) Ultra 5 125H, 18 logical CPUs
- **Memory:** 7.5 GiB
- **Kernel:** Linux 6.6.87.2-microsoft-standard-WSL2 (WSL2: CPUs are virtual, so pinning keeps processes apart but does not fix the physical core)
- **Compiler:** c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- **Build:** CMake `RelWithDebInfo` (`-O2 -g -DNDEBUG`)
- **redis-server:** Redis server v=7.0.15 sha=00000000:0 malloc=jemalloc-5.3.0 bits=64 build=d064f9f48c901a92
- **Python:** 3.12.3

- **Keys:** 1,000,000 keys of 16 bytes (`key:000000012345`) with 3-byte values, loaded over one connection, 1,000 SETs per write; no AOF.
- **Measure:** growth of the server's resident memory (VmRSS). Mini-Redis's baseline is a server started with `--maxkeys 1`, so the growth includes the hash table it sizes at startup; Redis's is the empty server.
- **Runs:** 3, the order alternating; median, with every run's value in brackets.

| Server | Allocator | RSS growth | Bytes per key | Redis's `used_memory` per key |
|---|---|---:|---:|---:|
| Mini-Redis | glibc malloc | 124.0 MB | 124 (124, 124, 124) | - |
| Mini-Redis | jemalloc (LD_PRELOAD) | 120.8 MB | 121 (121, 121, 121) | - |
| Redis | jemalloc (built in) | 115.6 MB | 116 (116, 116, 115) | 104 |
