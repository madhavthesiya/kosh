#!/usr/bin/env python3
"""The server with several event loops: connections reach every thread and are
served, the client limit holds across threads, and it stops promptly.

Usage: test_threads.py path/to/kosh-server
"""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

SERVER = os.path.abspath(sys.argv[1])
failures = []


def check(ok, what):
    if not ok:
        print("FAILED:", what)
        failures.append(what)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""

    def send(self, *parts):
        self.sock.sendall(encode(*parts))

    def read_line(self):
        while b"\r\n" not in self.buffer:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("closed")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        return line

    def read_reply(self):
        line = self.read_line()
        if line[:1] != b"$" or line == b"$-1":
            return line
        length = int(line[1:])
        while len(self.buffer) < length + 2:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("closed")
            self.buffer += data
        value, self.buffer = self.buffer[:length], self.buffer[length + 2:]
        return value

    def call(self, *parts):
        self.send(*parts)
        return self.read_reply()

    def close(self):
        self.sock.close()


def encode(*parts):
    out = b"*%d\r\n" % len(parts)
    for part in parts:
        part = part if isinstance(part, bytes) else str(part).encode()
        out += b"$%d\r\n%s\r\n" % (len(part), part)
    return out


class Server:
    def __init__(self, *options):
        self.port = free_port()
        self.dir = tempfile.TemporaryDirectory()
        self.log = open(os.path.join(self.dir.name, "log"), "w+")
        self.process = subprocess.Popen(
            [SERVER, "--port", str(self.port), "--appendonly", "no", "--maxkeys", "100000", *options],
            cwd=self.dir.name, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def stop(self):
        started = time.time()
        self.process.send_signal(signal.SIGTERM)
        code = self.process.wait(timeout=20)
        return code, time.time() - started

    def output(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()


def test_clients_on_every_thread_see_the_same_data():
    server = Server("--threads", "4")
    clients = [Client(server.port) for _ in range(40)]
    for i, client in enumerate(clients):
        check(client.call("SET", f"key{i}", f"value{i}") == b"+OK", "SET succeeds")
    for client in clients:
        for i in range(0, 40, 7):
            check(client.call("GET", f"key{i}") == f"value{i}".encode(), "a key set on one thread is seen on all")
    ids = [int(client.call("CLIENT", "ID")[1:]) for client in clients]
    check(len(set(ids)) == 40, "client ids are unique across threads")
    for client in clients:
        client.close()
    code, _ = server.stop()
    check(code == 0, "exit code 0")


def voluntary_switches(pid):
    counts = []
    for task in os.listdir(f"/proc/{pid}/task"):
        with open(f"/proc/{pid}/task/{task}/status") as status:
            for line in status:
                if line.startswith("voluntary_ctxt_switches"):
                    counts.append(int(line.split()[1]))
    return counts


# Each request wakes the thread serving it from epoll_wait, a voluntary context
# switch. An idle thread wakes once a second, so a thread with hundreds of them
# has been serving clients.
def test_connections_reach_every_thread():
    server = Server("--threads", "4")
    clients = [Client(server.port) for _ in range(16)]
    for _ in range(300):
        for client in clients:
            client.call("PING")
    busy = [count for count in voluntary_switches(server.process.pid) if count >= 200]
    check(len(busy) >= 4, f"all 4 threads served clients, switches: {voluntary_switches(server.process.pid)}")
    for client in clients:
        client.close()
    server.stop()


def test_pipeline_order_on_one_connection():
    server = Server("--threads", "4")
    client = Client(server.port)
    client.sock.sendall(b"".join(encode("SET", f"p{i}", i) for i in range(2000)))
    check(all(client.read_reply() == b"+OK" for _ in range(2000)), "2000 pipelined SETs answered")
    client.sock.sendall(b"".join(encode("GET", f"p{i}") for i in range(2000)))
    check(all(client.read_reply() == str(i).encode() for i in range(2000)), "2000 pipelined GETs in order")
    client.close()
    server.stop()


# A new client is handed to a thread that is asleep, so its first command is
# served only if the wakeup reaches it. A lost one would cost the idle wait, 1 s.
def test_new_clients_are_served_at_once():
    server = Server("--threads", "4")
    slowest = 0.0
    for _ in range(400):
        started = time.time()
        client = Client(server.port)
        check(client.call("PING") == b"+PONG", "PING answered")
        slowest = max(slowest, time.time() - started)
        client.close()
    check(slowest < 0.8, f"a new client waits for no idle timeout (slowest {slowest:.3f} s)")
    server.stop()


def test_client_limit_holds_across_threads():
    server = Server("--threads", "4", "--maxclients", "8")
    time.sleep(0.2)     # the connection Server() made to probe the port has gone
    clients = [Client(server.port) for _ in range(8)]
    for client in clients:
        check(client.call("PING") == b"+PONG", "within the limit")
    extra = Client(server.port)
    check(extra.read_line().startswith(b"-ERR max number of clients"), "the ninth client is refused")
    extra.close()
    clients.pop().close()
    deadline = time.time() + 5
    served = False
    while time.time() < deadline and not served:
        again = Client(server.port)
        try:
            served = again.call("PING") == b"+PONG"
        except (ConnectionError, OSError):
            served = False
        again.close()
        time.sleep(0.05)
    check(served, "a client that leaves frees its place")
    for client in clients:
        client.close()
    server.stop()


# Every thread is asleep. Without the wakeups, the others would notice only at
# their next idle timeout, up to 1 s later.
def test_stops_promptly_when_idle():
    server = Server("--threads", "8")
    clients = [Client(server.port) for _ in range(16)]
    for client in clients:
        client.call("PING")
    # Just after the PINGs, so every thread has nearly its whole idle wait
    # ahead of it, and only a wakeup can end it in time
    time.sleep(0.02)
    code, seconds = server.stop()
    check(code == 0, "exit code 0")
    check(seconds < 0.9, f"stops without waiting for the idle timeout ({seconds:.3f} s)")
    check("Received a stop signal" in server.output(), "says why it stopped")


def test_options():
    for options, message in ((["--threads", "0"], "Invalid value"), (["--threads", "257"], "Invalid value"),
                             (["--threads", "2", "--poller", "select"], "needs epoll")):
        result = subprocess.run([SERVER, "--port", "1", *options], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=20)
        check(result.returncode == 2, f"{options} is refused")
        check(message in result.stdout, f"{options} says why: {result.stdout!r}")


def test_one_thread_says_so():
    server = Server()
    check("single-threaded" in server.output(), "the default is one thread")
    check("WARNING: more than one" not in server.output(), "and no warning about threads")
    server.stop()


for name, test in list(globals().items()):
    if name.startswith("test_"):
        test()
if failures:
    print(f"{len(failures)} checks FAILED")
    sys.exit(1)
print("threads integration: all checks passed")
