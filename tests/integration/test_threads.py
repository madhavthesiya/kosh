#!/usr/bin/env python3
"""The server with several event loops: connections reach every thread and are
served, the client limit holds across threads, and it stops promptly.

Usage: test_threads.py path/to/kosh-server
"""
import os
import random
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

SERVER = os.path.abspath(sys.argv[1])
UNDER_TSAN = "TSAN_OPTIONS" in os.environ
failures = []


def check(ok, what):
    if not ok:
        print("FAILED:", what)
        failures.append(what)


class Canary:
    """How long the whole machine froze: a thread that only sleeps, and notes how
    late it woke. WSL2 sometimes stops every process for two seconds. A time
    limit on the server allows for that, so only the server can fail it."""

    def __enter__(self):
        self.worst = 0.0
        self.done = False
        self.thread = threading.Thread(target=self.run)
        self.thread.start()
        return self

    def run(self):
        while not self.done:
            started = time.time()
            time.sleep(0.005)
            self.worst = max(self.worst, time.time() - started - 0.005)

    def __exit__(self, *unused):
        self.done = True
        self.thread.join()


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

    def read_value(self):
        line = self.read_line()
        if line[:1] == b"*" and line != b"*-1":
            return [self.read_value() for _ in range(int(line[1:]))]
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
    with Canary() as canary:
        for _ in range(400):
            started = time.time()
            client = Client(server.port)
            check(client.call("PING") == b"+PONG", "PING answered")
            slowest = max(slowest, time.time() - started)
            client.close()
    check(slowest < 0.8 + canary.worst, f"a new client waits for no idle timeout (slowest {slowest:.3f} s, machine froze {canary.worst:.3f} s)")
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
    with Canary() as canary:
        code, seconds = server.stop()
    check(code == 0, "exit code 0")
    check(seconds < 0.9 + canary.worst, f"stops without waiting for the idle timeout ({seconds:.3f} s, machine froze {canary.worst:.3f} s)")
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


# ---- Forwarding: commands reach the thread that owns their key

def run_batch(client, commands):
    """Sends the commands in one write and returns their replies, in order."""
    client.sock.sendall(b"".join(encode(*c) for c in commands))
    return [client.read_value() for _ in commands]


# Replies must come back in the order the commands were sent, whichever
# thread runs each. Every command's reply is predicted from a model.
def random_commands(rng, prefix, model, count):
    commands, expected = [], []
    for _ in range(count):
        key = f"{prefix}:{rng.randrange(200)}"
        kind = rng.randrange(8)
        if kind == 0:
            value = str(rng.randrange(1 << 30))
            model[key] = value
            commands.append(("SET", key, value))
            expected.append(b"+OK")
        elif kind == 1:
            commands.append(("GET", key))
            expected.append(model[key].encode() if key in model else b"$-1")
        elif kind == 2:
            model[key] = str(int(model.get(key, "0")) + 1)
            commands.append(("INCR", key))
            expected.append(b":" + model[key].encode())
        elif kind == 3:
            commands.append(("DEL", key))
            expected.append(b":1" if model.pop(key, None) is not None else b":0")
        elif kind == 5:         # these three go to several threads
            keys = [f"{prefix}:{rng.randrange(200)}" for _ in range(rng.randrange(1, 12))]
            commands.append(("MGET", *keys))
            expected.append([model[k].encode() if k in model else b"$-1" for k in keys])
        elif kind == 6:
            keys = [f"{prefix}:{rng.randrange(200)}" for _ in range(rng.randrange(1, 12))]
            commands.append(("EXISTS", *keys))
            expected.append(b":%d" % sum(1 for k in keys if k in model))
        elif kind == 7:
            keys = [f"{prefix}:{rng.randrange(200)}" for _ in range(rng.randrange(1, 6))]
            commands.append(("DEL", *keys))
            expected.append(b":%d" % sum(1 for k in set(keys) if model.pop(k, None) is not None))
        else:
            commands.append(("PING",))      # runs on the home thread, between the others
            expected.append(b"+PONG")
    return commands, expected


