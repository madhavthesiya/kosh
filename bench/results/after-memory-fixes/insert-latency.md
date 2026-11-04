Commit `cbae3d9b1315`, CMake `RelWithDebInfo`. Command: `taskset -c 2 ./build-bench/insert_latency 1000000 3`

```
# Insert latency of new keys

1000000 new keys of 16 bytes ("key:000000012345"), random order, 3-byte values; 3 repeats.

## 1. std::unordered_map<std::string, 112-byte entry>: every rehash

Repeat 1: rehashes that took over 100 us (keys before -> buckets after: time)
      10273 ->   20753:      163 us
      20753 ->   42043:      416 us
      42043 ->   85229:     1769 us
      85229 ->  172933:     6010 us
     172933 ->  351061:    20325 us
     351061 ->  712697:    36343 us
     712697 -> 1447153:    92556 us
    all rehashes together: 157717 us
Repeat 2: rehashes that took over 100 us (keys before -> buckets after: time)
      10273 ->   20753:      111 us
      20753 ->   42043:      349 us
      42043 ->   85229:     1180 us
      85229 ->  172933:     5289 us
     172933 ->  351061:    14459 us
     351061 ->  712697:    33425 us
     712697 -> 1447153:    77629 us
    all rehashes together: 132524 us
Repeat 3: rehashes that took over 100 us (keys before -> buckets after: time)
      10273 ->   20753:      117 us
      20753 ->   42043:      412 us
      42043 ->   85229:     1166 us
      85229 ->  172933:     3932 us
     172933 ->  351061:    13849 us
     351061 ->  712697:    38055 us
     712697 -> 1447153:    80145 us
    all rehashes together: 137743 us

## 2. InMemoryStorage::set of a new key (no AOF)

Repeat 1: p50 0.13 us, p99 1.53 us, p99.9 6.08 us, max 286 us. Slowest:
         286 us  at key 283683
         227 us  at key 924337
         218 us  at key 995474
         203 us  at key 199984
Repeat 2: p50 0.14 us, p99 1.56 us, p99.9 4.91 us, max 405 us. Slowest:
         405 us  at key 942252
         291 us  at key 311583
         238 us  at key 724425
         236 us  at key 134556
Repeat 3: p50 0.13 us, p99 1.64 us, p99.9 5.32 us, max 272 us. Slowest:
         272 us  at key 503953
         234 us  at key 919208
         234 us  at key 463725
         215 us  at key 52469
```
