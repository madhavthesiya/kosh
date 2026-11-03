Commit `a80e66437776`, CMake `RelWithDebInfo`. Command: `taskset -c 2 ./build-bench/insert_latency 100000 5`

```
# Insert latency of new keys

100000 new keys of 16 bytes ("key:000000012345"), random order, 3-byte values; 5 repeats.

## 1. std::unordered_map<std::string, 112-byte entry>: every rehash

Repeat 1: rehashes that took over 100 us (keys before -> buckets after: time)
      10273 ->   20753:      529 us
      20753 ->   42043:      759 us
      42043 ->   85229:     1798 us
      85229 ->  172933:     8634 us
    all rehashes together: 11963 us
Repeat 2: rehashes that took over 100 us (keys before -> buckets after: time)
      10273 ->   20753:      140 us
      20753 ->   42043:      398 us
      42043 ->   85229:     1049 us
      85229 ->  172933:     8845 us
    all rehashes together: 10521 us
Repeat 3: rehashes that took over 100 us (keys before -> buckets after: time)
      10273 ->   20753:      137 us
      20753 ->   42043:      452 us
      42043 ->   85229:     1294 us
      85229 ->  172933:     6326 us
    all rehashes together: 8299 us
Repeat 4: rehashes that took over 100 us (keys before -> buckets after: time)
      10273 ->   20753:      145 us
      20753 ->   42043:      758 us
      42043 ->   85229:     2609 us
      85229 ->  172933:     8844 us
    all rehashes together: 12465 us
Repeat 5: rehashes that took over 100 us (keys before -> buckets after: time)
      10273 ->   20753:      138 us
      20753 ->   42043:      407 us
      42043 ->   85229:     1385 us
      85229 ->  172933:     8629 us
    all rehashes together: 10663 us

## 2. InMemoryStorage::set of a new key (no AOF)

Repeat 1: p50 0.18 us, p99 1.77 us, p99.9 13.75 us, max 112 us. Slowest:
         112 us  at key 61740
          92 us  at key 65949
          79 us  at key 68792
          53 us  at key 69083
Repeat 2: p50 0.17 us, p99 0.73 us, p99.9 0.99 us, max 28 us. Slowest:
          28 us  at key 59621
          21 us  at key 92511
          20 us  at key 72528
          18 us  at key 12017
Repeat 3: p50 0.19 us, p99 0.76 us, p99.9 1.07 us, max 82 us. Slowest:
          82 us  at key 14967
          81 us  at key 24234
          50 us  at key 14182
          49 us  at key 70381
Repeat 4: p50 0.17 us, p99 0.72 us, p99.9 1.00 us, max 30 us. Slowest:
          30 us  at key 9584
          20 us  at key 66270
          18 us  at key 82747
          18 us  at key 60199
Repeat 5: p50 0.22 us, p99 0.78 us, p99.9 1.07 us, max 69 us. Slowest:
          69 us  at key 54001
          44 us  at key 56091
          39 us  at key 77162
          37 us  at key 76872
```