def test_pipelines_stay_in_order_across_threads():
    server = Server("--threads", "4")
    rng = random.Random(5)
    client = Client(server.port)
    model = {}
    for _ in range(60):
        commands, expected = random_commands(rng, "order", model, rng.randrange(1, 600))
        replies = run_batch(client, commands)
        check(replies == expected, "a pipeline's replies match a model, in order")
    client.close()
    server.stop()


def test_many_clients_against_a_model():
    server = Server("--threads", "8")

    def worker(index):
        rng = random.Random(index)
        client = Client(server.port)
        model = {}
        for _ in range(120):
            commands, expected = random_commands(rng, f"c{index}", model, rng.randrange(1, 40))
            if run_batch(client, commands) != expected:
                check(False, f"client {index} got replies that don't match its model")
                break
        client.close()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check(server.process.poll() is None, "the server survived")
    server.stop()


# One key, one owner: increments from every thread are never lost
def test_counter_shared_by_all_threads():
    server = Server("--threads", "8")

    def worker():
        client = Client(server.port)
        for _ in range(100):
            run_batch(client, [("INCR", "shared")] * 20)
        client.close()

    threads = [threading.Thread(target=worker) for _ in range(16)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    client = Client(server.port)
    check(client.call("GET", "shared") == b"32000", "no increment was lost")
    client.close()
    server.stop()


# A client may close only its sending side and go on reading
def test_replies_reach_a_client_that_closed_its_sending_side():
    server = Server("--threads", "4")
    client = Client(server.port)
    client.sock.sendall(b"".join(encode("SET", f"h{i}", i) for i in range(200)))
    client.sock.shutdown(socket.SHUT_WR)
    check(all(client.read_reply() == b"+OK" for _ in range(200)), "every reply arrives after shutdown(SHUT_WR)")
    check(client.sock.recv(100) == b"", "then the server closes")
    client.close()
    server.stop()


class BusyThread:
    """Keeps the thread that owns {hot} busy: an LRANGE of 400,000 elements takes
    it tens of milliseconds, while what other threads send it piles up."""
    ELEMENTS = 400000

    def __init__(self, port):
        self.client = Client(port)
        for _ in range(self.ELEMENTS // 1000):
            self.client.call("RPUSH", "{hot}:list", *(["v"] * 1000))
        self.reply_size = len(b"*%d\r\n" % self.ELEMENTS) + self.ELEMENTS * len(b"$1\r\nv\r\n")
        self.stopping = False
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        request = encode("LRANGE", "{hot}:list", 0, -1)
        buffer = bytearray(1 << 20)
        while not self.stopping:
            self.client.sock.sendall(request)
            remaining = self.reply_size
            while remaining:
                n = self.client.sock.recv_into(buffer, min(remaining, len(buffer)))
                if n == 0:
                    return
                remaining -= n

    def stop(self):
        self.stopping = True
        self.thread.join()
        self.client.close()


# Thousands of one-command sends to a thread that is busy: each becomes its own
# batch, more than the ring between two threads could hold, so batches must
# wait at home and later commands join them.
def test_batches_wait_while_a_thread_is_busy():
    server = Server("--threads", "4")
    busy = BusyThread(server.port)
    clients = [Client(server.port) for _ in range(8)]
    request = encode("INCR", "{hot}:n")
    sends = 8000 if UNDER_TSAN else 20000
    for i in range(sends):
        clients[i % 8].sock.sendall(request)
    in_order = True
    for index, client in enumerate(clients):
        values = [int(client.read_reply()[1:]) for _ in range(sends // 8)]
        in_order = in_order and values == sorted(values) and len(set(values)) == len(values)
    check(in_order, "each client's increments come back in order, none twice")
    check(clients[0].call("GET", "{hot}:n") == str(sends).encode(), "and none was lost")
    busy.stop()
    for client in clients:
        client.close()
    server.stop()


# A reset closes the client at once, even with a reply on its way. The next
# client can get the same socket number, and must not get that reply.
def test_a_reused_socket_number_gets_no_stale_reply():
    server = Server("--threads", "2")
    busy = BusyThread(server.port)
    reset_on_close = struct.pack("ii", 1, 0)
    setter = Client(server.port)
    check(setter.call("SET", "{hot}:present", "present") == b"+OK", "SET")
    setter.close()
    spread = [f"spread{i}" for i in range(12)]
    for round_ in range(40 if UNDER_TSAN else 300):
        # Clients are dealt in turn and a closed socket's number is the next one
        # given out. So with 2 threads, the filler takes the other thread, and
        # the next client gets the doomed one's thread and its socket number.
        doomed = Client(server.port)
        filler = Client(server.port)
        # Half the time the late reply is a whole command's, half a split one's.
        # (Each round starts on the other thread, so the kinds change every two.)
        doomed.sock.sendall(encode("GET", "{hot}:missing") if round_ % 4 < 2 else encode("MGET", "{hot}:missing", *spread))
        time.sleep(0.001)
        doomed.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, reset_on_close)
        doomed.close()
        time.sleep(0.001)
        # This goes through the busy thread too, behind the doomed client's
        # command, so that command's reply must arrive while this client is here
        fresh = Client(server.port)
        check(fresh.call("GET", "{hot}:present") == b"present", "a new client gets only its own reply")
        fresh.close()
        filler.close()
    busy.stop()
    check(server.process.poll() is None, "the server survived")
    server.stop()


# The limit on replies a client can have in flight is what bounds its memory.
# The keys are on one thread and the two clients on different threads, so one
# of them sends every command away. One read of 16 KB holds over 550 commands,
# so without the limit far more than 256 would be out at once.
def test_replies_in_flight_are_bounded():
    server = Server("--threads", "2")
    writer = Client(server.port)
    check(writer.call("SET", "{w}:k", "v") == b"+OK", "SET")
    writer.close()
    count = 2000
    readers = [Client(server.port), Client(server.port)]
    for reader in readers:
        reader.sock.sendall(b"".join(encode("GET", "{w}:k") for _ in range(count)))
    for reader in readers:
        received = sum(1 for _ in range(count) if reader.read_reply() == b"v")
        check(received == count, f"every reply arrives ({received} of {count})")
    peak = 0
    for reader in readers:
        for line in reader.call("INFO", "stats").decode().split("\r\n"):
            if line.startswith("peak_pending_replies:"):
                peak = max(peak, int(line.split(":")[1]))
    check(200 <= peak <= 256, f"at most 256 replies were in flight, and it got there (peak {peak})")
    for reader in readers:
        reader.close()
    server.stop()


# What ends a connection comes after every reply to the commands before it
def test_quit_comes_after_replies_on_other_threads():
    server = Server("--threads", "4")
    client = Client(server.port)
    client.sock.sendall(b"".join(encode("SET", f"q{i}", i) for i in range(100)) + encode("QUIT") + encode("SET", "after", "1"))
    check(all(client.read_reply() == b"+OK" for _ in range(100)), "the commands before QUIT are answered")
    check(client.read_reply() == b"+OK", "then QUIT")
    check(client.sock.recv(100) == b"", "then the connection closes")
    client.close()
    other = Client(server.port)
    check(other.call("GET", "after") == b"$-1", "a command after QUIT doesn't run")
    other.close()
    server.stop()


def test_protocol_error_comes_after_replies_on_other_threads():
    server = Server("--threads", "4")
    client = Client(server.port)
    client.sock.sendall(b"".join(encode("SET", f"p{i}", i) for i in range(100)) + b"*1\r\n$abc\r\n")
    check(all(client.read_reply() == b"+OK" for _ in range(100)), "the commands before the error are answered")
    check(client.read_reply().startswith(b"-ERR Protocol error"), "then the error")
    check(client.sock.recv(100) == b"", "then the connection closes")
    client.close()
    server.stop()


# Replies still on their way to clients that have gone must be dropped, not
# crash the server or reach whoever gets the socket number next
def test_clients_leaving_with_replies_in_flight():
    server = Server("--threads", "2")
    for round_ in range(400):
        doomed = Client(server.port)
        doomed.sock.sendall(b"".join(encode("GET", f"gone{i}") for i in range(64)))
        doomed.close()
        fresh = Client(server.port)
        check(fresh.call("SET", "fresh", round_) == b"+OK", "a new client gets its own reply")
        check(fresh.call("GET", "fresh") == str(round_).encode(), "and only its own")
        fresh.close()
    check(server.process.poll() is None, "the server survived")
    server.stop()


# The reply is written on another thread, in the protocol of the client that asked
def test_replies_use_the_clients_protocol():
    server = Server("--threads", "4")
    resp2, resp3 = Client(server.port), Client(server.port)
    resp3.sock.sendall(encode("HELLO", "3"))
    header = resp3.read_line()
    check(header.startswith(b"%"), "HELLO 3 replies with a map")
    for _ in range(int(header[1:]) * 2):
        resp3.read_reply()
    for i in range(40):
        check(resp2.call("GET", f"missing{i}") == b"$-1", "RESP2's null")
        check(resp3.call("GET", f"missing{i}") == b"_", "RESP3's null")
    resp2.close()
    resp3.close()
    server.stop()


def test_password_is_checked_before_forwarding():
    server = Server("--threads", "4", "--requirepass", "secret")
    client = Client(server.port)
    for i in range(20):
        check(client.call("GET", f"k{i}").startswith(b"-NOAUTH"), "refused before AUTH, on any key")
    check(client.call("AUTH", "secret") == b"+OK", "AUTH")
    for i in range(20):
        check(client.call("SET", f"k{i}", i) == b"+OK", "served after AUTH, on any key")
    client.close()
    server.stop()


def test_large_values_cross_threads():
    server = Server("--threads", "4")
    client = Client(server.port)
    value = b"v" * (2 << 20)
    for i in range(20):
        check(client.call("SET", f"big{i}", value) == b"+OK", "a 2 MB SET")
    for i in range(20):
        check(client.call("GET", f"big{i}") == value, "a 2 MB GET")
    client.close()
    server.stop()


def resident_mb(pid):
    with open(f"/proc/{pid}/status") as status:
        for line in status:
            if line.startswith("VmRSS"):
                return int(line.split()[1]) // 1024
    return 0


# One client that stops reading must not hold up the others, and none of its
# replies may be lost
def test_a_client_that_does_not_read():
    server = Server("--threads", "4")
    writer = Client(server.port)
    value = b"x" * 65536
    for i in range(32):
        writer.call("SET", f"wide{i}", value)
    slow = Client(server.port)
    count = 3000
    sender = threading.Thread(target=lambda: slow.sock.sendall(
        b"".join(encode("GET", f"wide{i % 32}") for i in range(count))))
    sender.start()
    time.sleep(0.5)
    other = Client(server.port)
    slowest = 0.0
    with Canary() as canary:
        for i in range(200):
            started = time.time()
            other.call("GET", f"wide{i % 32}")
            slowest = max(slowest, time.time() - started)
    check(slowest < 0.5 + canary.worst, f"another client isn't held up (slowest {slowest:.3f} s, machine froze {canary.worst:.3f} s)")
    other.close()
    received = 0
    for _ in range(count):
        if slow.read_reply() == value:
            received += 1
    sender.join()
    check(received == count, f"the slow client got every reply ({received} of {count})")
    slow.close()
    server.stop()


# Many clients taking turns: threads keep going to sleep and being woken just
# as work arrives. A lost wakeup would cost the idle wait, a whole second.
def test_no_stall_from_a_lost_wakeup():
    server = Server("--threads", "4")
    slowest = [0.0] * 8
    deadline = time.time() + 6

    def worker(index):
        client = Client(server.port)
        rng = random.Random(index)
        while time.time() < deadline:
            started = time.time()
            client.call("GET", f"wake{rng.randrange(1000)}")
            slowest[index] = max(slowest[index], time.time() - started)
            if rng.randrange(4) == 0:
                time.sleep(0.0002)
        client.close()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
    with Canary() as canary:
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    check(max(slowest) < 0.8 + canary.worst, f"no request waited for an idle timeout (slowest {max(slowest):.3f} s, machine froze {canary.worst:.3f} s)")
    server.stop()


# ---- Commands that name several keys

def test_keys_on_several_threads_are_refused():
    server = Server("--threads", "4")
    client = Client(server.port)
    keys = [f"multi{i}" for i in range(30)]    # 30 keys can't all be on one thread of 4 in practice
    pairs = [part for key in keys for part in (key, "v")]
    check(client.call("MSET", *pairs).startswith(b"-CROSSSLOT"), "MSET across threads is refused")
    for key in keys:
        check(client.call("GET", key) == b"$-1", "and sets nothing")
    check(client.call("MSET", "a", "1", "b").startswith(b"-ERR wrong number"), "unpaired MSET says so")
    check(client.call("MSET", "{t}a", "1", "{t}b", "2") == b"+OK", "MSET with one tag works")
    check(client.call("EXISTS", "{t}a", "{t}b", "{t}c") == b":2", "EXISTS with one tag")
    check(client.call("DEL", "{t}a", "{t}b") == b":2", "DEL with one tag")
    client.close()
    server.stop()


# ---- Bad input

# The fuzz target goes through the parser and the command table on one thread.
# This sends such input through routing, forwarding and split commands.
def test_garbage_and_random_commands():
    server = Server("--threads", "4")
    corpus = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fuzz", "corpus")
    inputs = []
    for name in sorted(os.listdir(corpus)):
        with open(os.path.join(corpus, name), "rb") as f:
            inputs.append(f.read())
    rng = random.Random(9)
    names = ["GET", "SET", "DEL", "EXISTS", "MGET", "MSET", "MULTI", "EXEC", "DISCARD", "SCAN", "DBSIZE", "INFO",
             "FLUSHALL", "HELLO", "CLIENT", "AUTH", "QUIT", "PING", "LPUSH", "LRANGE", "HSET", "HGETALL", "SADD",
             "EXPIRE", "TTL", "INCR", "BGREWRITEAOF", "NOSUCH", "COMMAND", "CONFIG"]
    pieces = [b"", b"{", b"}", b"{}", b"{t}", b"a{b}c", b"0", b"-1", b"18446744073709551615", b"\x00\xff", b"3", b"x" * 300, b"*", b"COUNT", b"MATCH"]
    for _ in range(400 if UNDER_TSAN else 2000):
        commands = []
        for _ in range(rng.randrange(1, 12)):
            parts = [rng.choice(names).encode()] + [rng.choice(pieces) for _ in range(rng.randrange(0, 6))]
            commands.append(encode(*parts))
        inputs.append(b"".join(commands))
    for index, data in enumerate(inputs):
        client = Client(server.port)
        try:
            client.sock.sendall(data)
            if index % 3 == 0:
                client.sock.settimeout(0.02)
                client.sock.recv(65536)
        except OSError:
            pass
        client.close()
    time.sleep(0.2)
    check(server.process.poll() is None, "the server survived")
    client = Client(server.port)
    check(client.call("PING") == b"+PONG", "and still answers")
    client.close()
    server.stop()


# ---- Commands that span threads

def read_array_of(client, count):
    return [client.read_value() for _ in range(count)]


def test_multi_key_commands_across_threads():
    server = Server("--threads", "4")
    client = Client(server.port)
    keys = [f"mk{i}" for i in range(80)]
    for i, key in enumerate(keys[::2]):
        check(client.call("SET", key, i) == b"+OK", "SET")
    rng = random.Random(3)
    for _ in range(300):
        chosen = [rng.choice(keys) for _ in range(rng.randrange(1, 30))]
        expected = [str(keys[::2].index(k)).encode() if k in keys[::2] else b"$-1" for k in chosen]
        client.send("MGET", *chosen)
        check(client.read_value() == expected, "MGET's values come back in the order the keys were given")
        count = sum(1 for k in chosen if k in keys[::2])
        check(client.call("EXISTS", *chosen) == b":%d" % count, "EXISTS adds up, counting a repeated key twice")
    check(client.call("LPUSH", "alist", "x") == b":1", "LPUSH")
    client.send("MGET", "alist", keys[0], "missing-key")
    check(client.read_value() == [b"$-1", b"0", b"$-1"], "MGET gives nil for a key of another type")
    victims = keys[::2][:20] + ["no-such-key", keys[0]]
    check(client.call("DEL", *victims) == b":20", "DEL adds up, counting a key once")
    client.send("MGET", *keys[::2][:20])
    check(client.read_value() == [b"$-1"] * 20, "and the keys are gone")
    client.close()
    server.stop()


def test_mget_across_threads_uses_the_clients_protocol():
    server = Server("--threads", "4")
    client = Client(server.port)
    client.sock.sendall(encode("HELLO", "3"))
    header = client.read_line()
    for _ in range(int(header[1:]) * 2):
        client.read_reply()
    check(client.call("SET", "present", "v") == b"+OK", "SET")
    keys = ["present"] + [f"absent{i}" for i in range(30)]
    client.send("MGET", *keys)
    check(client.read_value() == [b"v"] + [b"_"] * 30, "RESP3's nulls")
    client.close()
    server.stop()


def transaction(client, *commands):
    client.sock.sendall(encode("MULTI") + b"".join(encode(*c) for c in commands) + encode("EXEC"))
    check(client.read_reply() == b"+OK", "MULTI")
    queued = [client.read_value() for _ in commands]
    return queued, client.read_value()


def test_a_transaction_on_one_thread():
    server = Server("--threads", "4")
    client = Client(server.port)
    queued, result = transaction(client, ("SET", "{t}a", "1"), ("INCR", "{t}a"), ("GET", "{t}a"), ("PING",))
    check(queued == [b"+QUEUED"] * 4, "commands are queued")
    check(result == [b"+OK", b":2", b"2", b"+PONG"], f"EXEC runs them in order: {result}")
    check(client.call("GET", "{t}a") == b"2", "and they took effect")
    queued, result = transaction(client, ("PING",),)
    check(result == [b"+PONG"], "a transaction with no keys")
    client.send("MULTI")
    check(client.read_reply() == b"+OK", "MULTI")
    check(client.call("SET", "{t}b", "1") == b"+QUEUED", "queued")
    check(client.call("DISCARD") == b"+OK", "DISCARD")
    check(client.call("GET", "{t}b") == b"$-1", "and nothing ran")
    client.close()
    server.stop()


# Nothing may come between a transaction's commands, whichever thread clients are on
def test_transactions_are_atomic():
    server = Server("--threads", "4")
    stop = [False]

    def writer(index):
        client = Client(server.port)
        i = 0
        while not stop[0]:
            i += 1
            value = f"{index}.{i}"
            _, result = transaction(client, ("SET", "{a}x", value), ("SET", "{a}y", value))
            check(result == [b"+OK", b"+OK"], "EXEC")
        client.close()

    threads = [threading.Thread(target=writer, args=(i,)) for i in range(8)]
    for t in threads:
        t.start()
    reader = Client(server.port)
    torn = 0
    for _ in range(600 if UNDER_TSAN else 6000):
        reader.send("MGET", "{a}x", "{a}y")
        x, y = reader.read_value()
        torn += x != y
    stop[0] = True
    for t in threads:
        t.join()
    check(torn == 0, f"no reader saw half a transaction ({torn} did)")
    reader.close()
    server.stop()


def test_a_transaction_across_threads_is_refused():
    server = Server("--threads", "4")
    client = Client(server.port)
    client.send("MULTI")
    check(client.read_reply() == b"+OK", "MULTI")
    replies = [client.call("SET", f"tx{i}", "v") for i in range(30)]
    check(any(r.startswith(b"-CROSSSLOT") for r in replies), "a key on another thread is refused while queuing")
    check(client.call("EXEC").startswith(b"-EXECABORT"), "and EXEC refuses the transaction")
    for i in range(30):
        check(client.call("GET", f"tx{i}") == b"$-1", "nothing ran")
    client.close()
    server.stop()


def test_commands_refused_inside_a_transaction():
    server = Server("--threads", "4")
    client = Client(server.port)
    for command in (("INFO",), ("DBSIZE",), ("SCAN", "0"), ("FLUSHALL",), ("CLIENT", "ID"), ("HELLO",)):
        client.send("MULTI")
        check(client.read_reply() == b"+OK", "MULTI")
        reply = client.call(*command)
        check(reply.startswith(b"-ERR Command not allowed"), f"{command[0]} is refused in a transaction: {reply}")
        check(client.call("EXEC").startswith(b"-EXECABORT"), "and aborts it")
    client.close()
    server.stop()


def scan_all(client, *options):
    cursor, seen, calls = "0", [], 0
    while True:
        client.send("SCAN", cursor, *options)
        cursor, keys = client.read_value()
        cursor = cursor.decode()
        seen += keys
        calls += 1
        if cursor == "0" or calls > 200000:
            return seen, calls


def test_scan_visits_every_thread():
    server = Server("--threads", "4", "--maxkeys", "1000000")
    client = Client(server.port)
    keys = {f"scan:{i}".encode() for i in range(3000)}
    run_batch(client, [("SET", k.decode(), "v") for k in keys])
    run_batch(client, [("RPUSH", "alist", "x")])
    for count in (1, 10, 1000):
        seen, calls = scan_all(client, "COUNT", count)
        check(set(seen) == keys | {b"alist"}, f"COUNT {count}: every key is returned ({len(set(seen))})")
        check(calls >= 4, "and every thread was visited")
    seen, _ = scan_all(client, "MATCH", "scan:1*", "COUNT", 50)
    check(set(seen) == {k for k in keys if k.startswith(b"scan:1")}, "MATCH works across threads")
    seen, _ = scan_all(client, "TYPE", "list")
    check(set(seen) == {b"alist"}, "TYPE works across threads")
    # A key present for the whole scan comes back, whatever else changes
    extra = Client(server.port)
    cursor, seen, step = "0", [], 0
    while True:
        client.send("SCAN", cursor, "COUNT", 40)
        cursor, batch = client.read_value()
        cursor = cursor.decode()
        seen += batch
        step += 1
        extra.call("SET", f"added:{step}", "v")
        if cursor == "0":
            break
    check(keys <= set(seen), "keys present throughout are returned while others are added")
    client.send("SCAN", str(100 << 56))
    check(client.read_value() == [b"0", []], "a cursor naming no thread ends the scan")
    check(client.call("SCAN", "abc").startswith(b"-ERR invalid cursor"), "a bad cursor is refused")
    check(client.call("SCAN", "0", "NOPE", "1").startswith(b"-ERR syntax"), "a bad option is refused")
    extra.close()
    client.close()
    server.stop()


def test_dbsize_and_flushall_cover_every_thread():
    server = Server("--threads", "4")
    client = Client(server.port)
    run_batch(client, [("SET", f"d{i}", "v") for i in range(500)])
    check(client.call("DBSIZE") == b":500", "DBSIZE adds up the threads")
    check(client.call("FLUSHALL") == b"+OK", "FLUSHALL")
    check(client.call("DBSIZE") == b":0", "and every thread is empty")
    run_batch(client, [("SET", f"d{i}", "v") for i in range(100)])
    check(client.call("FLUSHDB", "ASYNC") == b"+OK", "FLUSHDB")
    check(client.call("DBSIZE") == b":0", "and every thread is empty again")
    check(client.call("FLUSHALL", "NOPE").startswith(b"-ERR syntax"), "a bad option is refused, once")
    client.close()
    server.stop()


# A whole-server command sees this client's earlier commands and none of its later ones
def test_whole_server_commands_keep_their_place_in_a_pipeline():
    server = Server("--threads", "4")
    client = Client(server.port)
    for round_ in range(30):
        commands = [("SET", f"a{i}", "v") for i in range(60)] + [("DBSIZE",)]
        commands += [("SET", f"b{i}", "v") for i in range(40)] + [("DBSIZE",), ("FLUSHALL",), ("GET", "a0"), ("SET", "c", "v"), ("DBSIZE",), ("FLUSHALL",)]
        expected = [b"+OK"] * 60 + [b":60"] + [b"+OK"] * 40 + [b":100", b"+OK", b"$-1", b"+OK", b":1", b"+OK"]
        check(run_batch(client, commands) == expected, "whole-server commands keep their place in the pipeline")
    client.close()
    server.stop()


def test_info_adds_up_the_threads():
    server = Server("--threads", "4", "--maxkeys", "9999")
    time.sleep(0.2)
    clients = [Client(server.port) for _ in range(16)]

    def info(client, *sections):
        fields = {}
        for line in client.call("INFO", *sections).decode().split("\r\n"):
            if ":" in line:
                name, value = line.split(":", 1)
                fields[name] = value
        return fields

    before = int(info(clients[0], "stats")["total_commands_processed"])
    commands = 0
    for i, client in enumerate(clients):
        for j in range(25):
            check(client.call("SET", f"i{i}.{j}", "v") == b"+OK", "SET")
            check(client.call("GET", f"i{i}.{j}") == b"v", "a hit")
            check(client.call("GET", f"none{i}.{j}") == b"$-1", "a miss")
            commands += 3
    time.sleep(0.05)
    fields = info(clients[3])
    check(int(fields["total_commands_processed"]) == before + commands + 1, f"commands add up: {fields['total_commands_processed']}, wanted {before + commands + 1}")
    check(fields["keyspace_hits"] == "400" and fields["keyspace_misses"] == "400", "hits and misses add up")
    check(fields["connected_clients"] == "16", f"clients are counted once: {fields['connected_clients']}")
    check(fields["maxkeys"] == "9999", "the limit is the whole server's")
    check(fields["db0"].startswith("keys=400,"), f"keys add up: {fields['db0']}")
    # A command that spans threads counts once, however many threads ran it
    before = int(info(clients[1], "stats")["total_commands_processed"])
    spread = [f"i{i}.0" for i in range(16)]
    for _ in range(20):
        clients[1].send("MGET", *spread)
        clients[1].read_value()
        check(clients[1].call("EXISTS", *spread) == b":16", "EXISTS")
        check(clients[1].call("DBSIZE") == b":400", "DBSIZE")
    after = int(info(clients[1], "stats")["total_commands_processed"])
    check(after == before + 61, f"split commands count once: {after - before}, wanted 61")
    only = info(clients[0], "keyspace")
    check(list(only) == ["db0"], f"sections still select: {only}")
    for client in clients:
        client.close()
    server.stop()


# Parts of a split command are still out when the client goes away
def test_clients_leaving_during_a_split_command():
    server = Server("--threads", "4")
    reset_on_close = struct.pack("ii", 1, 0)
    keys = [f"lk{i}" for i in range(40)]
    for round_ in range(200):
        doomed = Client(server.port)
        doomed.sock.sendall(encode("MGET", *keys) + encode("DBSIZE") + encode("INFO"))
        if round_ % 2:
            doomed.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, reset_on_close)
        doomed.close()
    client = Client(server.port)
    check(client.call("PING") == b"+PONG", "the server survived")
    client.send("MGET", *keys)
    check(client.read_value() == [b"$-1"] * 40, "and still works")
    client.close()
    server.stop()


# ---- Each thread evicts its own keys

def test_the_key_limit_is_shared_out():
    port_server = Server("--threads", "4", "--maxkeys", "400", "--eviction", "lru")
    client = Client(port_server.port)
    for i in range(0, 4000, 40):
        run_batch(client, [("SET", f"e{j}", "v") for j in range(i, i + 40)])
    hits = 0
    for i in range(0, 4000, 40):
        hits += sum(1 for r in run_batch(client, [("GET", f"e{j}") for j in range(i, i + 40)]) if r != b"$-1")
    check(hits == 400, f"exactly --maxkeys remain ({hits}), each thread holding its share")
    client.close()
    port_server.stop()


def test_options_for_shards():
    for options, message in ((["--threads", "4", "--maxkeys", "3", "--appendonly", "no"], "at least --threads"),
                             ):
        result = subprocess.run([SERVER, "--port", "1", *options], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=20)
        check(result.returncode == 2 and message in result.stdout, f"{options} is refused: {result.stdout!r}")


if __name__ == "__main__":
    for name, test in list(globals().items()):
        if name.startswith("test_"):
            test()
    if failures:
        print(f"{len(failures)} checks FAILED")
        sys.exit(1)
    print("threads integration: all checks passed")
