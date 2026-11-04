"""Integration tests: run the real kosh-server binary and talk to it over TCP.

Usage:  python3 tests/integration/test_server.py <path-to-kosh-server> [unittest args]
CTest runs this automatically (see CMakeLists.txt).

Standard library only, so the test suite stays dependency-free. Each test starts
its own server on a free port inside a temporary directory, so tests can't see
each other's data or AOF files.
"""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

try:
    import resource   # POSIX only
except ImportError:
    resource = None

SERVER_BINARY = None  # set from the command line in __main__


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def describe_exit(returncode):
    """Turns -13 into 'killed by SIGPIPE' (POSIX reports signals as negative codes)."""
    if returncode is not None and returncode < 0:
        try:
            return f"killed by {signal.Signals(-returncode).name}"
        except ValueError:
            pass
    return f"exit code {returncode}"


class ServerProcess:
    """A running kosh-server on a free port, working in `workdir`.
    `nofile` limits its open files (POSIX): a soft limit, or (soft, hard).
    `fsize` limits the size of the files it writes, in bytes (soft limit)."""

    def __init__(self, *args, workdir=None, nofile=None, fsize=None):
        self._tmp = None
        if workdir is None:
            self._tmp = tempfile.TemporaryDirectory()
            workdir = self._tmp.name
        self.workdir = workdir
        self.port = free_port()
        # Server output goes to a file, not a pipe: an unread pipe fills up and blocks the server.
        self._log = open(os.path.join(workdir, "server.log"), "w+")
        def set_limits():
            if nofile:
                soft, hard = nofile if isinstance(nofile, tuple) else \
                    (nofile, resource.getrlimit(resource.RLIMIT_NOFILE)[1])
                resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))
            if fsize:
                resource.setrlimit(resource.RLIMIT_FSIZE,
                                   (fsize, resource.getrlimit(resource.RLIMIT_FSIZE)[1]))
        # ctest runs the suite once more with KOSH_POLLER=select, so the
        # select() path stays tested now that Linux uses epoll by default. A
        # test that names a poller itself keeps it.
        poller = os.environ.get("KOSH_POLLER")
        if poller and "--poller" not in args:
            args = (*args, "--poller", poller)
        self.proc = subprocess.Popen(
            [SERVER_BINARY, "--port", str(self.port), *args],
            cwd=workdir, stdout=self._log, stderr=subprocess.STDOUT,
            preexec_fn=set_limits if nofile or fsize else None)
        self._wait_until_listening()

    def _wait_until_listening(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"server exited during startup ({describe_exit(self.proc.returncode)}):\n{self.output()}")
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        self.stop()
        raise RuntimeError(f"server did not listen on port {self.port} within {timeout} s")

    def is_running(self):
        return self.proc.poll() is None

    def cpu_seconds(self):
        """User + system CPU time the server has used so far (Linux /proc)."""
        with open(f"/proc/{self.proc.pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")

    def output(self):
        self._log.flush()
        self._log.seek(0)
        return self._log.read()

    def stop(self):
        if self.is_running():
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self._log.close()
        if self._tmp is not None:
            self._tmp.cleanup()


def exchange_raw(port, data, timeout=2.0):
    """Sends raw bytes and returns everything the server sends back until it closes
    the connection. Raises TimeoutError if it neither replies nor closes."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(data)
        received = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                return received
            received += chunk


class RespError(Exception):
    """An error reply (-ERR ...) from the server."""


class Client:
    """Minimal RESP client: sends commands as RESP arrays, parses one reply per call."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.reader = self.sock.makefile("rb")

    def call(self, *args):
        self.sock.sendall(self.encode(args))
        return self.read_reply()

    @staticmethod
    def encode(args):
        parts = [b"*%d\r\n" % len(args)]
        for arg in args:
            data = arg if isinstance(arg, bytes) else str(arg).encode()
            parts.append(b"$%d\r\n%s\r\n" % (len(data), data))
        return b"".join(parts)

    def read_reply(self):
        line = self.reader.readline()
        if not line.endswith(b"\r\n"):
            raise ConnectionError(f"connection closed mid-reply: {line!r}")
        kind, payload = line[:1], line[1:-2]
        if kind == b"+":
            return payload
        if kind == b"-":
            raise RespError(payload.decode())
        if kind == b":":
            return int(payload)
        if kind == b"$":
            length = int(payload)
            return None if length < 0 else self.reader.read(length + 2)[:-2]
        if kind == b"*":
            return [self.read_reply() for _ in range(int(payload))]
        raise ValueError(f"unexpected reply: {line!r}")

    def close(self):
        self.reader.close()
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class ServerTestCase(unittest.TestCase):
    def start_server(self, *args, workdir=None, nofile=None, fsize=None):
        server = ServerProcess(*args, workdir=workdir, nofile=nofile, fsize=fsize)
        self.addCleanup(server.stop)
        return server


