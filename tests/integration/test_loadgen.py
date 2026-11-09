#!/usr/bin/env python3
"""kosh-loadgen against a real kosh-server. The central check: freeze the server
for a known time and compare what the tool reports with what arithmetic says.

Usage: test_loadgen.py path/to/kosh-loadgen path/to/kosh-server
"""

import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_server
from test_server import ServerProcess

LOADGEN = None  # set from the command line in __main__


def run_loadgen(port, *args, timeout=60):
    """Runs the tool and returns (exit status, parsed JSON or None, stdout)."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "out.json")
        done = subprocess.run(
            [LOADGEN, "--port", str(port), "--json", path, "--keyspace", "1000", *map(str, args)],
            capture_output=True, text=True, timeout=timeout)
        data = None
        if os.path.exists(path):
            with open(path) as f:
                data = json.load(f)
        return done.returncode, data, done.stdout


def percentile(rows, name):
    return next(row for row in rows if row["name"] == name)


class StallTest(unittest.TestCase):
    def _stall_run(self, rate, duration, warmup, drain):
        server = ServerProcess()
        try:
            measured = {}

            def stall():
                time.sleep(warmup + 1.5)
                os.kill(server.proc.pid, signal.SIGSTOP)
                begin = time.monotonic()
                time.sleep(0.4)
                measured["stall"] = time.monotonic() - begin
                os.kill(server.proc.pid, signal.SIGCONT)

            staller = threading.Thread(target=stall)
            staller.start()
            status, data, out = run_loadgen(
                server.port, "--rate", rate, "--duration", duration, "--warmup", warmup, "--drain", drain)
            staller.join()
        finally:
            server.proc.send_signal(signal.SIGCONT)
            server.stop()
        return status, data, out, measured["stall"]

    def test_known_stall_matches_the_arithmetic(self):
        """Requests due during a stall of S seconds wait from their due time to
        its end, so their latencies spread evenly over 0..S. In T measured
        seconds a fraction S/T of all requests is in that ramp, so the
        percentile p is about S - (1 - p) * T."""
        rate, duration, warmup, drain = 5000, 4, 1, 5
        # On a busy machine the generator itself can run late, and the tool
        # then refuses the percentile: the self-check working, and no evidence
        # about the server. Rerun only for that measured reason.
        for _ in range(3):
            status, data, out, stall_s = self._stall_run(rate, duration, warmup, drain)
            if data is None or not any(r["status"] == "client_late" for r in data["intended"]):
                break

        self.assertEqual(status, 0, out)
        self.assertEqual(data["unanswered"], 0)
        self.assertEqual(data["answered"], rate * duration)

        max_s = data["max_ns"] / 1e9
        self.assertGreaterEqual(max_s, stall_s - 0.02, out)
        self.assertLessEqual(max_s, stall_s + 0.25, out)

        for name, p in (("p99", 0.99), ("p99.9", 0.999)):
            row = percentile(data["intended"], name)
            self.assertEqual(row["status"], "ok", out)
            expected = stall_s - (1 - p) * duration
            self.assertLess(abs(row["value_ns"] / 1e9 - expected), 0.10 * stall_s + 0.03,
                            f"{name}: expected about {expected:.3f} s\n{out}")
            self.assertEqual(row["windows_above"], 1, out)

        # The median is a request nowhere near the stall
        p50 = percentile(data["intended"], "p50")["value_ns"] / 1e9
        self.assertLess(p50, 0.05, out)


    def test_full_socket_still_counts_from_the_schedule(self):
        """In a plain stall the kernel keeps accepting requests, so the send
        time and the intended time agree. Here large values fill the socket
        buffers early in the stall, so later requests wait inside the
        generator. Timing from the send would lose exactly that wait, which is
        what makes this the case that separates the two clocks.
        p90 is a request due 0.4 s into the stall, well after the buffers fill."""
        rate, duration, warmup = 1000, 4, 1
        server = ServerProcess("--appendonly", "no")
        try:
            measured = {}

            def stall():
                time.sleep(warmup + 1.5)
                os.kill(server.proc.pid, signal.SIGSTOP)
                begin = time.monotonic()
                time.sleep(1.0)
                measured["stall"] = time.monotonic() - begin
                os.kill(server.proc.pid, signal.SIGCONT)

            staller = threading.Thread(target=stall)
            staller.start()
            status, data, out = run_loadgen(
                server.port, "--rate", rate, "--duration", duration, "--warmup", warmup, "--drain", 10,
                "--connections", 2, "--set-ratio", 1, "--value-size", 100000, "--keyspace", 100)
            staller.join()
        finally:
            server.proc.send_signal(signal.SIGCONT)
            server.stop()

        self.assertEqual(status, 0, out)
        intended = percentile(data["intended"], "p90")
        actual = percentile(data["actual"], "p90")
        self.assertEqual(intended["status"], "ok", out)
        # The server clears the backlog while new requests keep arriving, and
        # 100 KB values make that take a while. With load R/C = rho the latency
        # falls from S to 0 over S/(1-rho), so p is S - (1-p)*T*(1-rho): from
        # S - (1-p)*T if the backlog clears at once, up to S if it barely does.
        stall_s = measured["stall"]
        seen = intended["value_ns"] / 1e9
        self.assertGreaterEqual(seen, stall_s - 0.1 * duration - 0.03, out)
        self.assertLessEqual(seen, stall_s + 0.05, out)
        self.assertLess(actual["value_ns"], intended["value_ns"] / 2, out)

    def test_each_reply_is_timed_from_its_own_slot(self):
        """At a low rate the connections' slots are far apart (50 ms here), so
        matching a reply to the wrong slot would add tens of milliseconds."""
        server = ServerProcess("--appendonly", "no")
        try:
            status, data, out = run_loadgen(server.port, "--rate", 20, "--duration", 6, "--warmup", 0,
                                            "--connections", 4, "--no-prefill")
        finally:
            server.stop()
        self.assertEqual(status, 0, out)
        self.assertLess(percentile(data["intended"], "p50")["value_ns"] / 1e9, 0.02, out)


class RefusalTest(unittest.TestCase):
    def test_too_few_samples_are_refused(self):
        server = ServerProcess()
        try:
            status, data, out = run_loadgen(server.port, "--rate", 200, "--duration", 2, "--warmup", 0,
                                            "--connections", 2)
        finally:
            server.stop()
        self.assertEqual(status, 0, out)
        self.assertEqual(data["answered"], 400)
        self.assertEqual(percentile(data["intended"], "p50")["status"], "ok", out)
        self.assertEqual(percentile(data["intended"], "p99")["status"], "too_few_samples", out)
        self.assertEqual(percentile(data["intended"], "p99.9")["status"], "too_few_samples", out)
        self.assertIn("N/A: needs 100000 samples", out)

    def test_unanswered_requests_refuse_the_tail(self):
        server = ServerProcess()
        try:
            def freeze():
                time.sleep(1.0)
                os.kill(server.proc.pid, signal.SIGSTOP)

            freezer = threading.Thread(target=freeze)
            freezer.start()
            status, data, out = run_loadgen(server.port, "--rate", 1000, "--duration", 2, "--warmup", 0,
                                            "--drain", 1, "--no-prefill")
            freezer.join()
        finally:
            server.proc.send_signal(signal.SIGCONT)
            server.stop()
        self.assertEqual(status, 3, out)
        self.assertGreater(data["unanswered"], 500, out)
        self.assertEqual(percentile(data["intended"], "p99")["status"], "unfinished", out)
        self.assertIn("max     over", out)


class FailureTest(unittest.TestCase):
    def test_nothing_listening(self):
        port = test_server.free_port()
        done = subprocess.run([LOADGEN, "--port", str(port), "--rate", "100", "--duration", "1"],
                              capture_output=True, text=True, timeout=10)
        self.assertEqual(done.returncode, 2)

    def test_error_replies_are_counted(self):
        server = ServerProcess("--requirepass", "secret")
        try:
            status, data, out = run_loadgen(server.port, "--rate", 500, "--duration", 1, "--warmup", 0,
                                            "--no-prefill", "--connections", 2)
        finally:
            server.stop()
        self.assertEqual(status, 3, out)
        self.assertEqual(data["errors"], 500, out)
        self.assertEqual(data["answered"], 500)

    def test_prefill_failure_stops_the_run(self):
        server = ServerProcess("--requirepass", "secret")
        try:
            done = subprocess.run([LOADGEN, "--port", str(server.port), "--rate", "100", "--duration", "1"],
                                  capture_output=True, text=True, timeout=10)
        finally:
            server.stop()
        self.assertEqual(done.returncode, 2)
        self.assertIn("prefill failed", done.stderr)

    def test_several_instances_each_get_their_own_keys(self):
        first, second = ServerProcess(), ServerProcess()
        try:
            status, data, out = run_loadgen(first.port, "--ports", f"{first.port},{second.port}", "--rate", 2000,
                                            "--duration", 1, "--warmup", 0, "--connections", 4, "--set-ratio", 0)
            self.assertEqual(status, 0, out)
            with test_server.Client(first.port) as a, test_server.Client(second.port) as b:
                self.assertEqual(a.call("DBSIZE"), 500)
                self.assertEqual(b.call("DBSIZE"), 500)
                self.assertIsNotNone(a.call("GET", "key:000000000000"))
                self.assertIsNone(a.call("GET", "key:000000000001"))
                self.assertIsNotNone(b.call("GET", "key:000000000001"))
                self.assertIsNone(b.call("GET", "key:000000000000"))
                # Every GET found its key, on both: none went to the wrong instance
                for client in (a, b):
                    fields = dict(line.split(":", 1) for line in client.call("INFO", "stats").decode().split("\r\n") if ":" in line)
                    self.assertGreater(int(fields["keyspace_hits"]), 500)
                    self.assertLessEqual(int(fields["keyspace_misses"]), 1)
        finally:
            first.stop()
            second.stop()

    def test_bad_arguments(self):
        for args in (["--rate", "0"], ["--rate", "-5"], ["--rate", "100", "--connections", "1", "--threads", "2"],
                     ["--rate", "100", "--set-ratio", "2"], ["--nonsense", "1"],
                     ["--rate", "100", "--ports", "1,2,3", "--connections", "2"],
                     ["--rate", "100", "--ports", "1,2", "--keyspace", "1"]):
            done = subprocess.run([LOADGEN, *args], capture_output=True, text=True, timeout=10)
            self.assertEqual(done.returncode, 1, args)


if __name__ == "__main__":
    LOADGEN = sys.argv[1]
    test_server.SERVER_BINARY = sys.argv[2]
    unittest.main(argv=[sys.argv[0]], verbosity=2)
