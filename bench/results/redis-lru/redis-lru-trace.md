Commit `734c700` plus the eviction_reference.py of this commit. Command: `python3 bench/eviction_reference.py trace.txt csv:twitter_cluster52.csv --policy redis-lru --clock trace --rate 2000` (trace.txt: `eviction_sim --dump-trace trace.txt zipf:0.99 zipfscan:0.99`, B1's traces). 18 CPUs.

| Trace | Capacity | Keys | Clock | Seeds | Misses (median) | Fewest | Most | Miss ratio |
|---|---:|---:|---|---:|---:|---:|---:|---:|
| Zipf alpha 0.99 | 0.001 | 780 | 1 s, 2000 requests/s | 5 | 6545297 | 6544331 | 6552204 | 0.6545 |
| Zipf alpha 0.99 | 0.01 | 7806 | 1 s, 2000 requests/s | 5 | 4565675 | 4565419 | 4565911 | 0.4566 |
| Zipf alpha 0.99 | 0.1 | 78066 | 1 s, 2000 requests/s | 5 | 2599974 | 2599608 | 2600228 | 0.2600 |
| Zipf + scans, alpha 0.99 | 0.001 | 1752 | 1 s, 2000 requests/s | 5 | 6224297 | 6224045 | 6227675 | 0.6224 |
| Zipf + scans, alpha 0.99 | 0.01 | 17524 | 1 s, 2000 requests/s | 5 | 4672381 | 4672194 | 4672527 | 0.4672 |
| Zipf + scans, alpha 0.99 | 0.1 | 175241 | 1 s, 2000 requests/s | 5 | 2958391 | 2958159 | 2958860 | 0.2958 |
| twitter_cluster52.csv | 0.001 | 143 | the trace's, 1 s | 5 | 544446 | 541815 | 544782 | 0.5444 |
| twitter_cluster52.csv | 0.01 | 1435 | the trace's, 1 s | 5 | 340706 | 340559 | 340740 | 0.3407 |
| twitter_cluster52.csv | 0.1 | 14354 | the trace's, 1 s | 5 | 207699 | 207545 | 207812 | 0.2077 |
