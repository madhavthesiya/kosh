Commit `a80e66437776`, CMake `RelWithDebInfo`. Command: `./build-bench/alloc_count`

# Heap allocations per operation

10000 keys of 16 bytes, 3-byte values, averages over 100000 operations.

| Operation | Allocations |
|---|---:|
| Parse a GET request | 0.00 |
| Parse a SET request | 0.00 |
| Engine: GET of an existing key | 0.00 |
| Engine: SET of an existing key, no AOF | 0.00 |
| Engine: SET of an existing key, AOF on | 0.00 |
| Command: GET (dispatch, engine, reply) | 0.00 |
| Command: SET, AOF on (dispatch, engine, log, reply) | 0.00 |
| Request: GET, parsed and run | 0.00 |
| Request: SET, AOF on, parsed and run | 0.00 |
| Command: GET of a 100-byte value | 0.00 |
| Request: GET of a 100-byte value, parsed and run | 0.00 |
