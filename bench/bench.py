#!/usr/bin/env python3
"""Throughput and latency of Mini-Redis, measured the same way against real Redis.

For each server, AOF setting and pipeline depth, one run:
  1. starts a fresh server in an empty temporary directory (no old AOF to replay),
  2. pins the server and redis-benchmark to different CPUs,
  3. runs SET, then GET against the keys SET just wrote,
  4. records requests/s, latency percentiles and the server's CPU use,
  5. checks the server is still alive, then stops it.

Every combination runs --runs times (default 3) and the run with the median
throughput is reported, with the spread of all runs. Runs are interleaved (all
combinations once, then all again), so slow drift such as thermal throttling
hits every combination alike instead of whichever ran last.

Server CPU shows whether the result measures the server at all: near 100% means
the server was the bottleneck; well below means the client or the network was.

Usage (from the repository root, on Linux):
  python3 bench/bench.py --out bench/results/<name>
"""

import argparse
import csv
import io
import resource
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import benchlib as bl

FIELDS = ["run", "server", "aof", "pipeline", "test", "rps", "avg_ms", "min_ms", "p50_ms",
          "p95_ms", "p99_ms", "max_ms", "seconds", "server_cpu_pct", "kernel_pct",
          "client_cpu_pct", "aof_bytes"]
SERVER_NAMES = {"mini": "Mini-Redis", "redis": "Redis"}


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--out", required=True, type=Path, help="results directory")
    p.add_argument("--servers", default="mini,redis", help="mini, redis or both (default)")
    p.add_argument("--runs", type=int, default=3)
    p.add_argument("--requests", type=int, default=1_000_000, help="per unpipelined test (-n)")
    # With --threads, redis-benchmark notices that a run has finished only on its
    # 250 ms timer, so every run time is rounded up to the next 250 ms: an error
    # of up to 250 ms / run time. Pipelined runs are fast, so they get more
    # requests (20-60 s at this machine's speed, an error under ~1%).
    p.add_argument("--pipelined-requests", type=int, default=5_000_000,
                   help="per pipelined test (-n)")
    p.add_argument("--keyspace", type=int, default=100_000, help="random keys (-r)")
    p.add_argument("--clients", type=int, default=50, help="connections (-c)")
    p.add_argument("--pipelines", default="1,16", help="pipeline depths (-P)")
    p.add_argument("--aof", default="on,off", help="AOF settings to measure")
    p.add_argument("--appendfsync", default="no", choices=["always", "everysec", "no"],
                   help="when both servers sync the AOF to disk (default no)")
    p.add_argument("--server-cpu", default="2", help="CPU list for the server (taskset)")
    # redis-benchmark does about as much work per request as the server (one read and
    # one write system call), so one client thread can run out of CPU first. Two give
    # it headroom; the Client CPU column shows whether that was enough.
    p.add_argument("--client-cpus", default="4,6", help="CPU list for redis-benchmark")
    p.add_argument("--client-threads", type=int, default=2, help="redis-benchmark --threads")
    p.add_argument("--build-dir", type=Path, default=bl.REPO_ROOT / "build-bench")
    p.add_argument("--redis-server", default="redis-server")
    p.add_argument("--redis-benchmark", default="redis-benchmark")
    args = p.parse_args()
    args.servers = args.servers.split(",")
    args.pipelines = [int(x) for x in args.pipelines.split(",")]
    args.aof = [x == "on" for x in args.aof.split(",")]
    if set(args.servers) - set(SERVER_NAMES) or args.runs < 1:
        p.error("--servers takes mini and/or redis; --runs must be at least 1")
    return args


def show(argv):
    """A command line for the results file, with the AOF setting left as a placeholder."""
    argv = list(argv)
    if "--appendonly" in argv:
        argv[argv.index("--appendonly") + 1] = "<yes|no>"
    return " ".join(a if a else '""' for a in argv)


def requests_for(args, pipeline):
    return args.requests if pipeline == 1 else args.pipelined_requests


