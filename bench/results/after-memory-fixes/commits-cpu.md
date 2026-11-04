# Server CPU per request, by commit

From [`commits-runs.csv`](commits-runs.csv) (the runs of [`commits.md`](commits.md)), with `compare_commits.py`'s functions. Each cell: the median of the per-round ratios of requests per CPU-second, second version over first (above 1.00: less CPU per request), [lowest, highest], the rounds in which the second used less, the sign-test p-value, and a 95% interval.

| AOF | Pipeline | Test | sieve-default / v2-fast | keytable / sieve-default | keytable / v2-fast |
|---|---:|---|---|---|---|
| on | 16 | SET | 0.987x [0.629, 1.119] 3/6 p=1.00, CI 0.73-1.17 | 1.097x [0.938, 1.224] 5/6 p=0.22, CI 0.99-1.22 | 1.107x [0.591, 1.259] 5/6 p=0.22, CI 0.76-1.35 |
| on | 16 | GET | 0.976x [0.479, 1.292] 3/6 p=1.00, CI 0.64-1.32 | 1.085x [0.902, 1.415] 5/6 p=0.22, CI 0.94-1.29 | 1.103x [0.555, 1.328] 5/6 p=0.22, CI 0.73-1.40 |
| off | 16 | SET | 1.066x [0.631, 1.155] 5/6 p=0.22, CI 0.78-1.26 | 1.104x [1.016, 1.139] 6/6 p=0.03, CI 1.04-1.14 | 1.146x [0.641, 1.316] 5/6 p=0.22, CI 0.82-1.43 |
| off | 16 | GET | 1.045x [0.803, 1.274] 4/6 p=0.69, CI 0.88-1.22 | 1.108x [0.957, 1.328] 5/6 p=0.22, CI 0.99-1.28 | 1.133x [0.840, 1.692] 5/6 p=0.22, CI 0.91-1.49 |
