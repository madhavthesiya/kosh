# Memory layout

Commit `a80e66437776`, CMake `RelWithDebInfo`. Command: `./build-bench/memory_layout`

Compiler: GCC 13.3.0, libstdc++ 20240904
Pointer size: 8 bytes

| Type | sizeof (bytes) |
|---|---:|
| `std::string` | 32 |
| `std::deque<std::string>` (List) | 80 |
| `std::unordered_set<std::string>` (Set) | 56 |
| `std::unordered_map<std::string, std::string>` (Hash) | 56 |
| `ValueType` (`std::variant` of the four) | 88 |
| `CacheEntry` (value, expiry, LRU iterator) | 112 |
| Map element `std::pair<const std::string, CacheEntry>` | 144 |

Strings up to 15 characters are stored inside the `std::string` object (small-string optimisation); longer ones allocate.

Heap per key (glibc mallinfo2), 1000000 keys of 16 bytes with 3-byte values:
  total 298.0 MiB, 312 bytes per key, for 19 bytes of key and value