def run_benchmark(args, port, test, pipeline):
    requests = requests_for(args, pipeline)
    argv = [args.redis_benchmark, "-h", "127.0.0.1", "-p", str(port), "-t", test,
            "-n", str(requests), "-r", str(args.keyspace), "-c", str(args.clients),
            "-P", str(pipeline), "--csv"]
    if args.client_threads > 1:
        argv += ["--threads", str(args.client_threads)]
    out = subprocess.run(bl.pinned(args.client_cpus, argv), capture_output=True, text=True,
                         timeout=max(600, requests // 500))
    csv_lines = [line for line in out.stdout.splitlines() if line.startswith('"')]
    rows = [r for r in csv.DictReader(io.StringIO("\n".join(csv_lines)))
            if r.get("test") == test.upper()]
    if out.returncode != 0 or len(rows) != 1:
        raise RuntimeError(f"redis-benchmark failed ({bl.describe_exit(out.returncode)}):\n"
                           f"{out.stdout[-2000:]}\n{out.stderr[-2000:]}")
    return rows[0]


def aof_bytes(workdir):
    return sum(f.stat().st_size for f in Path(workdir).rglob("*")
               if f.is_file() and ".aof" in f.name)


def measure(args, binaries, run, server, aof, pipeline):
    """One run: fresh server, SET then GET. Returns one row per test."""
    rows = []
    with tempfile.TemporaryDirectory(prefix="mini-redis-bench-") as workdir:
        port = bl.free_port()
        if server == "mini":
            # Twice the keyspace, so SET never evicts.
            argv = bl.mini_redis_argv(binaries["mini"], port, workdir, aof, 2 * args.keyspace,
                                      args.appendfsync)
        else:
            argv = bl.redis_argv(binaries["redis"], port, workdir, aof, args.appendfsync)
        with bl.Server(argv, workdir, port, cpus=args.server_cpu) as srv:
            srv.wait_ready()
            for test in ("set", "get"):
                user0, sys0 = srv.cpu_times()
                # redis-benchmark is the only child that exits during this window,
                # so the change in RUSAGE_CHILDREN is exactly its CPU time.
                client0 = resource.getrusage(resource.RUSAGE_CHILDREN)
                start = time.monotonic()
                try:
                    result = run_benchmark(args, port, test, pipeline)
                except (RuntimeError, subprocess.TimeoutExpired) as e:
                    raise RuntimeError(f"{e}\n{SERVER_NAMES[server]} "
                                       f"{bl.describe_exit(srv.proc.poll())}; its output:\n"
                                       f"{srv.log_tail()}") from None
                seconds = time.monotonic() - start
                client1 = resource.getrusage(resource.RUSAGE_CHILDREN)
                user1, sys1 = srv.cpu_times()
                client_cpu = (client1.ru_utime - client0.ru_utime) + (client1.ru_stime - client0.ru_stime)
                if not srv.alive():
                    raise RuntimeError(f"{SERVER_NAMES[server]} "
                                       f"{bl.describe_exit(srv.proc.returncode)} during {test}:\n"
                                       f"{srv.log_tail()}")
                cpu = (user1 - user0) + (sys1 - sys0)
                rows.append({
                    "run": run, "server": server, "aof": "on" if aof else "off",
                    "pipeline": pipeline, "test": test.upper(),
                    "rps": float(result["rps"]), "avg_ms": float(result["avg_latency_ms"]),
                    "min_ms": float(result["min_latency_ms"]),
                    "p50_ms": float(result["p50_latency_ms"]),
                    "p95_ms": float(result["p95_latency_ms"]),
                    "p99_ms": float(result["p99_latency_ms"]),
                    "max_ms": float(result["max_latency_ms"]),
                    "seconds": round(seconds, 2),
                    "server_cpu_pct": round(100 * cpu / seconds, 1),
                    "kernel_pct": round(100 * (sys1 - sys0) / cpu, 1) if cpu else 0.0,
                    "client_cpu_pct": round(100 * client_cpu / seconds, 1),
                    "aof_bytes": aof_bytes(workdir),
                })
    return rows


def summarise(rows, args):
    """One line per (server, AOF, pipeline, test): the median-throughput run, plus spread."""
    groups = {}
    for r in rows:
        groups.setdefault((r["aof"], r["pipeline"], r["test"], r["server"]), []).append(r)
    summary = []
    for key in sorted(groups, key=lambda k: (k[0] == "off", k[1], k[2] != "SET", k[3] != "mini")):
        runs = sorted(groups[key], key=lambda r: r["rps"])
        mid = runs[(len(runs) - 1) // 2]
        rps = [r["rps"] for r in runs]
        summary.append({**mid, "median_rps": statistics.median(rps), "min_rps": min(rps),
                        "max_rps": max(rps), "n": len(runs)})
    return summary


def markdown(summary, args, env_rows, commands):
    def ms(x):
        return f"{x:.3f}" if x < 1 else f"{x:.1f}"

    out = ["# Throughput and latency", "",
           bl.environment_markdown(env_rows), "",
           f"- **Load:** `-n {args.requests:,}` requests per test without pipelining and "
           f"`-n {args.pipelined_requests:,}` with it (redis-benchmark `--threads` rounds each "
           "run time up to its 250 ms timer, so fast runs need to be long), "
           f"`-r {args.keyspace:,}` random keys, `-c {args.clients}` clients, 3-byte values; "
           "SET then GET on the same fresh server.",
           f"- **Pinning:** server on CPU {args.server_cpu}, redis-benchmark on CPU "
           f"{args.client_cpus} ({args.client_threads} thread"
           f"{'s' if args.client_threads > 1 else ''}).",
           f"- **Runs:** {args.runs} per row, interleaved, with the server measured first "
           "alternating from run to run. The row shows the run with the "
           "median throughput; *spread* is (max - min) / median over all runs.",
           "- **Server CPU:** 100% = one core fully busy. *In kernel* is the share of that "
           "CPU time spent in system calls (`stime` / (`utime` + `stime`)).",
           f"- **Client CPU:** redis-benchmark's CPU use ({100 * args.client_threads}% = all "
           f"{args.client_threads} thread{'s' if args.client_threads > 1 else ''} busy). If the "
           "client is near its limit while the server is not, the row measures the client.",
           "", "Commands (port and directory differ per run):", "", "```"]
    out += commands
    out += ["```", "",
            "| Server | AOF | Pipeline | Test | Requests/s | Spread | p50 ms | p99 ms | "
            "max ms | Server CPU | In kernel | Client CPU |",
            "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for s in summary:
        spread = 100 * (s["max_rps"] - s["min_rps"]) / s["median_rps"]
        out.append(f"| {SERVER_NAMES[s['server']]} | {s['aof']} | {s['pipeline']} | "
                   f"{s['test']} | {s['median_rps']:,.0f} | {spread:.1f}% | {ms(s['p50_ms'])} | "
                   f"{ms(s['p99_ms'])} | {ms(s['max_ms'])} | {s['server_cpu_pct']:.0f}% | "
                   f"{s['kernel_pct']:.0f}% | {s['client_cpu_pct']:.0f}% |")

    by_key = {(s["server"], s["aof"], s["pipeline"], s["test"]): s for s in summary}
    pairs = [(k[1:], by_key[k], by_key.get(("redis",) + k[1:]))
             for k in by_key if k[0] == "mini" and ("redis",) + k[1:] in by_key]
    if pairs:
        out += ["", "## Mini-Redis relative to Redis", "",
                "| AOF | Pipeline | Test | Mini-Redis req/s | Redis req/s | Mini-Redis / Redis |",
                "|---|---:|---|---:|---:|---:|"]
        for (aof, pipeline, test), mini, redis in pairs:
            out.append(f"| {aof} | {pipeline} | {test} | {mini['median_rps']:,.0f} | "
                       f"{redis['median_rps']:,.0f} | "
                       f"{mini['median_rps'] / redis['median_rps']:.2f}x |")
    return "\n".join(out) + "\n"


def main():
    args = parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    runs_csv = args.out / "throughput-runs.csv"
    if runs_csv.exists():
        sys.exit(f"{runs_csv} already exists; pick another --out or delete it")

    binaries = {}
    if "mini" in args.servers:
        args.build_dir = bl.build(args.build_dir, ["mini_redis_server"])
        bl.check_optimised(args.build_dir)
        binaries["mini"] = args.build_dir / "mini_redis_server"
    if "redis" in args.servers:
        binaries["redis"] = args.redis_server
    env_rows = bl.environment(args.build_dir if "mini" in args.servers else None,
                              tools=["redis-server", "redis-benchmark", "taskset"])

    for cpu in bl.cpu_list(args.client_cpus):
        if cpu in bl.smt_siblings(args.server_cpu):
            print(f"warning: client CPU {cpu} shares a physical core with the server's "
                  f"CPU {args.server_cpu}", file=sys.stderr)

    # The exact command lines, with placeholders for what changes per run.
    commands = []
    example = Path("<tmp>")
    if "mini" in args.servers:
        commands.append(show(bl.pinned(args.server_cpu, bl.mini_redis_argv(
            "build-bench/mini_redis_server", "<port>", example, True, 2 * args.keyspace,
            args.appendfsync))))
    if "redis" in args.servers:
        commands.append(show(bl.pinned(args.server_cpu, bl.redis_argv(
            "redis-server", "<port>", example, True, args.appendfsync))))
    commands.append(show(bl.pinned(args.client_cpus, [
        "redis-benchmark", "-h", "127.0.0.1", "-p", "<port>", "-t", "<set|get>",
        "-n", "<requests>", "-r", str(args.keyspace), "-c", str(args.clients),
        "-P", "<pipeline>", "--csv"] + (["--threads", str(args.client_threads)]
                                        if args.client_threads > 1 else []))))

    rows = []
    with open(runs_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writeheader()
        for run in range(1, args.runs + 1):
            # Which server goes first alternates from run to run: anything that
            # drifts within a run (a CPU's boost budget running out, say) must
            # not always favour the same one. (Until Phase 4, Mini-Redis always
            # went first.)
            servers = args.servers if run % 2 else list(reversed(args.servers))
            cases = [(aof, p, s) for aof in args.aof for p in args.pipelines for s in servers]
            for aof, pipeline, server in cases:
                new = measure(args, binaries, run, server, aof, pipeline)
                for r in new:
                    writer.writerow(r)
                f.flush()
                rows += new
                print(f"[run {run}/{args.runs}] {SERVER_NAMES[server]:10} "
                      f"AOF {'on ' if aof else 'off'} P={pipeline:<3} " +
                      "  ".join(f"{r['test']} {r['rps']:>10,.0f} req/s "
                                f"(CPU {r['server_cpu_pct']:.0f}%)" for r in new),
                      file=sys.stderr, flush=True)

    report = markdown(summarise(rows, args), args, env_rows, commands)
    (args.out / "throughput.md").write_text(report)
    print(report)


if __name__ == "__main__":
    main()
