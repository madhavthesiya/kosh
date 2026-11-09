# Miss ratio of the engine's eviction

Each request is a GET, and a miss is followed by a SET of the key. Capacity in keys, as a fraction of the trace's distinct keys. Synthetic traces: 10000000 requests over 1000000 keys.

| Trace | Eviction | Threads | Requests | Distinct keys | Capacity | Keys | Misses | Miss ratio |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Zipf alpha 0.99 | sieve | 1 | 10000000 | 780666 | 0.01 | 7806 | 3754102 | 0.3754 |
| Zipf alpha 0.99 | sieve | 2 | 10000000 | 780666 | 0.01 | 7806 | 3749367 | 0.3749 |
| Zipf alpha 0.99 | sieve | 4 | 10000000 | 780666 | 0.01 | 7806 | 3743292 | 0.3743 |
| Zipf alpha 0.99 | sieve | 8 | 10000000 | 780666 | 0.01 | 7806 | 3729460 | 0.3729 |
| Zipf alpha 0.99 | sieve | 1 | 10000000 | 780666 | 0.1 | 78066 | 2178173 | 0.2178 |
| Zipf alpha 0.99 | sieve | 2 | 10000000 | 780666 | 0.1 | 78066 | 2177983 | 0.2178 |
| Zipf alpha 0.99 | sieve | 4 | 10000000 | 780666 | 0.1 | 78066 | 2178328 | 0.2178 |
| Zipf alpha 0.99 | sieve | 8 | 10000000 | 780666 | 0.1 | 78066 | 2177944 | 0.2178 |
| twitter_cluster52.csv | sieve | 1 | 1000000 | 143542 | 0.01 | 1435 | 321586 | 0.3216 |
| twitter_cluster52.csv | sieve | 2 | 1000000 | 143542 | 0.01 | 1435 | 321271 | 0.3213 |
| twitter_cluster52.csv | sieve | 4 | 1000000 | 143542 | 0.01 | 1435 | 321058 | 0.3211 |
| twitter_cluster52.csv | sieve | 8 | 1000000 | 143542 | 0.01 | 1435 | 320410 | 0.3204 |
| twitter_cluster52.csv | sieve | 1 | 1000000 | 143542 | 0.1 | 14354 | 197254 | 0.1973 |
| twitter_cluster52.csv | sieve | 2 | 1000000 | 143542 | 0.1 | 14354 | 197258 | 0.1973 |
| twitter_cluster52.csv | sieve | 4 | 1000000 | 143542 | 0.1 | 14354 | 197199 | 0.1972 |
| twitter_cluster52.csv | sieve | 8 | 1000000 | 143542 | 0.1 | 14354 | 197310 | 0.1973 |
