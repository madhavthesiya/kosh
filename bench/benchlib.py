"""Shared helpers for the measurement scripts in this directory.

Standard library only, Linux only (CPU times come from /proc).
"""

import os
import platform
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CLK_TCK = os.sysconf("SC_CLK_TCK")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def describe_exit(code):
    if code is None:
        return "still running"
    if code < 0:
        try:
            return f"killed by {signal.Signals(-code).name}"
        except ValueError:
            return f"killed by signal {-code}"
    return f"exit code {code}"


def pinned(cpus, argv):
    """Prefixes argv with taskset so the process only runs on the given CPUs."""
    return ["taskset", "-c", cpus, *argv] if cpus else list(argv)


def cpu_list(text):
    """Parses a CPU list such as "2", "4,6" or "2-3" into a set of CPU numbers."""
    cpus = set()
    for part in filter(None, text.strip().split(",")):
        lo, _, hi = part.partition("-")
        cpus.update(range(int(lo), int(hi or lo) + 1))
    return cpus


def smt_siblings(cpus):
    """CPUs sharing a physical core (hyper-threads) with any of the given CPUs."""
    siblings = set()
    for cpu in cpu_list(cpus):
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
        if path.exists():
            siblings |= cpu_list(path.read_text())
    return siblings


# ---------------------------------------------------------------- building

def build(build_dir, targets, build_type="RelWithDebInfo", sanitize=""):
    """Configures and builds this repository's targets; returns the build directory
    as an absolute path (servers run in temporary directories, where a relative
    path would not resolve).

    Building here, rather than accepting any binary, guarantees the measured
    binary matches the recorded commit and build type.
    """
    build_dir = Path(build_dir).resolve()
    subprocess.run(["cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
                    f"-DCMAKE_BUILD_TYPE={build_type}", f"-DSANITIZE={sanitize}"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(build_dir), "-j", str(os.cpu_count() or 2),
                    "--target", *targets], check=True, stdout=subprocess.DEVNULL)
    return build_dir


def cmake_cache(build_dir):
    values = {}
    path = Path(build_dir) / "CMakeCache.txt"
    if path.exists():
        for line in path.read_text().splitlines():
            m = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
            if m:
                values[m.group(1)] = m.group(2)
    return values


def check_optimised(build_dir):
    """Refuses to measure a Debug or sanitizer build by mistake."""
    cache = cmake_cache(build_dir)
    build_type = cache.get("CMAKE_BUILD_TYPE", "")
    if build_type not in ("Release", "RelWithDebInfo") or cache.get("SANITIZE"):
        sys.exit(f"{build_dir} is a '{build_type or 'no build type'}' build"
                 f"{' with SANITIZE=' + cache['SANITIZE'] if cache.get('SANITIZE') else ''}; "
                 "measurements need Release or RelWithDebInfo without sanitizers")


# ---------------------------------------------------------------- servers

def mini_redis_argv(binary, port, workdir, aof, maxkeys, fsync="no"):
    # `fsync` is the AOF's sync policy (always, everysec or no). The server's
    # default is everysec, as Redis's; measurements pass it explicitly, so both
    # servers always run with the same durability.
    argv = [str(binary), "--port", str(port), "--maxkeys", str(maxkeys),
            "--appendonly", "yes" if aof else "no",
            "--appendfilename", str(Path(workdir) / "appendonly.aof")]
    return argv + ["--appendfsync", fsync] if aof else argv


def redis_argv(binary, port, workdir, aof, fsync="no"):
    # The same durability as Mini-Redis (`fsync`, "no" unless asked otherwise).
    # No RDB snapshots and no automatic AOF rewrites in the middle of a run.
    return [str(binary), "--port", str(port), "--bind", "127.0.0.1", "--dir", str(workdir),
            "--save", "", "--appendonly", "yes" if aof else "no", "--appendfsync", fsync,
            "--auto-aof-rewrite-percentage", "0", "--logfile", str(Path(workdir) / "redis.log")]


class NotReady(ConnectionError):
    """The server accepted the connection but is still loading its data."""


def ping(port, timeout=2.0):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(b"*1\r\n$4\r\nPING\r\n")
        reply = b""
        while not reply.endswith(b"\r\n"):
            chunk = s.recv(64)
            if not chunk:
                raise ConnectionError("connection closed before the reply")
            reply += chunk
    if reply.startswith(b"-LOADING"):   # real Redis answers this while loading its AOF
        raise NotReady(reply.decode(errors="replace").strip())
    if reply != b"+PONG\r\n":
        raise RuntimeError(f"unexpected reply to PING: {reply!r}")


def resp(*parts):
    """Encodes one command as a RESP array of bulk strings."""
    parts = [p if isinstance(p, bytes) else str(p).encode() for p in parts]
    return b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n%s\r\n" % (len(p), p) for p in parts)


