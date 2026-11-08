## 1. Keys chosen to collide under std::hash

Found 40000 keys whose std::hash agrees in its low 15 bits, from 1.31e+09 candidates, in 31.6 s on one core.

Look up, then insert, each key into an empty table: median ms of 5 rounds

| Keys | Colliding keys, std::hash | Colliding keys, KeyedHash | Ordinary keys, std::hash | Ordinary keys, KeyedHash |
|---:|---:|---:|---:|---:|
| 2500 | 5.8 | 0.29 | 0.24 | 0.28 |
| 5000 | 30.8 | 0.62 | 0.47 | 0.58 |
| 10000 | 137.8 | 1.22 | 0.97 | 1.19 |
| 20000 | 624.7 | 3.18 | 2.48 | 3.17 |
| 40000 | 2019.2 | 5.46 | 3.96 | 5.16 |

## 2. The cost for ordinary keys

One hash of a 16-byte key: std::hash 3.43 ns, SipHash-1-3 11.22 ns, SipHash-2-4 19.25 ns (median of 5 rounds)

Lookups of random keys: median ns per lookup of 5 rounds

| Keys | Independent: std::hash | KeyedHash | ratio | One at a time: std::hash | KeyedHash | ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 1000 | 32.1 | 38.0 | 1.18x | 37.3 | 41.4 | 1.11x |
| 100000 | 49.1 | 73.0 | 1.49x | 79.7 | 91.4 | 1.15x |
| 1000000 | 119.7 | 230.2 | 1.92x | 267.3 | 293.5 | 1.10x |
