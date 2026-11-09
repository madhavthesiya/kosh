#!/usr/bin/env python3
"""Scaling of shard-per-core Kosh against fair baselines, at 1, 2, 4 and 8 cores.

For each core budget:
    kosh        --threads C
    locked      C event loops sharing one locked map (Kosh at commit dd5e505):
                what the shards are measured against
    redis8      Redis 8 with io-threads, at C and at its config file's advice
    valkey      Valkey 8.1 with io-threads, at C and at its config file's advice
    nredis      C separate Redis 8 processes, one per core, each client talking
                to one of them about its own keys (no routing cost: the
                hardest baseline)
and once each kosh-main (before threads) and redis7 (for continuity).

Part 1, peak throughput: memtier, closed loop, pipeline 1 and 16. The rounds
are interleaved and rotated, so that a machine changing speed affects every
server alike. Server CPU per request comes from /proc.

Part 2, latency: kosh-loadgen, open loop, at a common rate and at 50% and 90%
of each server's own peak. It prints a percentile only if it can support it.

Everything is pinned: the servers on CPUs 0 to C-1, the load generators on 8-17.
Each server gets the same cores, keys, values and persistence setting.

Usage: shard_bench.py --out DIR [--rounds 3] [--seconds 8] [--trial]
Binaries are found under --builds and --tools; see the defaults.
"""
import argparse
import json
import os
import pathlib
import signal
import socket
import statistics
import subprocess
import tempfile
import time

HOME = pathlib.Path.home()
CLIENT_CPUS = "8-17"
KEYS = 100000
VALUE_SIZE = 64
BASE_PORT = 7500
CORES = [1, 2, 4, 8]
# What each project's config file suggests for 4 and 8 cores (counting the main thread)
IO_THREADS = {"redis8": {1: [1], 2: [2], 4: [4, 3], 8: [8, 7]},
              "valkey": {1: [1], 2: [2], 4: [4, 3], 8: [8, 6]}}
WORKLOADS = [("GET", "0:1", 1, False), ("SET", "1:0", 1, False),
             ("GET", "0:1", 16, False), ("SET", "1:0", 16, False),
             ("SET", "1:0", 16, True)]     # with the AOF on, everysec


class Paths:
    def __init__(self, builds, tools):
        self.kosh = builds / "final/build/kosh-server"
        self.locked = builds / "locked/build/kosh-server"
        self.main = builds / "main/build/kosh-server"
        self.loadgen = builds / "final/build/kosh-loadgen"
        self.ring = builds / "final/build/ring_bench"
        self.redis8 = tools / "redis-src/8.10.3/src/redis-server"
        self.valkey = tools / "valkey-src/8.1.11/src/valkey-server"
        self.redis7 = pathlib.Path("/usr/bin/redis-server")
        self.memtier = tools / "memtier/install/bin/memtier_benchmark"


def configs():
    all_of_them = []
    for cores in CORES:
        all_of_them.append(("kosh", cores, 0))
        all_of_them.append(("locked", cores, 0))
        for name in ("redis8", "valkey"):
            all_of_them += [(name, cores, io) for io in IO_THREADS[name][cores]]
        if cores > 1:
            all_of_them.append(("nredis", cores, 0))
    return all_of_them + [("kosh-main", 1, 0), ("redis7", 1, 0)]


def label(config):
    name, cores, io = config
    return f"{name}, {cores} core{'s' if cores > 1 else ''}" + (f", io-threads {io}" if io else "")


def ping(port):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
        s.sendall(b"*1\r\n$4\r\nPING\r\n")
        return s.recv(16).startswith(b"+PONG")


def wait_for(port, process):
    deadline = time.time() + 15
    while time.time() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server on port {port} exited with {process.returncode}")
        try:
            if ping(port):
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"server on port {port} didn't answer")


