#!/usr/bin/env python3
"""How long Kosh takes to start from an AOF of a given size.

For each size it writes an AOF of that many SET commands and times the server
from launch until it answers PING. Kosh replays the AOF before it starts
listening, so that time is replay plus a fixed startup cost (the 0-command row).

The generated files are byte-for-byte what Kosh itself writes for
redis-benchmark's SETs (16-byte keys, 3-byte values); every run checks this
first against a live server. Each value encodes its command number, so a GET
after startup proves replay reached the end of the file.

The growth exponent compares neighbouring sizes: log(time ratio) / log(size
ratio). About 1 means replay time grows linearly with the file, about 2 means
quadratically.

With --server redis it times real Redis loading the same files instead, for
comparison (written to replay-redis.md).

Usage (from the repository root, on Linux):
  python3 bench/replay_bench.py --out bench/results/<name>
  python3 bench/replay_bench.py --out bench/results/<name> --server redis
"""

import argparse
import math
import random
import shutil
import statistics
import sys
import tempfile
from pathlib import Path

import benchlib as bl


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--out", required=True, type=Path, help="results directory")
    p.add_argument("--sizes", default="0,25000,50000,100000,200000,400000,1000000",
                   help="numbers of commands in the AOF")
    p.add_argument("--keyspace", type=int, default=100_000, help="distinct random keys")
    p.add_argument("--runs", type=int, default=3, help="runs per size (median reported)")
    p.add_argument("--repeat-limit", type=float, default=60,
                   help="measure a size only once if one run takes longer than this (s)")
    p.add_argument("--cap", type=float, default=900,
                   help="stop a run after this many seconds and report '> cap'")
    p.add_argument("--server-cpu", default="2", help="CPU list for the server (taskset)")
    p.add_argument("--server", default="mini", choices=["mini", "redis"],
                   help="time Kosh (replay.md) or real Redis loading the same files "
                        "(replay-redis.md)")
    p.add_argument("--redis-server", default="redis-server")
    p.add_argument("--build-dir", type=Path, default=bl.REPO_ROOT / "build-bench")
    args = p.parse_args()
    args.sizes = [int(x) for x in args.sizes.split(",")]
    return args


def generate(path, commands, keyspace, seed=1):
    """Writes `commands` SETs of random keys, as Kosh logs them.
    Returns the last key written and its value."""
    rng = random.Random(seed)
    key = value = None
    with open(path, "wb") as f:
        batch = []
        for i in range(commands):
            key = b"key:%012d" % rng.randrange(keyspace)
            value = b"%03d" % (i % 1000)
            batch.append(bl.resp(b"SET", key, value))
            if len(batch) == 10_000:
                f.write(b"".join(batch))
                batch.clear()
        f.write(b"".join(batch))
    return key, value


def check_format(binary, keyspace):
    """Sends SETs to a live server and checks its AOF matches what generate() writes."""
    with tempfile.TemporaryDirectory(prefix="kosh-replay-") as workdir:
        expected = Path(workdir) / "expected.aof"
        generate(expected, 5, keyspace, seed=7)
        rng = random.Random(7)
        port = bl.free_port()
        with bl.Server(bl.kosh_argv(binary, port, workdir, True, 2 * keyspace),
                       workdir, port) as srv:
            srv.wait_ready()
            for i in range(5):
                reply = bl.request(port, "SET", b"key:%012d" % rng.randrange(keyspace),
                                   b"%03d" % i)
                if reply != "+OK":
                    sys.exit(f"SET returned {reply!r} during the format check")
        written = (Path(workdir) / "appendonly.aof").read_bytes()
        if written != expected.read_bytes():
            sys.exit("Generated AOF does not match what the server writes:\n"
                     f"  server:    {written[:120]!r}\n  generated: {expected.read_bytes()[:120]!r}")


def time_startup(args, binary, aof, last):
    """Seconds from launch to first PONG, or None if it exceeded the cap."""
    port = bl.free_port()
    if args.server == "mini":
        workdir = aof.parent
        argv = bl.kosh_argv(binary, port, workdir, True, 2 * args.keyspace)
    else:
        # Redis 7 converts a single-file AOF into its multi-part format when it first
        # loads it, moving the file, so each run starts from a fresh copy.
        workdir = aof.parent / f"redis-run-{port}"
        workdir.mkdir()
        shutil.copyfile(aof, workdir / "appendonly.aof")
        argv = bl.redis_argv(binary, port, workdir, True)
    # Redis answers -LOADING until the file is loaded; ping() treats that as not ready.
    with bl.Server(argv, workdir, port, cpus=args.server_cpu) as srv:
        try:
            seconds = srv.wait_ready(timeout=args.cap, interval=0.005)
        except TimeoutError:
            return None
        if last is not None:
            key, value = last
            got = bl.request(port, "GET", key)
            if got != value:
                sys.exit(f"Replay did not reach the end of the file: GET {key!r} returned "
                         f"{got!r}, expected {value!r}")
    if args.server == "redis":
        shutil.rmtree(workdir)   # this run's copy of the AOF
    return seconds


