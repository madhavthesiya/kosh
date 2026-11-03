#!/usr/bin/env python3
"""Where Mini-Redis spends its CPU time under load: perf samples and flame graphs.

For each workload it starts a fresh server, runs redis-benchmark in a loop
against it, waits for the load to settle, then samples the server with
`perf record` for a fixed time. It writes:
  <workload>.svg       flame graph (open the file in a browser to zoom in)
  <workload>-top.txt   CPU time by binary/library and by function
  profile.md           a summary with the commands used

Kernel frames are included, which matters here: most of a small request's cost
is system calls. That needs, as root:
  sysctl -w kernel.perf_event_paranoid=1 kernel.kptr_restrict=0
Ubuntu's /etc/sysctl.d/10-kernel-hardening.conf sets kptr_restrict back to 1
whenever systemd starts, which under WSL2 includes the distribution restarting
after being idle, so apply it right before profiling.

Stacks are unwound with DWARF debug info (--call-graph dwarf), because the
binary is built without frame pointers. The profiled binary is the same
RelWithDebInfo build that bench.py measures.

Usage (from the repository root, on Linux):
  python3 bench/cpu_profile.py --out bench/results/<name>/profile
"""

import argparse
import glob
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import benchlib as bl

WORKLOADS = {   # name: (redis-benchmark test, pipeline depth)
    "set-p1": ("set", 1),
    "set-p16": ("set", 16),
    "get-p16": ("get", 16),
}

# Share of all samples with this frame anywhere on the stack (inclusive time),
# by the names stackcollapse-perf.pl prints (argument lists already removed).
INCLUSIVE = [
    ("send()", {"__send", "__libc_send", "send"}),
    ("write(), the AOF", {"__GI___libc_write", "__libc_write", "write"}),
    ("recv()", {"__libc_recv", "__recv", "recv"}),
    ("Waiting: select() or epoll_wait()", {"__select", "___select", "select", "epoll_wait",
                                           "__epoll_wait", "__GI_epoll_wait", "epoll_pwait"}),
    ("All AOF logging (InMemoryStorage::logAOF)", {"InMemoryStorage::logAOF"}),
    # parse() before Phase 4; parseInto() and its helpers since
    ("RESP parsing", {"RespParser::parse", "RespParser::parseResp", "RespParser::parseInline",
                      "RespParser::parseInto", "RespParser::parseRespInto",
                      "RespParser::parseInlineInto"}),
]


def inclusive_shares(folded):
    """[(label, percent of samples)] for INCLUSIVE, counting each stack once."""
    total, hits = 0, [0] * len(INCLUSIVE)
    for line in folded.decode(errors="replace").splitlines():
        stack, _, count = line.rpartition(" ")
        if not count.isdigit():
            continue
        frames = set(stack.split(";"))
        total += int(count)
        for i, (_, names) in enumerate(INCLUSIVE):
            if frames & names:
                hits[i] += int(count)
    return [(label, 100 * h / total if total else 0.0)
            for (label, _), h in zip(INCLUSIVE, hits)]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--out", required=True, type=Path, help="output directory")
    p.add_argument("--workloads", default=",".join(WORKLOADS))
    p.add_argument("--aof", default="on", choices=["on", "off"])
    p.add_argument("--seconds", type=float, default=10, help="sampling time per workload")
    p.add_argument("--freq", type=int, default=499, help="samples per second")
    p.add_argument("--keyspace", type=int, default=100_000)
    p.add_argument("--clients", type=int, default=50)
    p.add_argument("--server-cpu", default="2")
    p.add_argument("--client-cpus", default="4,6", help="same default as bench.py")
    p.add_argument("--client-threads", type=int, default=2, help="redis-benchmark --threads")
    p.add_argument("--perf", help="perf binary (default: first one that works)")
    p.add_argument("--flamegraph", type=Path, default=Path.home() / "tools" / "FlameGraph",
                   help="checkout of https://github.com/brendangregg/FlameGraph")
    p.add_argument("--build-dir", type=Path, default=bl.REPO_ROOT / "build-bench")
    args = p.parse_args()
    args.workloads = args.workloads.split(",")
    unknown = set(args.workloads) - set(WORKLOADS)
    if unknown:
        p.error(f"unknown workloads {sorted(unknown)}; choose from {sorted(WORKLOADS)}")
    return args


def find_perf(explicit):
    """Ubuntu's /usr/bin/perf is a wrapper that refuses kernels it has no package
    for (such as WSL2's); the versioned binaries under /usr/lib/linux-tools work."""
    candidates = [explicit] if explicit else \
        [shutil.which("perf")] + sorted(glob.glob("/usr/lib/linux-tools/*/perf"), reverse=True)
    for perf in filter(None, candidates):
        if subprocess.run([perf, "--version"], capture_output=True).returncode == 0:
            return perf
    sys.exit("perf not found: install linux-tools-generic (Ubuntu) or pass --perf")


