#!/usr/bin/env python3
"""Mini-Redis throughput at several commits, measured in one interleaved session.

Each commit is checked out into its own git worktree and measured with that
commit's own bench.py (Mini-Redis only). Every round measures every commit,
so each round gives a paired comparison: the versions measured minutes apart,
under the same conditions. The order rotates from round to round, so no
version is always measured first.

For each pair of neighbouring versions, the report gives the median of the
per-round ratios, their range, how many rounds went each way, and a sign
test: the chance of at least that many rounds agreeing if the change made no
difference. A change is reported as real only when the rounds agree. With 5
rounds that all agree, p = 0.0625; with 6, p = 0.031.

Numbers from different sessions are not comparable on a noisy machine; this
puts every version in the same session, which is what a before/after claim
for a single commit needs.

Usage (from the repository root, on Linux):
  python3 bench/compare_commits.py --out bench/results/<name>/commits.md \\
      before=<commit> after=<commit> [more=<commit> ...]
The raw numbers of every run go next to it, in commits-runs.csv.
"""

import argparse
import csv
import math
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

import benchlib as bl

RUN_FIELDS = ["round", "order", "label", "aof", "pipeline", "test", "rps", "p50_ms",
              "p99_ms", "max_ms", "server_cpu_pct", "kernel_pct", "client_cpu_pct"]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("versions", nargs="+", help="label=commit, in the order to show them")
    p.add_argument("--out", required=True, type=Path, help="Markdown file to write")
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--pipelines", default="16")
    p.add_argument("--aof", default="on,off")
    p.add_argument("--requests", type=int, default=1_000_000, help="per unpipelined test")
    # Long runs: with --threads, redis-benchmark rounds each run time up to its
    # 250 ms timer (see bench.py), which matters for fast pipelined runs
    p.add_argument("--pipelined-requests", type=int, default=5_000_000,
                   help="per pipelined test")
    args = p.parse_args()
    args.versions = [v.split("=", 1) for v in args.versions]
    if any(len(v) != 2 for v in args.versions) or len(args.versions) < 2:
        p.error("give at least two versions as label=commit")
    if len({label for label, _ in args.versions}) != len(args.versions):
        p.error("labels must be different")
    return args


def git(*argv):
    return subprocess.run(["git", "-C", str(bl.REPO_ROOT), *argv], check=True,
                          capture_output=True, text=True).stdout.strip()


def request_flags(tree, args):
    """bench.py's request-count flags for this commit. bench.py has taken a
    separate count for pipelined tests since 3ac13ff; before that, one count
    applied to every test."""
    if "--pipelined-requests" in (tree / "bench" / "bench.py").read_text():
        return ["--requests", str(args.requests),
                "--pipelined-requests", str(args.pipelined_requests)], None
    pipelined = any(int(p) > 1 for p in args.pipelines.split(","))
    count = args.pipelined_requests if pipelined else args.requests
    return ["--requests", str(count)], f"{count:,} requests for every test (older bench.py)"