class BasicCommandTest(ServerTestCase):
    def setUp(self):
        self.server = self.start_server("--appendonly", "no")

    def test_ping(self):
        with Client(self.server.port) as c:
            self.assertEqual(c.call("PING"), b"PONG")

    def test_set_get_binary_safe_value(self):
        value = b"hello world\r\nwith a newline"
        with Client(self.server.port) as c:
            self.assertEqual(c.call("SET", "greeting", value), b"OK")
            self.assertEqual(c.call("GET", "greeting"), value)
            self.assertIsNone(c.call("GET", "missing"))

    def test_log_lines_are_written_at_once(self):
        # The log is a file here, as under systemd or docker: each line must
        # reach it while the server runs, not when a buffer fills or at exit.
        # The server logs a connection when it accepts it, before reading the
        # PING, so the line is written by the time PONG arrives.
        with Client(self.server.port) as c:
            self.assertEqual(c.call("PING"), b"PONG")
            self.assertIn("Client connected", self.server.output())

    def test_bgrewriteaof_reports_error_when_aof_is_disabled(self):
        with Client(self.server.port) as c:
            with self.assertRaises(RespError):
                c.call("BGREWRITEAOF")

    def test_save_is_not_supported(self):
        # SAVE wrote only string keys to dump.rdb, and nothing loads that file:
        # its "+OK" promised persistence that didn't exist. The AOF is the
        # server's persistence.
        with Client(self.server.port) as c:
            with self.assertRaises(RespError) as caught:
                c.call("SAVE")
            self.assertEqual(str(caught.exception), "ERR unknown command 'SAVE'")
        self.assertFalse(os.path.exists(os.path.join(self.server.workdir, "dump.rdb")))


class ClientDisconnectTest(ServerTestCase):
    def test_client_closing_before_reading_replies_does_not_kill_server(self):
        # Regression test for SIGPIPE. On Linux, send() to a client that has
        # already closed its socket raises SIGPIPE, whose default action
        # terminates the process: one client could take the server down.
        server = self.start_server("--appendonly", "no")
        for _ in range(5):
            s = socket.create_connection(("127.0.0.1", server.port))
            s.sendall(b"PING\r\n" * 10000)
            s.close()  # close without reading a single reply
        time.sleep(0.5)  # give the server time to answer into the closed sockets

        self.assertTrue(server.is_running(),
                        f"server died ({describe_exit(server.proc.returncode)})\n{server.output()}")
        with Client(server.port) as c:
            self.assertEqual(c.call("PING"), b"PONG")


def send_in_background(sock, data):
    """sendall() from a daemon thread, so a test can't hang if the server stops
    reading. The thread ends when the socket is closed."""
    def run():
        try:
            sock.sendall(data)
        except OSError:
            pass
    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread


