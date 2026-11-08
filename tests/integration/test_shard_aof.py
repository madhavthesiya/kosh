#!/usr/bin/env python3
"""One AOF per thread: what a reply promises, and what may be restarted from.

Usage: test_shard_aof.py path/to/kosh-server
"""
import os
import random
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_threads import Client, SERVER, check, failures, free_port     # noqa: E402

AOF = "appendonly.aof"
UNDER_TSAN = "TSAN_OPTIONS" in os.environ


class AofServer:
    count = 0

    def __init__(self, directory, *options, fsize=None):
        AofServer.count += 1
        self.port = free_port()
        self.log_path = os.path.join(directory, f"log.{AofServer.count}")

        def limit():
            if fsize is not None:
                resource.setrlimit(resource.RLIMIT_FSIZE, (fsize, resource.getrlimit(resource.RLIMIT_FSIZE)[1]))

        with open(self.log_path, "w") as log:
            self.process = subprocess.Popen(
                [SERVER, "--port", str(self.port), "--maxkeys", "2000000", *options], cwd=directory, stdout=log,
                stderr=subprocess.STDOUT, preexec_fn=limit if fsize is not None else None)
        deadline = time.time() + 30
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("server exited: " + self.output())
            try:
                Client(self.port).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def output(self):
        with open(self.log_path) as log:
            return log.read()

    def term(self):
        self.process.send_signal(signal.SIGTERM)
        return self.process.wait(timeout=30)

    def kill(self):
        self.process.kill()
        self.process.wait(timeout=30)


def refused(directory, *options):
    result = subprocess.run([SERVER, "--port", str(free_port()), *options], cwd=directory, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=30)
    return result.returncode, result.stdout


def shard_file(directory, index):
    return os.path.join(directory, f"{AOF}.shard-{index}")


def fill(directory, keys=2000, threads="4"):
    """Starts a server, writes keys, stops it cleanly."""
    server = AofServer(directory, "--threads", threads)
    client = Client(server.port)
    client.sock.sendall(b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$1\r\nv\r\n" % (len(k), k)
                                 for k in (b"key%d" % i for i in range(keys))))
    check(all(client.read_reply() == b"+OK" for _ in range(keys)), "the writes are answered")
    client.close()
    check(server.term() == 0, "a clean stop")


def present(port, keys, value=b"v"):
    client = Client(port)
    count = 0
    for key in keys:
        if client.call("GET", key) == value:
            count += 1
    client.close()
    return count


def test_every_kind_of_data_survives_a_restart():
    with tempfile.TemporaryDirectory() as d:
        server = AofServer(d, "--threads", "4")
        client = Client(server.port)
        strings, counters, lists = {}, {}, {}
        rng = random.Random(1)
        for i in range(1500):
            key = f"s{rng.randrange(300)}"
            strings[key] = str(i)
            check(client.call("SET", key, i) == b"+OK", "SET")
        for key in list(strings)[:40]:
            check(client.call("DEL", key) == b":1", "DEL")
            del strings[key]
        for i in range(400):
            key = f"n{rng.randrange(40)}"
            counters[key] = counters.get(key, 0) + 1
            client.call("INCR", key)
        for i in range(300):
            key = f"l{rng.randrange(20)}"
            lists.setdefault(key, []).append(str(i))
            client.call("RPUSH", key, i)
        client.call("SADD", "set", "a", "b")
        client.call("HSET", "hash", "f", "v")
        client.call("SET", "expiring", "v", "EX", "1000")
        client.call("SET", "gone", "v", "PX", "1")
        client.close()
        time.sleep(0.05)
        check(server.term() == 0, "a clean stop")
        with open(os.path.join(d, AOF + ".manifest")) as f:
            before = f.read()
        check(not os.path.exists(os.path.join(d, AOF)), "no single-thread file is made")
        check(all(os.path.getsize(shard_file(d, i)) > 0 for i in range(4)), "every thread wrote its own file")

        server = AofServer(d, "--threads", "4")
        client = Client(server.port)
        check(all(client.call("GET", k) == v.encode() for k, v in strings.items()), "strings come back, routed as before")
        check(all(client.call("GET", k) == str(v).encode() for k, v in counters.items()), "counters come back")
        for key, values in lists.items():
            client.send("LRANGE", key, 0, -1)
            header = client.read_line()
            check(header == b"*%d" % len(values), "a list keeps its length")
            check([client.read_reply() for _ in values] == [v.encode() for v in values], "and its order")
        check(client.call("SCARD", "set") == b":2", "sets come back")
        check(client.call("HGET", "hash", "f") == b"v", "hashes come back")
        ttl = client.call("TTL", "expiring")
        check(ttl.startswith(b":") and 900 < int(ttl[1:]) <= 1000, "a TTL comes back")
        check(client.call("GET", "gone") == b"$-1", "an expired key stays gone")
        client.close()
        check(server.term() == 0, "a clean stop")
        with open(os.path.join(d, AOF + ".manifest")) as f:
            check(f.read() == before, "the manifest doesn't change")


