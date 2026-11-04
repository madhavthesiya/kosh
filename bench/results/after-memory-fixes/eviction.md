Commit `cbae3d9b1315`, CMake `RelWithDebInfo`. Command: `./build-bench/eviction_sim --eviction both --dump-trace trace.txt zipf:0.99 zipfscan:0.99 csv:twitter_cluster52.csv`

# Miss ratio of the engine's eviction

Each request is a GET, and a miss is followed by a SET of the key. Capacity in keys, as a fraction of the trace's distinct keys. Synthetic traces: 10000000 requests over 1000000 keys.

| Trace | Eviction | Requests | Distinct keys | Capacity | Keys | Misses | Miss ratio | ns per request |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Zipf alpha 0.99 | lru | 10000000 | 780666 | 0.001 | 780 | 6355494 | 0.6355 | 160 |
| Zipf alpha 0.99 | sieve | 10000000 | 780666 | 0.001 | 780 | 5281637 | 0.5282 | 127 |
| Zipf alpha 0.99 | lru | 10000000 | 780666 | 0.01 | 7806 | 4543724 | 0.4544 | 128 |
| Zipf alpha 0.99 | sieve | 10000000 | 780666 | 0.01 | 7806 | 3754102 | 0.3754 | 118 |
| Zipf alpha 0.99 | lru | 10000000 | 780666 | 0.1 | 78066 | 2581993 | 0.2582 | 122 |
| Zipf alpha 0.99 | sieve | 10000000 | 780666 | 0.1 | 78066 | 2178173 | 0.2178 | 108 |
| Zipf + scans, alpha 0.99 | lru | 10000000 | 1752419 | 0.001 | 1752 | 6176494 | 0.6176 | 152 |
| Zipf + scans, alpha 0.99 | sieve | 10000000 | 1752419 | 0.001 | 1752 | 5250630 | 0.5251 | 123 |
| Zipf + scans, alpha 0.99 | lru | 10000000 | 1752419 | 0.01 | 17524 | 4656362 | 0.4656 | 121 |
| Zipf + scans, alpha 0.99 | sieve | 10000000 | 1752419 | 0.01 | 17524 | 3892385 | 0.3892 | 112 |
| Zipf + scans, alpha 0.99 | lru | 10000000 | 1752419 | 0.1 | 175241 | 2944615 | 0.2945 | 144 |
| Zipf + scans, alpha 0.99 | sieve | 10000000 | 1752419 | 0.1 | 175241 | 2570244 | 0.2570 | 128 |
| twitter_cluster52.csv | lru | 1000000 | 143542 | 0.001 | 143 | 495485 | 0.4955 | 123 |
| twitter_cluster52.csv | sieve | 1000000 | 143542 | 0.001 | 143 | 488354 | 0.4884 | 117 |
| twitter_cluster52.csv | lru | 1000000 | 143542 | 0.01 | 1435 | 337587 | 0.3376 | 102 |
| twitter_cluster52.csv | sieve | 1000000 | 143542 | 0.01 | 1435 | 321586 | 0.3216 | 100 |
| twitter_cluster52.csv | lru | 1000000 | 143542 | 0.1 | 14354 | 206786 | 0.2068 | 80 |
| twitter_cluster52.csv | sieve | 1000000 | 143542 | 0.1 | 14354 | 197254 | 0.1973 | 80 |