def sign_test_p(ratios):
    """Two-sided exact sign test: the chance of rounds splitting at least this
    unevenly around 1.00 if the change made no difference (ties are dropped)."""
    up = sum(r > 1 for r in ratios)
    down = sum(r < 1 for r in ratios)
    n = up + down
    if n == 0:
        return 1.0
    tail = sum(math.comb(n, i) for i in range(min(up, down) + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def paired_cell(runs, aof, pipeline, test, before, after):
    """Median ratio after/before over the rounds both were measured in, with
    its range, the rounds that went up, and the sign test."""
    a, b = runs[(aof, pipeline, test, before)], runs[(aof, pipeline, test, after)]
    ratios = [b[r]["rps"] / a[r]["rps"] for r in sorted(a.keys() & b.keys())]
    up = sum(x > 1 for x in ratios)
    return (f"{statistics.median(ratios):.3f}x [{min(ratios):.3f}, {max(ratios):.3f}] "
            f"{up}/{len(ratios)} p={sign_test_p(ratios):.2f}")


def main():
    args = parse_args()
    if args.out.exists():
        sys.exit(f"{args.out} already exists; pick another --out or delete it")
    runs_csv = args.out.with_name(args.out.stem + "-runs.csv")
    # Not /tmp: under WSL, systemd empties it whenever the distribution restarts.
    work = Path.home() / ".cache" / "mini-redis-compare" / time.strftime("%Y%m%d-%H%M%S")
    work.mkdir(parents=True)

    versions = []   # (label, worktree, commit description, request flags)
    notes = []
    rows = []       # every run, for the CSV and the tables
    try:
        for label, commit in args.versions:
            tree = work / label
            git("worktree", "add", "--detach", str(tree), commit)
            if not (tree / "bench" / "bench.py").exists():
                sys.exit(f"{commit} has no bench/bench.py")
            flags, note = request_flags(tree, args)
            if note:
                notes.append(f"`{label}`: {note}")
            versions.append((label, tree, git("log", "-1", "--format=%h %s", commit), flags))

        for round_ in range(1, args.rounds + 1):
            # Rotate the order each round, so no version is always measured
            # first, and reverse it every other round. Rotation alone keeps
            # neighbours in the same order in all but one round (Phase 4's run
            # measured "final" right before "epoll" in all 5), so anything that
            # drifts within a round favoured the same one of each compared pair.
            shift = (round_ - 1) % len(versions)
            ordered = versions[shift:] + versions[:shift]
            if round_ % 2 == 0:
                ordered.reverse()
            for order, (label, tree, _, flags) in enumerate(ordered, 1):
                out = work / f"{label}-r{round_}"
                result = subprocess.run(
                    [sys.executable, str(tree / "bench" / "bench.py"),
                     "--out", str(out), "--servers", "mini", "--pipelines", args.pipelines,
                     "--aof", args.aof, "--runs", "1", *flags],
                    capture_output=True, text=True)
                if result.returncode != 0:
                    sys.exit(f"bench.py failed for {label}:\n{result.stderr[-2000:]}")
                for r in csv.DictReader(open(out / "throughput-runs.csv")):
                    rows.append({"round": round_, "order": order, "label": label,
                                 **{k: r.get(k, "") for k in RUN_FIELDS[3:]}})
            print(f"round {round_}/{args.rounds} done", file=sys.stderr, flush=True)
    finally:
        for _, tree, _, _ in versions:
            subprocess.run(["git", "-C", str(bl.REPO_ROOT), "worktree", "remove", "--force",
                            str(tree)], capture_output=True)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(runs_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=RUN_FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    # runs[(aof, pipeline, test, label)][round] = that run's numbers
    runs = {}
    for r in rows:
        key = (r["aof"], int(r["pipeline"]), r["test"], r["label"])
        runs.setdefault(key, {})[r["round"]] = {
            "rps": float(r["rps"]), "cpu": float(r["server_cpu_pct"]), "p99": float(r["p99_ms"])}

    labels = [label for label, _, _, _ in versions]
    tests = [(aof, int(p), t) for aof in args.aof.split(",")
             for p in args.pipelines.split(",") for t in ("SET", "GET")]

    def median_of(key, field):
        return statistics.median(v[field] for v in runs[key].values())

    lines = ["# Throughput by commit", "",
             bl.environment_markdown(bl.environment()), "",
             f"- **Method:** each commit's own `bench.py --servers mini --pipelines "
             f"{args.pipelines} --aof {args.aof} --runs 1 --requests {args.requests} "
             f"--pipelined-requests {args.pipelined_requests}`. {args.rounds} rounds; each "
             "round measures every commit, in an order that rotates from round to round and "
             "is reversed every other round.",
             f"- **Raw numbers:** every run is in [`{runs_csv.name}`]({runs_csv.name})."]
    lines += [f"- **Note:** {n}" for n in notes]
    lines += ["", "| Version | Commit |", "|---|---|"]
    lines += [f"| {label} | `{desc}` |" for label, _, desc, _ in versions]

    lines += ["", "## Requests/s", "",
              "Median of the rounds, with (max - min) / median in brackets.", "",
              "| AOF | Pipeline | Test | " + " | ".join(labels) + " |",
              "|---|---:|---|" + "---:|" * len(labels)]
    for aof, pipeline, test in tests:
        cells = []
        for label in labels:
            values = [v["rps"] for v in runs[(aof, pipeline, test, label)].values()]
            m = statistics.median(values)
            cells.append(f"{m:,.0f} ({100 * (max(values) - min(values)) / m:.0f}%)")
        lines.append(f"| {aof} | {pipeline} | {test} | " + " | ".join(cells) + " |")

    pairs = [(labels[i - 1], labels[i]) for i in range(1, len(labels))]
    if len(labels) > 2:
        pairs.append((labels[0], labels[-1]))
    lines += ["", "## Paired comparison", "",
              "Each cell compares two versions round by round: the median of the per-round "
              "ratios of requests/s, [lowest, highest] ratio, the rounds in which the second "
              "version was faster, and the two-sided sign-test p-value. Treat a change as real "
              "only when the rounds agree (a small p).", "",
              "| AOF | Pipeline | Test | " + " | ".join(f"{b} / {a}" for a, b in pairs) + " |",
              "|---|---:|---|" + "---|" * len(pairs)]
    for aof, pipeline, test in tests:
        lines.append(f"| {aof} | {pipeline} | {test} | " + " | ".join(
            paired_cell(runs, aof, pipeline, test, a, b) for a, b in pairs) + " |")

    def table(title, explanation, cell):
        nonlocal lines
        lines += ["", f"## {title}", "", explanation, "",
                  "| AOF | Pipeline | Test | " + " | ".join(labels) + " |",
                  "|---|---:|---|" + "---:|" * len(labels)]
        for aof, pipeline, test in tests:
            lines.append(f"| {aof} | {pipeline} | {test} | " + " | ".join(
                cell((aof, pipeline, test, label)) for label in labels) + " |")

    table("Server CPU per request",
          "Median of the rounds, in µs: server CPU time divided by requests. This is the "
          "measure to compare when the server is the bottleneck (CPU near 100%, next table).",
          lambda key: f"{statistics.median(v['cpu'] / 100 / v['rps'] * 1e6 for v in runs[key].values()):.2f}")
    table("Server CPU", "Median of the rounds; 100% = one core busy.",
          lambda key: f"{median_of(key, 'cpu'):.0f}%")
    table("p99 latency", "Median of the rounds, in ms.",
          lambda key: f"{median_of(key, 'p99'):.1f}")

    args.out.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