# The promise: a reply means the record is with the OS, so killing the process
# (which doesn't lose the OS's page cache) loses nothing acknowledged.
def test_a_killed_server_keeps_every_acknowledged_write():
    for round_ in range(4):
        with tempfile.TemporaryDirectory() as d:
            server = AofServer(d, "--threads", "4")
            acknowledged = [[] for _ in range(8)]

            def writer(index):
                client = Client(server.port)
                try:
                    for i in range(1000000):
                        key = f"w{index}:{i}"
                        if client.call("SET", key, "v") == b"+OK":
                            acknowledged[index].append(key)
                except (ConnectionError, OSError):
                    pass

            threads = [threading.Thread(target=writer, args=(i,)) for i in range(8)]
            for t in threads:
                t.start()
            time.sleep(0.4 + random.random() * 0.4)
            server.kill()
            for t in threads:
                t.join()
            keys = [k for per_writer in acknowledged for k in per_writer]
            check(len(keys) > 100, f"writes were acknowledged before the kill ({len(keys)})")
            server = AofServer(d, "--threads", "4")
            found = present(server.port, keys)
            check(found == len(keys), f"every acknowledged write survived ({found} of {len(keys)})")
            server.term()


# A file size limit makes the AOF's write fail at a chosen point. With
# --appendfsync always that stops the server, and the write that failed must
# not have been answered: a thread's reply comes after its record is written.
def test_no_reply_before_the_record_is_written():
    for round_ in range(4):
        with tempfile.TemporaryDirectory() as d:
            server = AofServer(d, "--threads", "4", "--appendfsync", "always", fsize=20000)
            client = Client(server.port)
            acknowledged = []
            try:
                for i in range(200000):
                    key = f"a{round_}:{i}"
                    if client.call("SET", key, "v" * 50) != b"+OK":
                        break
                    acknowledged.append(key)
            except (ConnectionError, OSError):
                pass
            client.close()
            code = server.process.wait(timeout=30)
            check(code == 1, f"the server stops with exit code 1 (got {code})")
            check(len(acknowledged) > 50, f"writes were answered before the failure ({len(acknowledged)})")
            server = AofServer(d, "--threads", "4")
            found = present(server.port, acknowledged, b"v" * 50)
            check(found == len(acknowledged), f"every answered write was on disk ({found} of {len(acknowledged)})")
            server.term()


# With everysec a full disk refuses writes rather than stopping, on the thread
# whose file can't be written; reads go on, and writes resume when it recovers.
def test_writes_are_refused_while_a_file_cannot_be_written():
    with tempfile.TemporaryDirectory() as d:
        server = AofServer(d, "--threads", "4", "--appendfsync", "no", fsize=20000)
        client = Client(server.port)
        acknowledged = []
        streak = 0
        # Until every thread's file is full, so that every write is refused:
        # on this client's own thread, and those it forwards to the others
        for i in range(200000):
            key = f"f{i}"
            reply = client.call("SET", key, "v" * 50)
            if reply.startswith(b"-MISCONF"):
                streak += 1
                if streak == 300:
                    break
                continue
            streak = 0
            check(reply == b"+OK", "SET")
            acknowledged.append(key)
        check(streak == 300, f"every write is refused once the files can't be written (streak {streak})")
        check(client.call("GET", acknowledged[0]) == b"v" * 50, "reads still work")
        hard = resource.getrlimit(resource.RLIMIT_FSIZE)[1]
        resource.prlimit(server.process.pid, resource.RLIMIT_FSIZE, (hard, hard))
        resumed = 0
        for i in range(400):
            key = f"after{i}"
            if client.call("SET", key, "v" * 50) == b"+OK":
                acknowledged.append(key)
                resumed += 1
        check(resumed == 400, f"writes resume once the file can be written ({resumed} of 400)")
        client.close()
        check(server.term() == 0, "a clean stop")
        server = AofServer(d, "--threads", "4")
        found = present(server.port, acknowledged, b"v" * 50)
        check(found == len(acknowledged), f"everything acknowledged is there ({found} of {len(acknowledged)})")
        server.term()