def check_kernel_settings():
    paranoid = int(Path("/proc/sys/kernel/perf_event_paranoid").read_text())
    kptr = int(Path("/proc/sys/kernel/kptr_restrict").read_text())
    if paranoid > 1 or kptr != 0:
        sys.exit(f"perf_event_paranoid={paranoid}, kptr_restrict={kptr}: kernel time would be "
                 "missing or unnamed. As root, run:\n"
                 "  sysctl -w kernel.perf_event_paranoid=1 kernel.kptr_restrict=0")


STD_STRING = "std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >"


def short_name(symbol):
    """A C++ symbol shortened for reading: std::string spelled as such, and
    template arguments and parameter lists elided, e.g.
    std::_Hashtable<…>::_M_find_before_node(…). Full names are in the flame graph."""
    symbol = symbol.replace(STD_STRING, "std::string")
    out, depth = [], 0
    for ch in symbol:
        if ch in "<(":
            if depth == 0:
                out.append(ch + "…")
            depth += 1
        elif ch in ">)":
            depth -= 1
            if depth == 0:
                out.append(ch)
            elif depth < 0:   # e.g. operator>: leave the name as it is
                return symbol
        elif depth == 0:
            out.append(ch)
    return "".join(out) if depth == 0 else symbol


def report_line(line):
    """One row of `perf report --stdio` without its padding, or None for headers."""
    if not line.strip() or line.startswith("#"):
        return None
    parts = line.split(None, 3)   # overhead, dso[, [.] or [k], symbol ...]
    if len(parts) < 4:
        return f"{parts[0]:>8}  {' '.join(parts[1:])}"
    symbol = re.sub(r"(\s+-)+\s*$", "", parts[3])   # perf pads with empty "-" columns
    return f"{parts[0]:>8}  {parts[1]:<20} {parts[2]} {short_name(symbol)}"


def benchmark_argv(args, port, test, pipeline, loop):
    argv = ["redis-benchmark", "-h", "127.0.0.1", "-p", str(port), "-t", test,
            "-r", str(args.keyspace), "-c", str(args.clients), "-P", str(pipeline), "-q"]
    argv += ["-l"] if loop else ["-n", str(10 * args.keyspace)]
    if args.client_threads > 1:
        argv += ["--threads", str(args.client_threads)]
    return bl.pinned(args.client_cpus, argv)


