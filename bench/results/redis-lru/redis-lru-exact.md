Commit `734c700` plus the eviction_reference.py of this commit. Command: `python3 bench/eviction_reference.py trace.txt csv:twitter_cluster52.csv --policy redis-lru --clock exact --rate 2000` (trace.txt: `eviction_sim --dump-trace trace.txt zipf:0.99 zipfscan:0.99`, B1's traces). 18 CPUs.

| Trace | Capacity | Keys | Clock | Seeds | Misses (median) | Fewest | Most | Miss ratio |
|---|---:|---:|---|---:|---:|---:|---:|---:|
| Zipf alpha 0.99 | 0.001 | 780 | a tick per request | 5 | 6377361 | 6375856 | 6379088 | 0.6377 |
| Zipf alpha 0.99 | 0.01 | 7806 | a tick per request | 5 | 4564438 | 4563972 | 4565008 | 0.4564 |
| Zipf alpha 0.99 | 0.1 | 78066 | a tick per request | 5 | 2600074 | 2599907 | 2600245 | 0.2600 |
| Zipf + scans, alpha 0.99 | 0.001 | 1752 | a tick per request | 5 | 6195553 | 6194331 | 6196461 | 0.6196 |
| Zipf + scans, alpha 0.99 | 0.01 | 17524 | a tick per request | 5 | 4672244 | 4671796 | 4672250 | 0.4672 |
| Zipf + scans, alpha 0.99 | 0.1 | 175241 | a tick per request | 5 | 2958613 | 2958312 | 2958758 | 0.2959 |
| twitter_cluster52.csv | 0.001 | 143 | a tick per request | 5 | 499370 | 498754 | 499429 | 0.4994 |
| twitter_cluster52.csv | 0.01 | 1435 | a tick per request | 5 | 339143 | 339076 | 339275 | 0.3391 |
| twitter_cluster52.csv | 0.1 | 14354 | a tick per request | 5 | 207667 | 207585 | 207801 | 0.2077 |