def test_the_thread_count_cannot_change():
    with tempfile.TemporaryDirectory() as d:
        fill(d)
        code, out = refused(d, "--threads", "2")
        check(code == 1 and "written by 4 threads" in out and "--threads 4" in out, f"fewer threads are refused: {out}")
        code, out = refused(d, "--threads", "8")
        check(code == 1 and "written by 4 threads" in out, f"more threads are refused: {out}")
        code, out = refused(d)
        check(code == 1 and "several threads" in out, f"one thread is refused: {out}")
        server = AofServer(d, "--threads", "4")
        check(present(server.port, [f"key{i}" for i in range(2000)]) == 2000, "the right count still loads everything")
        server.term()


def test_a_single_thread_file_stays_single_thread():
    with tempfile.TemporaryDirectory() as d:
        server = AofServer(d)
        client = Client(server.port)
        check(client.call("SET", "one", "v") == b"+OK", "SET")
        client.close()
        server.term()
        code, out = refused(d, "--threads", "4")
        check(code == 1 and "single thread" in out, f"several threads are refused: {out}")
        check(not os.path.exists(os.path.join(d, AOF + ".manifest")), "and nothing is written")
        server = AofServer(d)
        check(present(server.port, ["one"]) == 1, "one thread still loads it")
        server.term()


def test_a_missing_file_is_refused():
    with tempfile.TemporaryDirectory() as d:
        fill(d)
        os.remove(shard_file(d, 2))
        code, out = refused(d, "--threads", "4")
        check(code == 1 and "missing" in out, f"a missing file is refused: {out}")


def test_a_missing_manifest_is_refused():
    with tempfile.TemporaryDirectory() as d:
        fill(d)
        os.remove(os.path.join(d, AOF + ".manifest"))
        code, out = refused(d, "--threads", "4")
        check(code == 1 and "manifest" in out, f"data without a manifest is refused: {out}")


def test_a_damaged_manifest_is_refused():
    with tempfile.TemporaryDirectory() as d:
        fill(d)
        with open(os.path.join(d, AOF + ".manifest"), "a") as f:
            f.write("x")
        code, out = refused(d, "--threads", "4")
        check(code == 1 and "damaged" in out, f"a damaged manifest is refused: {out}")


# The manifest is written last, so a crash before it leaves only empty files
def test_a_crash_before_the_manifest_is_harmless():
    with tempfile.TemporaryDirectory() as d:
        for i in range(4):
            open(shard_file(d, i), "w").close()
        server = AofServer(d, "--threads", "4")
        client = Client(server.port)
        check(client.call("SET", "k", "v") == b"+OK", "starts as new")
        client.close()
        server.term()
        check(os.path.exists(os.path.join(d, AOF + ".manifest")), "and writes a manifest")


# If a file can't be opened, there must be no manifest promising that it exists
def test_the_manifest_comes_after_the_files():
    with tempfile.TemporaryDirectory() as d:
        os.mkdir(shard_file(d, 2))      # opening this as a file fails
        code, out = refused(d, "--threads", "4")
        check(code == 1 and "Cannot open" in out, f"a file that can't be opened stops the start: {out}")
        check(not os.path.exists(os.path.join(d, AOF + ".manifest")), "and no manifest was written")


def test_files_swapped_between_threads_are_refused():
    with tempfile.TemporaryDirectory() as d:
        fill(d)
        a, b = shard_file(d, 0), shard_file(d, 1)
        shutil.move(a, a + ".x")
        shutil.move(b, a)
        shutil.move(a + ".x", b)
        code, out = refused(d, "--threads", "4")
        check(code == 1 and "another thread" in out, f"mixed-up files are refused: {out}")


# Only the end of one file is cut off, and the other files aren't touched
def test_an_incomplete_last_record_costs_only_that_record():
    with tempfile.TemporaryDirectory() as d:
        fill(d)
        path = shard_file(d, 2)
        size = os.path.getsize(path)
        with open(path, "r+b") as f:
            f.truncate(size - 3)
        server = AofServer(d, "--threads", "4")
        check("incomplete record" in server.output() and "shard-2" in server.output(), "the warning names the file")
        found = present(server.port, [f"key{i}" for i in range(2000)])
        check(found == 1999, f"exactly the cut-off record is gone ({found} of 2000)")
        server.term()


if __name__ == "__main__":
    for name, test in list(globals().items()):
        if name.startswith("test_"):
            test()
    if failures:
        print(f"{len(failures)} checks FAILED")
        sys.exit(1)
    print("shard AOF: all checks passed")