def profile(args, perf, binary, name, outdir):
    test, pipeline = WORKLOADS[name]
    with tempfile.TemporaryDirectory(prefix="mini-redis-profile-") as workdir:
        port = bl.free_port()
        argv = bl.mini_redis_argv(binary, port, workdir, args.aof == "on", 2 * args.keyspace)
        with bl.Server(argv, workdir, port, cpus=args.server_cpu) as srv:
            srv.wait_ready()
            if test == "get":   # fill the keyspace first, so GETs find their keys
                subprocess.run(benchmark_argv(args, port, "set", 16, loop=False),
                               check=True, stdout=subprocess.DEVNULL)
            load = subprocess.Popen(benchmark_argv(args, port, test, pipeline, loop=True),
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                time.sleep(2)   # let connections open and the load settle
                data = Path(workdir) / "perf.data"
                user0, sys0 = srv.cpu_times()
                t0 = time.monotonic()
                subprocess.run([perf, "record", "-e", "cpu-clock", "-F", str(args.freq),
                                "--call-graph", "dwarf,16384", "-p", str(srv.proc.pid),
                                "-o", str(data), "--", "sleep", str(args.seconds)],
                               check=True, capture_output=True)
                seconds = time.monotonic() - t0
                user1, sys1 = srv.cpu_times()
                if not srv.alive():
                    sys.exit(f"server {bl.describe_exit(srv.proc.returncode)} while profiling:\n"
                             f"{srv.log_tail()}")
            finally:
                load.terminate()
                load.wait()

        cpu = (user1 - user0) + (sys1 - sys0)
        stats = {"cpu_pct": 100 * cpu / seconds,
                 "kernel_pct": 100 * (sys1 - sys0) / cpu if cpu else 0.0}

        # Flame graph: perf script -> folded stacks -> SVG
        script = subprocess.run([perf, "script", "-i", str(data), "--no-inline"],
                                check=True, capture_output=True).stdout
        folded = subprocess.run([str(args.flamegraph / "stackcollapse-perf.pl")], input=script,
                                check=True, capture_output=True).stdout
        title = (f"Mini-Redis {test.upper()} P={pipeline}, AOF {args.aof}: "
                 f"{args.clients} clients, {args.keyspace:,} keys")
        svg = subprocess.run([str(args.flamegraph / "flamegraph.pl"), "--title", title,
                              "--subtitle", f"commit {bl.git_commit_id()}, "
                              f"{args.freq} Hz cpu-clock samples, kernel frames included",
                              "--width", "1400"],
                             input=folded, check=True, capture_output=True).stdout
        (outdir / f"{name}.svg").write_bytes(svg)

        # Tables: by binary/library, then by function (self time, no children)
        report = []
        for sort, limit in (("dso", "0.5"), ("dso,symbol", "1")):
            out = subprocess.run([perf, "report", "-i", str(data), "--stdio", "--no-children",
                                  "--no-inline", "-g", "none", "--sort", sort,
                                  "--percent-limit", limit],
                                 check=True, capture_output=True, text=True).stdout
            report.append("\n".join(filter(None, (report_line(line)
                                                  for line in out.splitlines()))))
        stats["inclusive"] = inclusive_shares(folded)
        inclusive = "\n".join(f"  {pct:6.2f}%  {label}" for label, pct in stats["inclusive"])
        (outdir / f"{name}-top.txt").write_text(
            f"{title}\nServer CPU {stats['cpu_pct']:.0f}%, of which {stats['kernel_pct']:.0f}% "
            f"in the kernel\n\n== Inclusive time (the call and everything under it)\n"
            f"{inclusive}\n\n== Self time by binary or library\n{report[0]}\n\n"
            f"== Self time by function (at least 1%)\n{report[1]}\n")
        return stats


def main():
    args = parse_args()
    perf = find_perf(args.perf)
    check_kernel_settings()
    for tool in ("stackcollapse-perf.pl", "flamegraph.pl"):
        if not (args.flamegraph / tool).exists():
            sys.exit(f"{args.flamegraph / tool} not found. Get it with:\n"
                     f"  git clone https://github.com/brendangregg/FlameGraph {args.flamegraph}")
    args.out.mkdir(parents=True, exist_ok=True)

    args.build_dir = bl.build(args.build_dir, ["mini_redis_server"])
    bl.check_optimised(args.build_dir)
    binary = args.build_dir / "mini_redis_server"
    env_rows = bl.environment(args.build_dir)
    env_rows.append(("perf", subprocess.run([perf, "--version"], capture_output=True,
                                            text=True).stdout.strip()))
    env_rows.append(("FlameGraph", f"`{bl.git_commit_id(args.flamegraph)}`"))

    lines = ["# CPU profile", "", bl.environment_markdown(env_rows), "",
             f"- **Sampling:** `perf record -e cpu-clock -F {args.freq} --call-graph "
             f"dwarf,16384 -p <server pid> -- sleep {args.seconds:g}`, after 2 s of load.",
             f"- **Server:** `taskset -c {args.server_cpu} build-bench/mini_redis_server "
             f"--maxkeys {2 * args.keyspace} --appendonly {'yes' if args.aof == 'on' else 'no'}`",
             f"- **Load:** `taskset -c {args.client_cpus} redis-benchmark -t <test> "
             f"-r {args.keyspace} -c {args.clients} -P <pipeline> -l"
             + (f" --threads {args.client_threads}" if args.client_threads > 1 else "") +
             "` (GET runs on keys written by a SET pass first).",
             "- Throughput under the profiler is not a benchmark result: sampling with stack "
             "copies slows the server down.",
             "- **Server CPU** comes from tick-based accounting (`/proc/<pid>/stat`), which in a "
             "VM can read a few percent above 100% for one thread; read ~100% as saturated.",
             "- **Inclusive** columns: share of samples with that call anywhere on the stack, "
             "i.e. the call and everything it does in the kernel.",
             "- Without hardware counters (as in a VM), perf samples on a timer interrupt, which "
             "cannot fire while the kernel has interrupts disabled. Such time is charged to the "
             "function that re-enables them, so `_raw_spin_unlock_irqrestore` in the tables "
             "stands for kernel work done with interrupts off, not for unlocking itself.",
             ""]
    labels = [label for label, _ in INCLUSIVE]
    table = ["| Workload | Server CPU | In kernel | " + " | ".join(labels) + " | Files |",
             "|---|---:|---:|" + "---:|" * len(labels) + "---|"]
    for name in args.workloads:
        stats = profile(args, perf, binary, name, args.out)
        print(f"{name}: server CPU {stats['cpu_pct']:.0f}%, {stats['kernel_pct']:.0f}% in kernel",
              file=sys.stderr, flush=True)
        table.append(f"| {name} | {stats['cpu_pct']:.0f}% | {stats['kernel_pct']:.0f}% | "
                     + " | ".join(f"{pct:.1f}%" for _, pct in stats["inclusive"])
                     + f" | [flame graph]({name}.svg), [top functions]({name}-top.txt) |")
    lines += table
    (args.out / "profile.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