def server_commands(paths, config, aof, work):
    """Each process to start: (argv, cpu list, port)"""
    name, cores, io = config
    cpus = f"0-{cores - 1}"
    persistence = ["--appendonly", "yes" if aof else "no", "--appendfsync", "everysec"]
    if name in ("kosh", "locked", "kosh-main"):
        binary = {"kosh": paths.kosh, "locked": paths.locked, "kosh-main": paths.main}[name]
        argv = [str(binary), "--port", str(BASE_PORT), "--maxkeys", "2000000", *persistence]
        if name != "kosh-main":
            argv += ["--threads", str(cores)]
        return [(argv, cpus, BASE_PORT, work)]
    if name == "nredis":
        result = []
        for i in range(cores):
            directory = work / str(i)
            directory.mkdir()
            argv = [str(paths.redis8), "--port", str(BASE_PORT + i), "--bind", "127.0.0.1", "--save", "",
                    "--dir", str(directory), *persistence]
            result.append((argv, str(i), BASE_PORT + i, directory))
        return result
    binary = {"redis8": paths.redis8, "valkey": paths.valkey, "redis7": paths.redis7}[name]
    argv = [str(binary), "--port", str(BASE_PORT), "--bind", "127.0.0.1", "--save", "", "--dir", str(work), *persistence]
    if io:
        argv += ["--io-threads", str(io)]
    return [(argv, cpus, BASE_PORT, work)]


class Servers:
    def __init__(self, paths, config, aof):
        self.tmp = tempfile.TemporaryDirectory(dir=HOME / "mr-dev")
        work = pathlib.Path(self.tmp.name)
        self.processes, self.ports = [], []
        self.log = open(work / "servers.log", "w")
        for argv, cpus, port, directory in server_commands(paths, config, aof, work):
            process = subprocess.Popen(["taskset", "-c", cpus, *argv], cwd=directory, stdout=self.log, stderr=subprocess.STDOUT)
            self.processes.append(process)
            self.ports.append(port)
        for process, port in zip(self.processes, self.ports):
            wait_for(port, process)

    def cpu_seconds(self):
        ticks = os.sysconf("SC_CLK_TCK")
        total = 0
        for process in self.processes:
            with open(f"/proc/{process.pid}/stat") as f:
                fields = f.read().rsplit(")", 1)[1].split()
            total += int(fields[11]) + int(fields[12])
        return total / ticks

    def stop(self):
        for process in self.processes:
            process.send_signal(signal.SIGTERM)
        for process in self.processes:
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        self.log.close()
        self.tmp.cleanup()


