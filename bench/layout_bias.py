#!/usr/bin/env python3
"""How much lookup_latency moves from code and stack layout alone.

Mytkowicz et al. ("Producing Wrong Data Without Doing Anything Obviously
Wrong!", ASPLOS 2009) showed that the size of the environment (which shifts
the stack) and the order objects are linked in (which shifts code alignment)
can each move a benchmark by several percent, with nothing else changed. So a
microbenchmark gain counts only if it is larger than what layout alone does.

This builds lookup_latency once per link order of its object files (the same
flags as the CMake RelWithDebInfo build), runs every build under several
environment sizes, in a shuffled order, and reports GET's ns per lookup for
each, with the spread (slowest / fastest). The spread includes ordinary
run-to-run noise as well as layout, so it is an upper bound on layout alone:
the floor a change's microbenchmark gain has to clear, measured on the same
tree in the same session.

Usage (from the repository root, on Linux):
  python3 bench/layout_bias.py [--sizes 100000,1000000] [--env-bytes 0,1024,2048,3072]
"""

import argparse
import itertools
import os
import random
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

import benchlib as bl

SOURCES = ["bench/lookup_latency.cpp", "InMemoryStorage.cpp", "AOFLogger.cpp"]
FLAGS = ["-std=c++17", "-O2", "-g", "-DNDEBUG", "-pthread"]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--repo", type=Path, default=bl.REPO_ROOT, help="tree to build")
    p.add_argument("--sizes", default="100000,1000000", help="lookup_latency --sizes")
    p.add_argument("--env-bytes", default="0,1024,2048,3072",
                   help="extra environment bytes to run with")
    p.add_argument("--ops", type=int, default=1_000_000)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--cpu", default="2", help="CPU for the runs (taskset)")
    p.add_argument("--compiler", default="g++")
    return p.parse_args()


def get_ns(output):
    """{keys: GET ns} from lookup_latency's uniform table, finding the
    columns by their headers"""
    result = {}
    in_uniform = False
    columns = {}
    for line in output.splitlines():
        if line.startswith("## "):
            in_uniform = line.startswith("## Uniform")
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if cells and cells[0] == "Keys":
            columns = {name: i for i, name in enumerate(cells)}
        elif in_uniform and columns and re.fullmatch(r"\d+", cells[0]):
            result[int(cells[0])] = float(cells[columns["GET"]].split()[0])
    return result


def main():
    args = parse_args()
    sizes = [int(s) for s in args.sizes.split(",")]
    env_bytes = [int(b) for b in args.env_bytes.split(",")]
    with tempfile.TemporaryDirectory(prefix="mini-redis-layout-") as tmp:
        tmp = Path(tmp)
        objects = []
        for src in SOURCES:
            obj = tmp / (Path(src).stem + ".o")
            subprocess.run([args.compiler, *FLAGS, "-c", str(args.repo / src), "-o", str(obj)],
                           check=True)
            objects.append(obj)
        binaries = []
        for n, order in enumerate(itertools.permutations(objects)):
            exe = tmp / f"lookup_latency_{n}"
            subprocess.run([args.compiler, *FLAGS, *map(str, order), "-o", str(exe)], check=True)
            binaries.append((" ".join(o.stem for o in order), exe))

        rows = []   # (link order, env bytes, {keys: ns})
        # Shuffled (with a fixed seed), so a machine that drifts over the run
        # doesn't line its drift up with one link order or environment size
        runs = list(itertools.product(binaries, env_bytes))
        random.Random(42).shuffle(runs)
        for (order, exe), extra in runs:
            # A minimal, fixed environment plus `extra` bytes of padding
            env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "PAD": "x" * extra}
            out = subprocess.run(["taskset", "-c", args.cpu, str(exe), "--sizes", args.sizes,
                                  "--ops", str(args.ops), "--repeats", str(args.repeats),
                                  "--dist", "uniform"],
                                 env=env, check=True, capture_output=True, text=True).stdout
            rows.append((order, extra, get_ns(out)))
            print(f"{order}, +{extra} env bytes: {rows[-1][2]}", file=sys.stderr, flush=True)

    print("# Layout bias of lookup_latency\n")
    print(bl.environment_markdown(bl.environment(tools=[args.compiler])) + "\n")
    print(f"GET ns per lookup (uniform keys, median of {args.repeats} repeats of "
          f"{args.ops:,} lookups), per link order and extra environment size, in the "
          "order the runs happened (so a drifting machine shows as a trend).\n")
    print("| Link order | Extra env bytes | " + " | ".join(f"{s:,} keys" for s in sizes) + " |")
    print("|---|---:|" + "---:|" * len(sizes))
    for order, extra, ns in rows:
        print(f"| {order} | {extra} | " + " | ".join(f"{ns[s]:.1f}" for s in sizes) + " |")
    print("\n| Keys | Fastest | Median | Slowest | Spread (slowest / fastest) |")
    print("|---:|---:|---:|---:|---:|")
    for s in sizes:
        values = [ns[s] for _, _, ns in rows]
        print(f"| {s:,} | {min(values):.1f} | {statistics.median(values):.1f} | "
              f"{max(values):.1f} | {max(values) / min(values):.3f} |")


if __name__ == "__main__":
    main()