def request(port, *parts, timeout=10.0):
    """Sends one command and returns its reply: a simple string, error, integer
    or bulk string (None for a null bulk string)."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(resp(*parts))
        data = b""
        while b"\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                raise ConnectionError("connection closed before the reply")
            data += chunk
        header, _, rest = data.partition(b"\r\n")
        if header[:1] != b"$":
            return header.decode(errors="replace")
        length = int(header[1:])
        if length < 0:
            return None
        while len(rest) < length + 2:
            chunk = s.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed in the middle of the reply")
            rest += chunk
        return rest[:length]


class Server:
    """A server process for one measurement. Output goes to a log file, not a
    pipe: a pipe nobody reads fills up and blocks the server."""

    def __init__(self, argv, workdir, port, cpus=None, env=None):
        """`env`: extra environment variables for the server (e.g. LD_PRELOAD)."""
        self.port = port
        self.argv = pinned(cpus, argv)
        self.log_path = Path(workdir) / "server-output.log"
        self._log = open(self.log_path, "wb")
        self.started = time.monotonic()
        # taskset execs the server, so this PID is the server's own.
        self.proc = subprocess.Popen(self.argv, cwd=workdir, stdout=self._log,
                                     stderr=subprocess.STDOUT,
                                     env={**os.environ, **env} if env else None)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()

    def alive(self):
        return self.proc.poll() is None

    def wait_ready(self, timeout=30.0, interval=0.002):
        """Waits until the server answers PING; returns seconds since it was started."""
        deadline = self.started + timeout
        while True:
            if not self.alive():
                raise RuntimeError(f"server {describe_exit(self.proc.returncode)} before it "
                                   f"accepted connections:\n{self.log_tail()}")
            try:
                ping(self.port, timeout=max(0.5, interval))
                return time.monotonic() - self.started
            except OSError:
                if time.monotonic() > deadline:
                    raise TimeoutError(f"server not ready after {timeout:.0f} s")
                time.sleep(interval)

    def cpu_times(self):
        """(user, system) CPU seconds used so far, all threads included."""
        with open(f"/proc/{self.proc.pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return int(fields[11]) / CLK_TCK, int(fields[12]) / CLK_TCK

    def stop(self):
        if self.alive():
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self._log.close()

    def log_tail(self, lines=30):
        # The server writes this file directly, so there is nothing to flush here.
        text = self.log_path.read_text(errors="replace").splitlines()
        return "\n".join(text[-lines:])


# ---------------------------------------------------------------- environment

def _first_line(argv):
    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=10)
        return (out.stdout or out.stderr).strip().splitlines()[0]
    except (OSError, IndexError, subprocess.SubprocessError):
        return "unknown"


def _git(path, *argv):
    try:
        return subprocess.run(["git", "-C", str(path), *argv], capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def git_commit_id(path=REPO_ROOT):
    """Short commit hash, or "unknown" outside a git checkout."""
    return _git(path, "rev-parse", "--short=12", "HEAD") or "unknown"


def git_commit(path=REPO_ROOT):
    """Commit hash for a results file, flagged if tracked files have been modified."""
    commit = _git(path, "rev-parse", "--short=12", "HEAD")
    if commit is None:
        return "unknown (not a git checkout?)"
    dirty = _git(path, "status", "--porcelain", "--untracked-files=no")
    return f"`{commit}`" + (" **with uncommitted changes**" if dirty else " (clean)")


def cpu_model():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def environment(build_dir=None, tools=()):
    """Rows describing the machine, build and tools, for the top of a results file."""
    release = platform.release()
    wsl = "microsoft" in release.lower()
    mem_kib = 0
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemTotal:"):
            mem_kib = int(line.split()[1])
    rows = [
        ("Date (UTC)", time.strftime("%Y-%m-%d %H:%M", time.gmtime())),
        ("Commit", git_commit()),
        ("CPU", f"{cpu_model()}, {os.cpu_count()} logical CPUs"),
        ("Memory", f"{mem_kib / 1024 / 1024:.1f} GiB"),
        ("Kernel", f"Linux {release}" + (" (WSL2: CPUs are virtual, so pinning keeps "
                                         "processes apart but does not fix the physical core)"
                                         if wsl else "")),
    ]
    if build_dir:
        cache = cmake_cache(build_dir)
        build_type = cache.get("CMAKE_BUILD_TYPE", "")
        flags = " ".join(f for f in (cache.get("CMAKE_CXX_FLAGS", ""),
                                     cache.get(f"CMAKE_CXX_FLAGS_{build_type.upper()}", "")) if f)
        compiler = cache.get("CMAKE_CXX_COMPILER", "c++")
        rows.append(("Compiler", _first_line([compiler, "--version"])))
        rows.append(("Build", f"CMake `{build_type}` (`{flags}`)"
                     + (f", SANITIZE={cache['SANITIZE']}" if cache.get("SANITIZE") else "")))
    for tool in tools:
        path = shutil.which(tool)
        rows.append((tool, _first_line([path, "--version"]) if path else "not installed"))
    rows.append(("Python", sys.version.split()[0]))
    return rows


def environment_markdown(rows):
    return "\n".join(f"- **{k}:** {v}" for k, v in rows)