def memtier(paths, port, keys, *args):
    return subprocess.Popen(["taskset", "-c", CLIENT_CPUS, str(paths.memtier), "-s", "127.0.0.1", "-p", str(port),
                             "--hide-histogram", "-d", str(VALUE_SIZE), f"--key-maximum={keys}", *args],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def totals(output):
    lines = [l for l in output.replace("\r", "\n").split("\n") if l.startswith("Totals")]
    if not lines:
        raise RuntimeError("memtier printed no totals: " + output[-300:])
    return float(lines[-1].split()[1])


def peak_cell(paths, config, workload, seconds):
    _, ratio, pipeline, aof = workload
    servers = Servers(paths, config, aof)
    try:
        count = len(servers.ports)
        keys = KEYS // count
        for fill in [memtier(paths, port, keys, "-t", "1", "-c", "1", "--pipeline=64", "--ratio=1:0", "--key-pattern=P:P",
                             "-n", "allkeys") for port in servers.ports]:
            fill.communicate(timeout=120)
        cpu_before = servers.cpu_seconds()
        # 48 connections in all, whatever the number of instances
        threads = max(1, 8 // count)
        runs = [memtier(paths, port, keys, "-t", str(threads), "-c", "6", f"--pipeline={pipeline}", f"--ratio={ratio}",
                        "--key-pattern=R:R", f"--test-time={seconds}", "--distinct-client-seed") for port in servers.ports]
        ops = sum(totals(run.communicate(timeout=seconds + 60)[0]) for run in runs)
        cpu = servers.cpu_seconds() - cpu_before
        return {"ops": ops, "cpu_us_per_op": cpu * 1e6 / (ops * seconds) if ops else None}
    finally:
        servers.stop()


def open_loop(paths, config, rate, seconds, out):
    servers = Servers(paths, config, False)
    try:
        json_path = out / f"open-{config[0]}-{config[1]}-{config[2]}-{rate}.json"
        ports = ",".join(str(p) for p in servers.ports)
        done = subprocess.run(["taskset", "-c", CLIENT_CPUS, str(paths.loadgen), "--ports", ports, "--rate", str(rate),
                               "--connections", "32", "--threads", "2", "--duration", str(seconds), "--warmup", "2",
                               "--keyspace", str(KEYS), "--set-ratio", "0.1", "--json", str(json_path)],
                              capture_output=True, text=True, timeout=seconds + 120)
        return done.stdout
    finally:
        servers.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seconds", type=int, default=8)
    parser.add_argument("--builds", default=str(HOME / "mr-dev/quiet-builds"))
    parser.add_argument("--tools", default=str(HOME / "mr-dev"))
    parser.add_argument("--trial", action="store_true", help="one of everything, briefly, to check that it all runs")
    args = parser.parse_args()
    paths = Paths(pathlib.Path(args.builds), pathlib.Path(args.tools))
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    chosen = configs()
    workloads = WORKLOADS
    if args.trial:
        args.rounds, args.seconds = 1, 2
        wanted = {("kosh", 4, 0), ("locked", 2, 0), ("redis8", 4, 3), ("valkey", 4, 3), ("nredis", 2, 0), ("kosh-main", 1, 0), ("redis7", 1, 0)}
        chosen = [c for c in chosen if c in wanted]
        workloads = [WORKLOADS[0], WORKLOADS[4]]

    cells = [(config, workload) for config in chosen for workload in workloads
             if not (config[0] == "locked" and workload[3])]        # the locked map is compared with the AOF off
    print(f"{len(cells)} cells, {args.rounds} rounds, {args.seconds} s each", flush=True)
    print("load before:", open("/proc/loadavg").read().strip(), flush=True)
    raw = open(out / "peak.jsonl", "w")
    results = {}
    for round_ in range(args.rounds):
        shift = round_ * 7 % len(cells)
        for config, workload in cells[shift:] + cells[:shift]:
            try:
                cell = peak_cell(paths, config, workload, args.seconds)
            except Exception as error:      # one failure mustn't cost the session
                print(f"FAILED {label(config)} {workload}: {error}", flush=True)
                raw.write(json.dumps({"round": round_, "config": config, "workload": workload[:3], "aof": workload[3], "error": str(error)}) + "\n")
                raw.flush()
                continue
            results.setdefault((config, workload), []).append(cell["ops"])
            raw.write(json.dumps({"round": round_, "config": config, "workload": workload[:3], "aof": workload[3], **cell}) + "\n")
            raw.flush()
            print(f"round {round_} {label(config)}: {workload[0]} P={workload[2]} aof={workload[3]}: {cell['ops']:,.0f}", flush=True)

    print("\nmedian ops/s (fastest - slowest)", flush=True)
    lines = []
    for (config, workload), values in results.items():
        lines.append(f"{label(config):32} {workload[0]} P={workload[2]:<2} aof={'on ' if workload[3] else 'off'}: "
                     f"{statistics.median(values):>10,.0f}  ({min(values):,.0f} - {max(values):,.0f})")
    (out / "peak-summary.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines), flush=True)

    # Part 2: open loop, at 20,000 req/s and at 50% and 90% of each server's GET peak at pipeline 1
    print("\n== open loop", flush=True)
    latency_configs = [c for c in chosen if c[1] in (1, 4, 8) and c[0] in ("kosh", "redis8", "valkey", "nredis") and (not c[2] or c[2] == c[1])]
    with open(out / "open-loop.txt", "w") as text:
        for config in latency_configs:
            values = results.get((config, WORKLOADS[0]))
            peak = statistics.median(values) if values else 100000
            for rate in (20000, int(peak * 0.5), int(peak * 0.9)):
                header = f"\n### {label(config)}: {rate} req/s (peak {peak:,.0f})\n"
                try:
                    body = open_loop(paths, config, rate, 4 if args.trial else 10, out)
                except Exception as error:
                    body = f"FAILED: {error}\n"
                text.write(header + body)
                text.flush()
                print(header.strip(), flush=True)

    print("\n== ring_bench", flush=True)
    ring = subprocess.run([str(paths.ring), "--rounds", "3" if args.trial else "15", "--ms", "300"], stdout=subprocess.PIPE, text=True)
    (out / "ring_bench.txt").write_text(ring.stdout)
    print(ring.stdout, flush=True)
    print("load after:", open("/proc/loadavg").read().strip(), flush=True)
    print("DONE", flush=True)


main()