def fmt_seconds(s):
    return f"{s * 1000:.0f} ms" if s < 1 else f"{s:.2f} s" if s < 100 else f"{s:.0f} s"


def fmt_bytes(n):
    return f"{n / 1e6:.1f} MB" if n >= 1e6 else f"{n / 1e3:.0f} kB" if n else "0 B"


def main():
    args = parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    mini = args.server == "mini"
    report_path = args.out / ("replay.md" if mini else "replay-redis.md")
    if report_path.exists():
        sys.exit(f"{report_path} already exists; pick another --out or delete it")

    if mini:
        args.build_dir = bl.build(args.build_dir, ["kosh-server"])
        bl.check_optimised(args.build_dir)
        binary = args.build_dir / "kosh-server"
        env_rows = bl.environment(args.build_dir)
        check_format(binary, args.keyspace)
    else:
        binary = args.redis_server
        env_rows = bl.environment(tools=["redis-server"])

    results = []   # (commands, aof bytes, median seconds or None, runs)
    with tempfile.TemporaryDirectory(prefix="kosh-replay-") as root:
        for commands in args.sizes:
            workdir = Path(root) / f"aof-{commands}"
            workdir.mkdir()
            aof = workdir / "appendonly.aof"
            last = generate(aof, commands, args.keyspace)
            last = last if commands else None
            times = []
            for _ in range(args.runs):
                t = time_startup(args, binary, aof, last)
                if t is None:
                    times = None
                    break
                times.append(t)
                if t > args.repeat_limit:
                    break
            median = statistics.median(times) if times else None
            results.append((commands, aof.stat().st_size, median, len(times or [1])))
            print(f"{commands:>9,} commands  {fmt_bytes(aof.stat().st_size):>9}  "
                  f"{fmt_seconds(median) if median is not None else f'> {args.cap:.0f} s'}"
                  f"  ({len(times or [1])} run{'s' if len(times or [1]) > 1 else ''})",
                  file=sys.stderr, flush=True)
            aof.unlink()   # keep the temporary directory small

    base = results[0][2] if results and results[0][0] == 0 and results[0][2] else 0.0
    if mini:
        title = "# AOF replay time: Kosh"
        timing = ("- **Time:** from launching the server to its first `PONG`. Kosh "
                  "replays before it listens, so this is replay plus the fixed startup cost in "
                  "the 0-command row.")
        server = (f"`taskset -c {args.server_cpu} build-bench/kosh-server --maxkeys "
                  f"{2 * args.keyspace} --appendonly yes --appendfilename <generated file>`")
    else:
        title = "# AOF load time: real Redis, same files"
        timing = ("- **Time:** from launching the server to its first `PONG`. Redis listens "
                  "while loading and answers `-LOADING` until it is done; that counts as not "
                  "ready. Redis 7 converts the single-file AOF to its multi-part format as it "
                  "loads it, so each run starts from a fresh copy.")
        server = "`taskset -c " + args.server_cpu + " " + " ".join(
            a if a else '""' for a in bl.redis_argv("redis-server", "<port>", "<tmp>", True)) + "`"
    rows = [title, "", bl.environment_markdown(env_rows), "",
            f"- **AOF contents:** N `SET` commands; keys drawn at random from "
            f"{args.keyspace:,} (`key:000000012345`), 3-byte values. Byte-for-byte what "
            "Kosh writes for redis-benchmark's SETs (checked against a live Kosh "
            "when measuring Kosh).",
            timing + " The AOF has just been written, so it is read from the OS page cache, "
            "not disk.",
            "- **Check:** after each start, `GET` of the last key written must return its last "
            "value, so the whole file was really loaded.",
            f"- **Runs:** median of {args.runs}; sizes where one run took over "
            f"{args.repeat_limit:.0f} s were run once; runs are stopped after "
            f"{args.cap:.0f} s.",
            f"- **Server:** {server}",
            "- **Growth exponent:** log(time ratio) / log(size ratio) against the previous "
            "row, after subtracting the 0-command startup time. 1 = linear, 2 = quadratic.",
            "", "| Commands | AOF size | Startup time | Runs | Per command | Growth exponent |",
            "|---:|---:|---:|---:|---:|---:|"]
    prev = None
    for commands, size, t, runs in results:
        if t is None:
            time_text, per_cmd = f"> {args.cap:.0f} s", "–"
            replay = args.cap - base
        else:
            time_text = fmt_seconds(t)
            replay = t - base
            per_cmd = f"{replay / commands * 1e6:.1f} µs" if commands else "–"
        exponent = "–"
        if prev and commands and prev[0] and prev[1] > 0 and replay > 0:
            k = math.log(replay / prev[1]) / math.log(commands / prev[0])
            exponent = f"{'≥ ' if t is None else ''}{k:.2f}"
        rows.append(f"| {commands:,} | {fmt_bytes(size)} | {time_text} | {runs} | "
                    f"{per_cmd} | {exponent} |")
        prev = (commands, replay)
    report = "\n".join(rows) + "\n"
    report_path.write_text(report)
    print(report)


if __name__ == "__main__":
    main()
