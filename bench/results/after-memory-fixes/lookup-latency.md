Commit `cbae3d9b1315`, CMake `RelWithDebInfo`. Command: `taskset -c 2 ./build-bench/lookup_latency --sizes 1000,100000,1000000 --eviction both`

# Lookup latency in the storage engine

Keys of 16 bytes ("key:000000012345") inserted in random order, 3-byte values, no AOF, capacity = key count (no eviction). 2000000 lookups per measurement, 5 repeats; Zipf alpha 0.99, scrambled.

## Uniform keys

ns per lookup, median of the repeats (min-max).

| Keys | Eviction | Engine memory | GET | GET, one at a time | SET of an existing key | find() in std::unordered_map | find(), one at a time | find() in KeyTable | KeyTable, one at a time |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1000 | lru | 0.1 MB | 46.3 (42.9-47.2) | 45.7 (42.4-46.4) | 55.9 (53.9-62.6) | 28.0 (23.7-33.4) | 28.0 (24.3-32.1) | 31.8 (29.4-34.0) | 32.1 (29.0-33.9) |
| 1000 | sieve | 0.1 MB | 44.3 (41.8-47.2) | 43.9 (42.7-49.2) | 54.1 (51.5-58.6) | - | - | - | - |
| 100000 | lru | 10.0 MB | 74.9 (73.5-80.6) | 83.7 (81.8-94.4) | 90.4 (87.8-99.7) | 103.4 (98.2-111.2) | 105.2 (95.5-110.5) | 50.3 (46.8-52.7) | 48.7 (47.3-50.5) |
| 100000 | sieve | 10.0 MB | 73.6 (70.5-76.7) | 83.2 (81.4-115.0) | 91.3 (89.7-93.7) | - | - | - | - |
| 1000000 | lru | 100.3 MB | 187.7 (185.7-212.1) | 249.5 (236.4-269.0) | 294.9 (280.6-313.3) | 222.9 (205.4-224.0) | 212.6 (207.4-226.7) | 106.9 (101.7-111.2) | 104.1 (98.5-110.0) |
| 1000000 | sieve | 100.3 MB | 168.0 (157.3-176.2) | 250.6 (237.9-260.1) | 310.9 (294.3-320.3) | - | - | - | - |

## Zipf keys (alpha 0.99)

ns per lookup, median of the repeats (min-max).

| Keys | Eviction | Engine memory | GET | GET, one at a time | SET of an existing key | find() in std::unordered_map | find(), one at a time | find() in KeyTable | KeyTable, one at a time |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1000 | lru | 0.1 MB | 42.2 (41.6-44.6) | 42.6 (41.5-45.8) | 51.9 (51.0-59.5) | 24.6 (21.9-34.3) | 23.7 (23.1-30.9) | 29.1 (27.2-29.9) | 28.1 (27.9-30.3) |
| 1000 | sieve | 0.1 MB | 39.8 (38.9-45.6) | 40.4 (39.5-42.1) | 50.2 (49.6-56.8) | - | - | - | - |
| 100000 | lru | 10.0 MB | 63.9 (60.2-98.8) | 70.2 (67.4-86.2) | 80.6 (77.5-141.4) | 87.0 (70.4-106.1) | 74.3 (68.8-105.3) | 45.9 (40.1-61.9) | 45.0 (41.6-66.3) |
| 100000 | sieve | 10.0 MB | 65.9 (58.4-102.4) | 74.3 (70.7-116.7) | 77.9 (75.0-136.2) | - | - | - | - |
| 1000000 | lru | 100.3 MB | 127.0 (126.0-129.2) | 145.0 (140.3-150.2) | 165.5 (157.3-170.1) | 151.4 (147.0-159.1) | 147.7 (146.5-151.1) | 79.2 (77.8-79.5) | 79.6 (77.1-83.5) |
| 1000000 | sieve | 100.3 MB | 116.6 (113.3-121.6) | 153.4 (141.3-159.1) | 161.3 (158.7-172.1) | - | - | - | - |

The harness alone (copying each key, no lookup): 3.9 ns per lookup.
Engine memory: heap growth from inserting the keys (glibc mallinfo2), including the hash table's bucket array.
