#!/usr/bin/env python3
"""Memory per key: Mini-Redis against real Redis, for the same keys.

Loads N keys shaped like redis-benchmark's (16-byte "key:000000012345", 3-byte
values) into each server over one pipelined connection, and reports how much
the server's resident memory (VmRSS) grew, per key. RSS is the measure both
servers share: it counts the hash table, every allocation, and the
allocator's own headers and fragmentation.

- Mini-Redis sizes its hash table for --maxkeys at startup, so its baseline
  comes from a server started with --maxkeys 1, and the growth includes the
  bucket array. Redis's table grows as keys arrive, so its baseline is the
  empty server itself.
- Redis allocates with jemalloc, Mini-Redis with glibc's malloc. Mini-Redis is
  also measured with jemalloc (LD_PRELOAD), to separate the allocator from the
  data layout.
- No AOF for either, and no snapshots for Redis. Redis's own count of its
  allocations (INFO memory, used_memory) is given too.
- A GET of the last key checks that every SET arrived.

Usage (from the repository root, on Linux):
  python3 bench/memory_compare.py --out bench/results/<name>/memory.md
"""

import argparse
import socket
import statistics
import sys
import tempfile
from pathlib import Path

import benchlib as bl

BATCH = 1000


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--out", required=True, type=Path, help="Markdown file to write")
    p.add_argument("--keys", type=int, default=1_000_000)
    p.add_argument("--runs", type=int, default=3, help="runs per server (median reported)")
    p.add_argument("--redis-server", default="redis-server")
    p.add_argument("--jemalloc", default="/usr/lib/x86_64-linux-gnu/libjemalloc.so.2",
                   help="jemalloc library to preload into Mini-Redis ('' to skip)")
    p.add_argument("--build-dir", type=Path, default=bl.REPO_ROOT / "build-bench")
    args = p.parse_args()
    if args.keys % BATCH:
        p.error(f"--keys must be a multiple of {BATCH}")
    return args


def rss_bytes(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024
    raise RuntimeError(f"no VmRSS for process {pid}")


def key(i):
    return b"key:%012d" % i


def load(port, keys):
    """SETs keys 0 .. keys-1 to "xxx", BATCH commands per write, and checks
    every reply is +OK."""
    expected = b"+OK\r\n" * BATCH
    with socket.create_connection(("127.0.0.1", port), timeout=30) as s:
        for start in range(0, keys, BATCH):
            s.sendall(b"".join(bl.resp(b"SET", key(i), b"xxx") for i in range(start, start + BATCH)))
            got = b""
            while len(got) < len(expected):
                chunk = s.recv(len(expected) - len(got))
                if not chunk:
                    raise ConnectionError("connection closed while loading")
                got += chunk
            if got != expected:
                raise RuntimeError(f"unexpected reply while loading: {got[:80]!r}")
    last = bl.request(port, "GET", key(keys - 1))
    if last != b"xxx":
        raise RuntimeError(f"GET of the last key returned {last!r}")


def used_memory(port):
    info = bl.request(port, "INFO", "memory")
    for line in info.decode().splitlines():
        if line.startswith("used_memory:"):
            return int(line.split(":")[1])
    raise RuntimeError("INFO memory has no used_memory")


def measure_mini(binary, keys, env):
    """RSS growth for `keys` keys: a server sized for them and loaded, against
    one started with --maxkeys 1"""
    with tempfile.TemporaryDirectory(prefix="mini-redis-mem-") as workdir:
        port = bl.free_port()
        with bl.Server(bl.mini_redis_argv(binary, port, workdir, False, 1), workdir, port,
                       env=env) as srv:
            srv.wait_ready()
            base = rss_bytes(srv.proc.pid)
        port = bl.free_port()
        with bl.Server(bl.mini_redis_argv(binary, port, workdir, False, keys), workdir, port,
                       env=env) as srv:
            srv.wait_ready()
            load(port, keys)
            return rss_bytes(srv.proc.pid) - base, None


def measure_redis(binary, keys):
    with tempfile.TemporaryDirectory(prefix="mini-redis-mem-") as workdir:
        port = bl.free_port()
        with bl.Server(bl.redis_argv(binary, port, workdir, False), workdir, port) as srv:
            srv.wait_ready()
            base, base_used = rss_bytes(srv.proc.pid), used_memory(port)
            load(port, keys)
            return rss_bytes(srv.proc.pid) - base, used_memory(port) - base_used


def main():
    args = parse_args()
    if args.out.exists():
        sys.exit(f"{args.out} already exists; pick another --out or delete it")
    args.build_dir = bl.build(args.build_dir, ["mini_redis_server"])
    bl.check_optimised(args.build_dir)
    binary = args.build_dir / "mini_redis_server"

    configs = [("Mini-Redis", "glibc malloc", lambda: measure_mini(binary, args.keys, None))]
    if args.jemalloc:
        if not Path(args.jemalloc).exists():
            sys.exit(f"{args.jemalloc} not found (install libjemalloc2, or pass --jemalloc '')")
        configs.append(("Mini-Redis", "jemalloc (LD_PRELOAD)",
                        lambda: measure_mini(binary, args.keys, {"LD_PRELOAD": args.jemalloc})))
    configs.append(("Redis", "jemalloc (built in)", lambda: measure_redis(args.redis_server, args.keys)))

    results = {i: [] for i in range(len(configs))}
    for run in range(args.runs):
        # Alternate the order, so no server is always measured first
        order = list(range(len(configs)))
        if run % 2:
            order.reverse()
        for i in order:
            results[i].append(configs[i][2]())
        print(f"run {run + 1}/{args.runs} done", file=sys.stderr, flush=True)

    lines = ["# Memory per key", "",
             bl.environment_markdown(bl.environment(args.build_dir, tools=["redis-server"])), "",
             f"- **Keys:** {args.keys:,} keys of 16 bytes (`key:000000012345`) with 3-byte "
             "values, loaded over one connection, 1,000 SETs per write; no AOF.",
             "- **Measure:** growth of the server's resident memory (VmRSS). Mini-Redis's "
             "baseline is a server started with `--maxkeys 1`, so the growth includes the "
             "hash table it sizes at startup; Redis's is the empty server.",
             f"- **Runs:** {args.runs}, the order alternating; median, with every run's "
             "value in brackets.", "",
             "| Server | Allocator | RSS growth | Bytes per key | Redis's `used_memory` per key |",
             "|---|---|---:|---:|---:|"]
    for i, (server, allocator, _) in enumerate(configs):
        growth = [g for g, _ in results[i]]
        per_key = [g / args.keys for g in growth]
        used = [u / args.keys for _, u in results[i] if u is not None]
        lines.append(f"| {server} | {allocator} | {statistics.median(growth) / 1e6:.1f} MB | "
                     f"{statistics.median(per_key):.0f} "
                     f"({', '.join(f'{v:.0f}' for v in per_key)}) | "
                     f"{f'{statistics.median(used):.0f}' if used else '-'} |")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
