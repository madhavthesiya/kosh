#!/usr/bin/env python3
"""The commit comparison of commits-runs.csv, for server CPU per request
instead of requests/s: round by round, the ratio of requests per CPU-second
(higher = less CPU per request), with compare_commits' own median, range,
sign test and 95% interval.

Usage: python3 bench/results/after-memory-fixes/cpu_paired.py > commits-cpu.md
"""
import csv
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))      # bench/
import compare_commits as cc                   # noqa: E402

runs = {}
for r in csv.DictReader(open(HERE / "commits-runs.csv")):
    key = (r["aof"], int(r["pipeline"]), r["test"], r["label"])
    per_cpu_second = float(r["rps"]) / (float(r["server_cpu_pct"]) / 100)
    runs.setdefault(key, {})[r["round"]] = {"rps": per_cpu_second}

pairs = [("v2-fast", "sieve-default"), ("sieve-default", "keytable"), ("v2-fast", "keytable")]
print("# Server CPU per request, by commit\n")
print("From [`commits-runs.csv`](commits-runs.csv) (the runs of [`commits.md`](commits.md)), "
      "with `compare_commits.py`'s functions. Each cell: the median of the per-round ratios "
      "of requests per CPU-second, second version over first (above 1.00: less CPU per "
      "request), [lowest, highest], the rounds in which the second used less, the "
      "sign-test p-value, and a 95% interval.\n")
print("| AOF | Pipeline | Test | " + " | ".join(f"{b} / {a}" for a, b in pairs) + " |")
print("|---|---:|---|" + "---|" * len(pairs))
for aof in ("on", "off"):
    for test in ("SET", "GET"):
        print(f"| {aof} | 16 | {test} | " +
              " | ".join(cc.paired_cell(runs, aof, 16, test, a, b) for a, b in pairs) + " |")
