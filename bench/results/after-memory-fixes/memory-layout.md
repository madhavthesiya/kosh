# Memory layout

Commit `cbae3d9b1315`, CMake `RelWithDebInfo`. Command: `./build-bench/memory_layout`

Compiler: GCC 13.3.0, libstdc++ 20240904
Pointer size: 8 bytes

| Type | sizeof (bytes) |
|---|---:|
| `std::string` | 32 |
| `std::deque<std::string>` (List) | 80 |
| `std::unordered_set<std::string>` (Set) | 56 |
| `std::unordered_map<std::string, std::string>` (Hash) | 56 |
| `ValueType` (`std::variant`: a string, or a pointer to a list, set or hash) | 40 |
| `CacheEntry` (value, expiry, eviction queue links, key length, flags); the key's bytes follow it in the same allocation | 72 |
| `KeyTable` bucket (one cache line: metadata, 7 tags, 7 entry pointers) | 64 |

Strings up to 15 characters are stored inside the `std::string` object (small-string optimisation); longer ones allocate.

Heap per key (glibc mallinfo2), 1000000 keys of 16 bytes with 3-byte values:
  total 95.7 MiB, 100 bytes per key, for 19 bytes of key and value
