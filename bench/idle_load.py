#!/usr/bin/env python3
"""One busy client's latency while many others sit idle: select() against epoll.

select() rebuilds and scans the sets of every watched socket on each wakeup, so
its cost grows with the number of connections, idle ones included; epoll returns
only the sockets that are ready. This holds N idle connections open to
Kosh, then times one busy client (redis-benchmark -c 1, GET, no
pipelining) against each poller (--poller), in interleaved rounds. It reports
requests/s, p50 and p99 latency, and the server's CPU time per request.

Usage (Linux):
  python3 bench/idle_load.py --out bench/results/<name>/idle-load.md
"""

import argparse
import csv
import io
import resource
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

import benchlib as bl
from idle_clients import open_connections


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--out", required=True, type=Path, help="Markdown file to write")
    p.add_argument("--idle", default="0,1000", help="idle connections to hold, per case")
    p.add_argument("--pollers", default="select,epoll")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--requests", type=int, default=100_000, help="GETs by the busy client")
    p.add_argument("--keyspace", type=int, default=100_000, help="random keys (-r)")
    p.add_argument("--server-cpu", default="2", help="CPU list for the server (taskset)")
    p.add_argument("--client-cpus", default="4", help="CPU list for redis-benchmark")
    p.add_argument("--build-dir", type=Path, default=bl.REPO_ROOT / "build-bench")
    p.add_argument("--redis-benchmark", default="redis-benchmark")
    args = p.parse_args()
    args.idle = [int(n) for n in args.idle.split(",")]
    args.pollers = args.pollers.split(",")
    return args


def busy_client(args, port):
    """One redis-benchmark client, GET without pipelining: its CSV row."""
    argv = [args.redis_benchmark, "-h", "127.0.0.1", "-p", str(port), "-t", "get",
            "-n", str(args.requests), "-r", str(args.keyspace), "-c", "1", "--csv"]
    out = subprocess.run(bl.pinned(args.client_cpus, argv), capture_output=True, text=True,
                         timeout=600)
    rows = [r for r in csv.DictReader(io.StringIO("\n".join(
        line for line in out.stdout.splitlines() if line.startswith('"')))) if r.get("test") == "GET"]
    if out.returncode != 0 or len(rows) != 1:
        raise RuntimeError(f"redis-benchmark failed ({bl.describe_exit(out.returncode)}):\n"
                           f"{out.stdout[-2000:]}\n{out.stderr[-2000:]}")
    return rows[0]


def measure(args, binary, poller, idle):
    with tempfile.TemporaryDirectory(prefix="kosh-idle-load-") as workdir:
        port = bl.free_port()
        argv = bl.kosh_argv(binary, port, workdir, False, 2 * args.keyspace) + ["--poller", poller]
        with bl.Server(argv, workdir, port, cpus=args.server_cpu) as srv:
            srv.wait_ready()
            sockets, opened, error, _ = open_connections(port, idle)
            try:
                if error:
                    raise RuntimeError(f"{poller}, {idle} idle: {error}")
                user0, sys0 = srv.cpu_times()
                row = busy_client(args, port)
                user1, sys1 = srv.cpu_times()
            finally:
                for s in sockets:
                    s.close()
    rps = float(row["rps"])
    return {"rps": rps, "p50": float(row["p50_latency_ms"]), "p99": float(row["p99_latency_ms"]),
            "us_per_request": (user1 - user0 + sys1 - sys0) / args.requests * 1e6}


def main():
    args = parse_args()
    if args.out.exists():
        sys.exit(f"{args.out} already exists; pick another --out or delete it")
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = max(args.idle) + 64
    if soft < want:
        resource.setrlimit(resource.RLIMIT_NOFILE, (min(want, hard), hard))

    bl.build(args.build_dir, ["kosh-server"])
    bl.check_optimised(args.build_dir)
    binary = args.build_dir / "kosh-server"
    env = bl.environment(args.build_dir, tools=["redis-benchmark", "taskset"])

    results = {}    # (idle, poller) -> list of runs
    for round_ in range(1, args.rounds + 1):
        for idle in args.idle:
            # Alternate which poller goes first, so neither always runs first
            pollers = args.pollers if round_ % 2 else list(reversed(args.pollers))
            for poller in pollers:
                run = measure(args, binary, poller, idle)
                results.setdefault((idle, poller), []).append(run)
                print(f"[round {round_}/{args.rounds}] {poller:6} {idle:5} idle: "
                      f"{run['rps']:9,.0f} req/s, p50 {run['p50']:.3f} ms, p99 {run['p99']:.3f} ms, "
                      f"{run['us_per_request']:.1f} us CPU/request", flush=True)

    def median(key, field):
        return statistics.median(r[field] for r in results[key])

    lines = ["# One busy client among idle ones: select() against epoll", "",
             bl.environment_markdown(env), "",
             f"- **Server:** `taskset -c {args.server_cpu} build-bench/kosh-server "
             f"--appendonly no --poller <select|epoll>`, with N idle connections open "
             "(each answered one PING, then stayed silent).",
             f"- **Busy client:** `taskset -c {args.client_cpus} redis-benchmark -c 1 -t get "
             f"-n {args.requests:,} -r {args.keyspace:,} --csv` (no pipelining).",
             f"- **Runs:** {args.rounds} round{'s' if args.rounds != 1 else ''}, each measuring "
             "every case with a fresh server, "
             "the order of the pollers alternating. Medians of the rounds; CPU per request "
             "is the server's CPU time over the busy client's requests.", "",
             "| Idle connections | Poller | Requests/s | p50 ms | p99 ms | Server CPU per request |",
             "|---:|---|---:|---:|---:|---:|"]
    for idle in args.idle:
        for poller in args.pollers:
            key = (idle, poller)
            spread = [r["rps"] for r in results[key]]
            lines.append(f"| {idle:,} | {poller} | {median(key, 'rps'):,.0f} "
                         f"({100 * (max(spread) - min(spread)) / median(key, 'rps'):.0f}%) | "
                         f"{median(key, 'p50'):.3f} | {median(key, 'p99'):.3f} | "
                         f"{median(key, 'us_per_request'):.1f} us |")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
