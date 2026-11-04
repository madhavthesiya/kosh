# Exact check of the miss counts in [eviction.md](eviction.md)

The same requests, replayed through `bench/eviction_reference.py` (a textbook LRU, and SIEVE from the paper's pseudocode).

## lru

| Trace | Capacity | Keys | Misses (reference) | Misses (engine) |
|---|---:|---:|---:|---:|
| Zipf alpha 0.99 | 0.001 | 780 | 6355494 | 6355494 |
| Zipf alpha 0.99 | 0.01 | 7806 | 4543724 | 4543724 |
| Zipf alpha 0.99 | 0.1 | 78066 | 2581993 | 2581993 |
| Zipf + scans, alpha 0.99 | 0.001 | 1752 | 6176494 | 6176494 |
| Zipf + scans, alpha 0.99 | 0.01 | 17524 | 4656362 | 4656362 |
| Zipf + scans, alpha 0.99 | 0.1 | 175241 | 2944615 | 2944615 |
| twitter_cluster52.csv | 0.001 | 143 | 495485 | 495485 |
| twitter_cluster52.csv | 0.01 | 1435 | 337587 | 337587 |
| twitter_cluster52.csv | 0.1 | 14354 | 206786 | 206786 |

All 9 counts match.

## sieve

| Trace | Capacity | Keys | Misses (reference) | Misses (engine) |
|---|---:|---:|---:|---:|
| Zipf alpha 0.99 | 0.001 | 780 | 5281637 | 5281637 |
| Zipf alpha 0.99 | 0.01 | 7806 | 3754102 | 3754102 |
| Zipf alpha 0.99 | 0.1 | 78066 | 2178173 | 2178173 |
| Zipf + scans, alpha 0.99 | 0.001 | 1752 | 5250630 | 5250630 |
| Zipf + scans, alpha 0.99 | 0.01 | 17524 | 3892385 | 3892385 |
| Zipf + scans, alpha 0.99 | 0.1 | 175241 | 2570244 | 2570244 |
| twitter_cluster52.csv | 0.001 | 143 | 488354 | 488354 |
| twitter_cluster52.csv | 0.01 | 1435 | 321586 | 321586 |
| twitter_cluster52.csv | 0.1 | 14354 | 197254 | 197254 |

All 9 counts match.