class SlowClientTest(ServerTestCase):
    """Replies wait in a per-client buffer and go out as each socket accepts them,
    so one client that doesn't read cannot hold up anyone else."""

    VALUE = b"v" * 10_000

    def setUp(self):
        self.server = self.start_server("--appendonly", "no")
        with Client(self.server.port) as c:
            c.call("SET", "big", self.VALUE)

    def test_client_that_never_reads_does_not_block_others(self):
        # 50,000 GETs of a 10 KB value: 500 MB of replies, far more than the
        # socket buffers hold. With blocking send() the server used to wait
        # forever for this client to read, and stopped serving everyone.
        hog = socket.create_connection(("127.0.0.1", self.server.port))
        self.addCleanup(hog.close)
        send_in_background(hog, Client.encode(("GET", "big")) * 50_000)
        time.sleep(0.5)   # let the server fill this client's buffers

        with Client(self.server.port) as c:
            start = time.monotonic()
            self.assertEqual(c.call("PING"), b"PONG")
            self.assertLess(time.monotonic() - start, 0.1)
        self.assertTrue(self.server.is_running())

    def test_pipelined_replies_all_arrive_in_order(self):
        # 50 MB of replies in one go, read by the client while the server writes.
        # The server stops reading a client whose replies are waiting and must
        # resume once they are sent, or this would hang.
        with Client(self.server.port) as c:
            count = 5_000
            send_in_background(c.sock, Client.encode(("GET", "big")) * count +
                               Client.encode(("PING",)))
            for _ in range(count):
                self.assertEqual(c.read_reply(), self.VALUE)
            self.assertEqual(c.read_reply(), b"PONG")

    def test_quit_closes_after_its_reply(self):
        reply = exchange_raw(self.server.port, b"SET a 1\r\nQUIT\r\nSET b 2\r\n")
        self.assertEqual(reply, b"+OK\r\n+OK\r\n")
        with Client(self.server.port) as c:
            self.assertIsNone(c.call("GET", "b"))   # nothing after QUIT runs

    def test_protocol_error_after_valid_commands(self):
        reply = exchange_raw(self.server.port, b"PING\r\n*abc\r\nPING\r\n")
        self.assertEqual(reply, b"+PONG\r\n-ERR Protocol error: invalid multibulk length\r\n")


def recv_exactly(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            break
        data += chunk
    return data


@unittest.skipIf(resource is None, "needs POSIX resource limits")
class ClientLimitTest(ServerTestCase):
    """More clients than the server can hold get "-ERR max number of clients
    reached" and a clean close, and everyone else carries on."""

    def allow_open_files(self, needed):
        """Lets this test process open `needed` files, or skips the test."""
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        if hard < needed:
            self.skipTest(f"open-file limit {hard} is below {needed}")
        if soft < needed:
            resource.setrlimit(resource.RLIMIT_NOFILE, (needed, hard))
            self.addCleanup(resource.setrlimit, resource.RLIMIT_NOFILE, (soft, hard))

    def connect_and_sort(self, server, count):
        """Opens `count` connections and returns (accepted sockets, number refused).
        Nothing is sent until a connection is sorted: a refused client then has
        the error waiting and nothing unread on the server side, so the server's
        close is a clean FIN, not an RST that could discard the error unread."""
        sockets = []
        self.addCleanup(lambda: [s.close() for s in sockets])
        for _ in range(count):
            sockets.append(socket.create_connection(("127.0.0.1", server.port), timeout=5))
        time.sleep(0.5)   # let the server accept or refuse every one
        self.assertTrue(server.is_running(),
                        f"server died ({describe_exit(server.proc.returncode)})\n"
                        + server.output()[-500:])

        accepted, refused = [], 0
        for s in sockets:
            s.setblocking(False)
            try:
                first = s.recv(4096)
            except BlockingIOError:   # nothing waiting: accepted, ready for commands
                s.setblocking(True)
                accepted.append(s)
                continue
            s.setblocking(True)
            rest = recv_exactly(s, 4096)   # up to the server's close
            self.assertEqual(first + rest, b"-ERR max number of clients reached\r\n")
            refused += 1
        return accepted, refused

    def assertStillServing(self, server, accepted):
        for s in accepted:
            s.sendall(b"PING\r\n")
        for s in accepted:
            self.assertEqual(recv_exactly(s, 7), b"+PONG\r\n")
        # Once some leave, new clients get in again
        for s in accepted[:10]:
            s.close()
        time.sleep(0.2)
        with Client(server.port) as c:
            self.assertEqual(c.call("PING"), b"PONG")

    def test_clients_beyond_select_limit_are_refused(self):
        # select() can only watch sockets numbered below FD_SETSIZE (1024 on
        # Linux). The 1,021st client got socket 1024 and FD_SET on it wrote past
        # the end of fd_set: glibc's _FORTIFY_SOURCE check killed the server, or,
        # in a build without it, memory was silently corrupted.
        self.allow_open_files(1100 + 64)
        # A common default limit: below it, socket 1024 can't even be created, so
        # the server must raise its own limit to be able to refuse anyone.
        server = self.start_server("--appendonly", "no", "--poller", "select", nofile=1024)
        accepted, refused = self.connect_and_sort(server, 1100)
        self.assertGreater(len(accepted), 1000)
        self.assertGreater(refused, 0)
        self.assertStillServing(server, accepted)

    @unittest.skipUnless(sys.platform.startswith("linux"), "epoll is Linux only")
    def test_epoll_serves_clients_beyond_select_limit(self):
        # epoll watches any socket number: the clients select() had to refuse
        # are served, and the server raises its open-file limit to make room
        self.allow_open_files(1100 + 64)
        server = self.start_server("--appendonly", "no", "--poller", "epoll", nofile=1024)
        accepted, refused = self.connect_and_sort(server, 1100)
        self.assertEqual(refused, 0)
        self.assertEqual(len(accepted), 1100)
        self.assertStillServing(server, accepted)

    def test_maxclients_refuses_the_next_client(self):
        # As Redis does at maxclients (10,000 by default)
        server = self.start_server("--appendonly", "no", "--maxclients", "10")
        accepted, refused = self.connect_and_sort(server, 12)
        self.assertEqual(len(accepted), 10)
        self.assertEqual(refused, 2)
        self.assertStillServing(server, accepted)

    @unittest.skipUnless(os.path.exists("/proc/self/stat"), "measures CPU time via /proc")
    def test_out_of_file_descriptors_does_not_spin(self):
        # With a hard limit it can't raise, the server runs out of descriptors
        # before select()'s limit. accept() then fails with EMFILE and the
        # connection stays queued, so select() reported it again at once: the
        # server spun at 100% CPU and logged "accept() failed" thousands of
        # times a second, and the waiting clients never got an answer. Both
        # pollers report a queued connection until it is accepted.
        pollers = ["select", "epoll"] if sys.platform.startswith("linux") else ["select"]
        for poller in pollers:
            with self.subTest(poller=poller):
                server = self.start_server("--appendonly", "no", "--poller", poller, nofile=(64, 64))
                accepted, refused = self.connect_and_sort(server, 100)

                cpu = server.cpu_seconds()
                time.sleep(1)
                self.assertLess(server.cpu_seconds() - cpu, 0.2, "server busy while idle")
                self.assertGreater(len(accepted), 40)
                self.assertGreater(refused, 0)
                self.assertStillServing(server, accepted)
                for s in accepted:
                    s.close()
                server.stop()


@unittest.skipIf(os.name == "nt", "POSIX signals")
class ShutdownTest(ServerTestCase):
    """Ctrl+C (SIGINT) and SIGTERM used to kill the server outright: no handler
    was installed. Now it stops, closes its connections and exits normally."""

    def test_sigterm_and_sigint_shut_down_cleanly(self):
        for sig in (signal.SIGTERM, signal.SIGINT):
            with self.subTest(signal=sig.name):
                server = self.start_server("--appendonly", "no")
                with Client(server.port) as c:
                    self.assertEqual(c.call("PING"), b"PONG")
                    server.proc.send_signal(sig)
                    returncode = server.proc.wait(timeout=5)
                    self.assertEqual(returncode, 0, describe_exit(returncode))
                    self.assertEqual(c.sock.recv(1), b"")   # its connection was closed
                self.assertIn("shutting down", server.output())


class ProtocolErrorTest(ServerTestCase):
    """Malformed input gets "-ERR Protocol error: ..." and the connection is closed,
    as in Redis. It used to be treated as an incomplete command, so the client
    waited forever while its buffer could grow without limit."""

    def setUp(self):
        self.server = self.start_server("--appendonly", "no")

    def assertProtocolError(self, data, reason):
        self.assertEqual(exchange_raw(self.server.port, data),
                         b"-ERR Protocol error: " + reason + b"\r\n")

    def test_non_numeric_array_length(self):
        self.assertProtocolError(b"*abc\r\n", b"invalid multibulk length")

    def test_negative_bulk_length(self):
        self.assertProtocolError(b"*1\r\n$-5\r\n", b"invalid bulk length")

    def test_bulk_length_over_512_mb(self):
        # Used to make the server wait for 2 GB that would never come.
        self.assertProtocolError(b"*1\r\n$2000000000\r\n", b"invalid bulk length")

    def test_trailing_garbage_after_length(self):
        # std::stoi read "3abc" as 3, so this ran a command named "foo".
        self.assertProtocolError(b"*1\r\n$3abc\r\nfoo\r\n", b"invalid bulk length")

    def test_inline_line_over_64_kb(self):
        # Exactly the smallest size that is an error, so the server has read every
        # byte when it closes. Closing with unread data makes the kernel send RST,
        # which can discard the error reply before the client reads it.
        self.assertProtocolError(b"a" * (64 * 1024 + 2), b"too big inline request")

    def test_other_clients_unaffected(self):
        with Client(self.server.port) as c:
            exchange_raw(self.server.port, b"*abc\r\n")
            self.assertEqual(c.call("PING"), b"PONG")


class ErrorReplyTest(ServerTestCase):
    """Error replies can quote client input, which may contain "\\r\\n" or be huge."""

    def setUp(self):
        self.server = self.start_server("--appendonly", "no")

    def test_line_break_in_command_name_keeps_reply_on_one_line(self):
        # A RESP error ends at the first "\r\n". The rest of the name used to be
        # read as the reply to the next command, so the connection fell out of step.
        with Client(self.server.port) as c:
            with self.assertRaises(RespError) as caught:
                c.call(b"bad\r\nname")
            self.assertEqual(str(caught.exception), "ERR unknown command 'bad  name'")
            self.assertEqual(c.call("PING"), b"PONG")

    def test_long_command_name_is_quoted_up_to_128_bytes(self):
        with Client(self.server.port) as c:
            with self.assertRaises(RespError) as caught:
                c.call(b"x" * 10_000)
            self.assertEqual(str(caught.exception), "ERR unknown command '" + "x" * 128 + "'")


class AofDamageTest(ServerTestCase):
    """A crash while writing a record leaves an incomplete record at the end of
    the AOF. Replay used to stop there silently, and the server then appended
    new records after the partial bytes: at the next restart they were read as
    the rest of the broken record, and lost."""

    def test_incomplete_last_record_is_cut_off(self):
        with tempfile.TemporaryDirectory() as workdir:
            aof = os.path.join(workdir, "appendonly.aof")
            first = self.start_server(workdir=workdir)
            with Client(first.port) as c:
                c.call("SET", "a", "1")
                c.call("SET", "b", "2")
            first.stop()
            with open(aof, "r+b") as f:     # as if the server died writing "SET b 2"
                f.truncate(os.path.getsize(aof) - 7)

            second = self.start_server(workdir=workdir)
            with Client(second.port) as c:
                self.assertEqual(c.call("GET", "a"), b"1")
                self.assertIsNone(c.call("GET", "b"))
                c.call("SET", "c", "3")
            self.assertIn("incomplete record", second.output())
            second.stop()

            third = self.start_server(workdir=workdir)
            with Client(third.port) as c:
                self.assertEqual(c.call("GET", "a"), b"1")
                self.assertEqual(c.call("GET", "c"), b"3")
            third.stop()

    def test_damage_before_the_end_refuses_to_start(self):
        # Starting with whatever came before the damage would silently drop
        # everything after it. Refuse, and say where the damage is.
        with tempfile.TemporaryDirectory() as workdir:
            good = Client.encode(("SET", "a", "1"))
            with open(os.path.join(workdir, "appendonly.aof"), "wb") as f:
                f.write(good + b"*2\r\n$3\r\nGET\r\nxx\r\n" + good)
            result = subprocess.run([SERVER_BINARY, "--port", str(free_port())], cwd=workdir,
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"damaged at byte %d" % len(good), result.stderr)


def pipeline(client, commands):
    """Sends all `commands` at once, then reads one reply for each."""
    client.sock.sendall(b"".join(Client.encode(cmd) for cmd in commands))
    return [client.read_reply() for _ in commands]


class AofGroupCommitTest(ServerTestCase):
    """The server writes the AOF records of every command in one pass of its
    event loop with one write(), before sending any of their replies. These
    tests put many commands, from many clients, into the same passes."""

    def test_pipelined_writes_from_many_clients_survive_a_restart(self):
        # RPUSH is not idempotent: a record written twice, or lost, changes a
        # list's length, so this catches duplicates as well as gaps
        clients, per_client = 5, 400
        with tempfile.TemporaryDirectory() as workdir:
            server = self.start_server(workdir=workdir)
            connections = [Client(server.port) for _ in range(clients)]
            try:
                for i, c in enumerate(connections):     # send everything first...
                    c.sock.sendall(b"".join(
                        Client.encode(("SET", f"k{i}:{n}", f"v{n}")) + Client.encode(("RPUSH", f"list{i}", n))
                        for n in range(per_client)))
                for i, c in enumerate(connections):     # ...then read every reply
                    for n in range(per_client):
                        self.assertEqual(c.read_reply(), b"OK")
                        self.assertEqual(c.read_reply(), n + 1)
            finally:
                for c in connections:
                    c.close()
            server.stop()

            restarted = self.start_server(workdir=workdir)
            with Client(restarted.port) as c:
                for i in range(clients):
                    self.assertEqual(c.call("LLEN", f"list{i}"), per_client)
                    for n in (0, per_client // 2, per_client - 1):
                        self.assertEqual(c.call("GET", f"k{i}:{n}"), f"v{n}".encode())
            restarted.stop()

    def test_bgrewriteaof_in_the_middle_of_a_pipeline(self):
        # The rewrite's snapshot includes the first RPUSH, whose record is still
        # buffered: it must not also be written after the new file, or replay
        # would apply it twice. The second RPUSH comes after the snapshot and
        # must be kept.
        with tempfile.TemporaryDirectory() as workdir:
            server = self.start_server(workdir=workdir)
            with Client(server.port) as c:
                replies = pipeline(c, [("RPUSH", "l", "a"), ("BGREWRITEAOF",), ("RPUSH", "l", "b")])
                self.assertEqual(replies, [1, b"OK", 2])
            server.stop()

            restarted = self.start_server(workdir=workdir)
            with Client(restarted.port) as c:
                self.assertEqual(c.call("LRANGE", "l", 0, -1), [b"a", b"b"])
            restarted.stop()


@unittest.skipIf(resource is None or not hasattr(resource, "prlimit"), "needs Linux prlimit")
class AofWriteErrorTest(ServerTestCase):
    """A full disk, played by a 4 KB limit on the size of the files the server
    writes: writes past it fail with EFBIG, as they would with ENOSPC."""

    def test_writes_are_refused_until_the_aof_can_be_written(self):
        # The server used to ignore the failed write and reply OK, so the AOF
        # silently lost every write from then on. (Without SIGXFSZ ignored, the
        # first write past the limit killed the server instead.)
        value = b"v" * 100
        with tempfile.TemporaryDirectory() as workdir:
            server = self.start_server(workdir=workdir, fsize=4096)
            with Client(server.port) as c:
                acknowledged, refusal = [], None
                for i in range(100):     # about 130 bytes each: 4 KB holds about 30
                    try:
                        c.call("SET", f"k{i}", value)
                        acknowledged.append(i)
                    except RespError as e:
                        refusal = str(e)
                        break
                self.assertIsNotNone(refusal, "every SET was acknowledged")
                self.assertEqual(refusal, "MISCONF Errors writing to the AOF file: File too large")
                self.assertEqual(c.call("GET", "k0"), value)     # reads still work

                # Room on the disk again: what was waiting is written, writes resume
                hard = resource.getrlimit(resource.RLIMIT_FSIZE)[1]
                resource.prlimit(server.proc.pid, resource.RLIMIT_FSIZE, (hard, hard))
                self.assertEqual(c.call("SET", "after", "1"), b"OK")
            server.stop()

            restarted = self.start_server(workdir=workdir)
            with Client(restarted.port) as c:
                for i in acknowledged:
                    self.assertEqual(c.call("GET", f"k{i}"), value, f"k{i} was acknowledged")
                self.assertEqual(c.call("GET", "after"), b"1")
            restarted.stop()

    def test_appendfsync_always_stops_the_server_when_the_aof_fails(self):
        # With "always", a reply means the write is on the disk. Once a write
        # fails, that can't be promised any more: the server must not reply,
        # and stops with exit code 1 (Redis exits too), instead of refusing
        # writes with MISCONF as it does with "everysec" or "no".
        value = b"v" * 100
        with tempfile.TemporaryDirectory() as workdir:
            server = self.start_server("--appendfsync", "always", workdir=workdir, fsize=4096)
            acknowledged = 0
            with Client(server.port) as c:
                with self.assertRaises((ConnectionError, OSError)):
                    for i in range(100):    # about 30 fit in 4 KB
                        self.assertEqual(c.call("SET", f"k{i}", value), b"OK")
                        acknowledged += 1
            self.assertGreater(acknowledged, 0)
            self.assertLess(acknowledged, 100)
            self.assertEqual(server.proc.wait(timeout=5), 1)
            self.assertIn("with --appendfsync always a reply must mean the write is on disk",
                          server.output())
            server.stop()

            # Every write that was acknowledged is on the disk
            restarted = self.start_server(workdir=workdir)
            with Client(restarted.port) as c:
                for i in range(acknowledged):
                    self.assertEqual(c.call("GET", f"k{i}"), value, f"k{i} was acknowledged")
            restarted.stop()


class CommandLineTest(ServerTestCase):
    def run_server(self, *args):
        with tempfile.TemporaryDirectory() as workdir:
            return subprocess.run([SERVER_BINARY, *args], cwd=workdir, capture_output=True, timeout=5)

    def test_help_exits_zero(self):
        result = self.run_server("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn(b"--maxkeys", result.stdout)

    def test_invalid_arguments_exit_with_usage_error(self):
        cases = [
            ("--port", "0"), ("--port", "65536"), ("--port", "80abc"), ("--port", "-1"),
            ("--port",), ("--maxkeys", "0"), ("--maxkeys", "ten"),
            ("--appendonly", "maybe"), ("--appendfilename", ""), ("--bogus",),
            ("--appendfsync", "sometimes"), ("--appendfsync",),
            ("--poller", "kqueue"), ("--maxclients", "0"), ("--maxclients", "many"),
            ("--eviction", "fifo"), ("--eviction",),
        ]
        for args in cases:
            with self.subTest(args=args):
                result = self.run_server(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertTrue(result.stderr, "expected an error message on stderr")

    def test_unopenable_aof_file_fails_fast(self):
        # Running without persistence while the user believes it is on would be worse than not starting.
        result = self.run_server("--appendfilename", os.path.join("no", "such", "dir", "x.aof"))
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"Cannot open AOF file", result.stderr)

    def test_maxkeys_limits_key_count(self):
        server = self.start_server("--appendonly", "no", "--maxkeys", "2")
        with Client(server.port) as c:
            for key in ("a", "b", "c"):
                c.call("SET", key, "1")
            self.assertIsNone(c.call("GET", "a"))  # least recently used, so evicted
            self.assertEqual(c.call("GET", "c"), b"1")

    def test_eviction_option_picks_the_policy(self):
        # Both keys read, b then a, and c needs room. LRU evicts b, the less
        # recently used; SIEVE evicts a, the oldest: a hit doesn't move a key.
        # Without --eviction, the server uses SIEVE.
        for policy, evicted, kept in (("lru", "b", "a"), ("sieve", "a", "b"), (None, "a", "b")):
            with self.subTest(policy=policy):
                flags = ("--eviction", policy) if policy else ()
                server = self.start_server("--appendonly", "no", "--maxkeys", "2", *flags)
                with Client(server.port) as c:
                    c.call("SET", "a", "1")
                    c.call("SET", "b", "1")
                    c.call("GET", "b")
                    c.call("GET", "a")
                    c.call("SET", "c", "1")
                    self.assertEqual(c.call("EXISTS", evicted), 0)
                    self.assertEqual(c.call("EXISTS", kept), 1)
                server.stop()

    def test_appendonly_no_writes_no_file(self):
        server = self.start_server("--appendonly", "no")
        with Client(server.port) as c:
            c.call("SET", "k", "v")
        self.assertFalse(os.path.exists(os.path.join(server.workdir, "appendonly.aof")))

    def test_appendfilename_is_written_and_replayed_after_restart(self):
        with tempfile.TemporaryDirectory() as workdir:
            first = self.start_server("--appendfilename", "custom.aof", workdir=workdir)
            with Client(first.port) as c:
                c.call("SET", "k", "v")
                c.call("RPUSH", "list", "x", "y")
            first.stop()
            self.assertTrue(os.path.exists(os.path.join(workdir, "custom.aof")))

            second = self.start_server("--appendfilename", "custom.aof", workdir=workdir)
            with Client(second.port) as c:
                self.assertEqual(c.call("GET", "k"), b"v")
                self.assertEqual(c.call("LRANGE", "list", 0, -1), [b"x", b"y"])
            second.stop()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(f"usage: {sys.argv[0]} <path-to-kosh-server> [unittest args]")
    SERVER_BINARY = os.path.abspath(sys.argv.pop(1))
    unittest.main()
