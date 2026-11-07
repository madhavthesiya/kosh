"""Real client libraries against the real server: redis-py and node-redis,
each in RESP2 and RESP3, with and without a password, through their own APIs.

Usage:  python3 tests/integration/test_clients.py <path-to-kosh-server> [unittest args]
CTest runs this too (see CMakeLists.txt).

The libraries aren't part of the repository:
- redis-py: pip install redis (or put its wheel on PYTHONPATH);
- node-redis: npm install redis in some directory, then NODE_PATH=<it>/node_modules.
A missing library skips its tests, saying so. With KOSH_REQUIRE_CLIENTS=1 (as
CI sets), a missing library fails instead, so the suite can't pass by running
nothing.
"""
import os
import shutil
import subprocess
import sys
import unittest

import test_server
from test_server import ServerTestCase

try:
    import redis
except ImportError:
    redis = None

HERE = os.path.dirname(os.path.abspath(__file__))
REQUIRE_CLIENTS = os.environ.get("KOSH_REQUIRE_CLIENTS") == "1"


def need(available, what):
    if available:
        return
    if REQUIRE_CLIENTS:
        raise AssertionError(f"{what} is not installed, and KOSH_REQUIRE_CLIENTS=1")
    raise unittest.SkipTest(f"{what} is not installed")


class RedisPyTest(ServerTestCase):
    def setUp(self):
        need(redis is not None, "redis-py")

    def connect(self, server, protocol, **options):
        client = redis.Redis(host="127.0.0.1", port=server.port, protocol=protocol, **options)
        self.addCleanup(client.close)
        return client

    def test_every_supported_command(self):
        server = self.start_server("--appendonly", "no")
        for protocol in (2, 3):
            with self.subTest(protocol=protocol):
                r = self.connect(server, protocol)
                r.delete("k", "t", "l", "s", "h", "c", "m1", "m2")     # one DEL with several keys
                self.assertTrue(r.ping())
                # Strings and keys
                self.assertTrue(r.set("k", "v"))
                self.assertEqual(r.get("k"), b"v")
                self.assertIsNone(r.get("missing"))
                self.assertTrue(r.set("t", "v", ex=100))
                self.assertIn(r.ttl("t"), (99, 100))
                self.assertTrue(r.execute_command("SETEX", "t", 50, "v"))   # setex() is deprecated
                self.assertEqual(r.exists("k"), 1)
                self.assertEqual(r.type("k"), b"string")
                self.assertTrue(r.expire("k", 50))
                self.assertEqual(r.delete("k"), 1)
                self.assertTrue(r.set("k2", "v"))
                self.assertEqual(r.exists("t", "k2", "missing"), 2)
                self.assertEqual(r.delete("t", "k2", "missing"), 2)
                self.assertTrue(r.mset({"m1": "1", "m2": "2"}))
                self.assertEqual(r.mget("m1", "missing", "m2"), [b"1", None, b"2"])
                # Counters: redis-py's incr() and decr() send INCRBY and DECRBY
                self.assertEqual(r.incr("c"), 1)
                self.assertEqual(r.incr("c", 5), 6)
                self.assertEqual(r.decr("c", 2), 4)
                self.assertEqual(r.execute_command("INCR", "c"), 5)
                self.assertEqual(r.execute_command("DECR", "c"), 4)
                self.assertEqual(r.get("c"), b"4")
                self.assertTrue(r.set(b"\x00\xff", b"\r\n\x00"))        # binary-safe
                self.assertEqual(r.get(b"\x00\xff"), b"\r\n\x00")
                # Lists
                self.assertEqual(r.rpush("l", "a", "b"), 2)
                self.assertEqual(r.lpush("l", "z"), 3)
                self.assertEqual(r.lrange("l", 0, -1), [b"z", b"a", b"b"])
                self.assertEqual(r.lpop("l"), b"z")
                self.assertEqual(r.rpop("l"), b"b")
                self.assertEqual(r.llen("l"), 1)
                # Sets: redis-py gives a Python set from a RESP3 set and a RESP2 array alike
                self.assertEqual(r.sadd("s", "a", "b", "a"), 2)
                self.assertEqual(r.smembers("s"), {b"a", b"b"})
                self.assertTrue(r.sismember("s", "a"))
                self.assertEqual(r.srem("s", "a"), 1)
                self.assertEqual(r.scard("s"), 1)
                # Hashes: a dict from a RESP3 map and a RESP2 array alike
                self.assertEqual(r.hset("h", mapping={"f": "1", "g": "2"}), 2)
                self.assertEqual(r.hget("h", "f"), b"1")
                self.assertEqual(r.hgetall("h"), {b"f": b"1", b"g": b"2"})
                self.assertEqual(r.hdel("h", "f"), 1)
                self.assertEqual(r.hlen("h"), 1)
                # Server and connection
                info = r.info()
                self.assertGreaterEqual(info["connected_clients"], 1)
                self.assertIn("kosh_version", info)
                self.assertIsInstance(r.client_id(), int)
                self.assertTrue(r.client_setname("worker"))
                self.assertIn(r.client_getname(), ("worker", b"worker"))
                # Errors are raised as such
                with self.assertRaisesRegex(redis.ResponseError, "WRONGTYPE"):
                    r.lpush("h", "x")
                with self.assertRaisesRegex(redis.ResponseError, "unknown command"):
                    r.execute_command("NOPE")

    def test_pipelines_and_transactions(self):
        server = self.start_server("--appendonly", "no")
        for protocol in (2, 3):
            with self.subTest(protocol=protocol):
                r = self.connect(server, protocol)
                # pipeline() is a MULTI/EXEC transaction by default
                with r.pipeline() as p:
                    self.assertEqual(p.set("a", "1").get("a").rpush("pl", "x").execute(), [True, b"1", 1])
                with r.pipeline(transaction=False) as p:
                    for i in range(100):
                        p.set(f"p{i}", i)
                    self.assertEqual(p.execute(), [True] * 100)
                # A command that fails as it runs doesn't stop the others
                with r.pipeline() as p:
                    results = p.set("w", "v").lpush("w", "x").get("w").execute(raise_on_error=False)
                self.assertEqual(results[0], True)
                self.assertIsInstance(results[1], redis.ResponseError)
                self.assertEqual(results[2], b"v")
                r.delete("pl")

    def test_password(self):
        server = self.start_server("--appendonly", "no", "--requirepass", "s3cret")
        for protocol in (2, 3):
            with self.subTest(protocol=protocol):
                self.assertTrue(self.connect(server, protocol, password="s3cret").ping())
                self.assertTrue(self.connect(server, protocol, username="default", password="s3cret").ping())
                with self.assertRaises(redis.AuthenticationError):
                    self.connect(server, protocol, password="wrong").ping()
                with self.assertRaises(redis.AuthenticationError):
                    self.connect(server, protocol).ping()


class NodeRedisTest(ServerTestCase):
    def setUp(self):
        self.node = shutil.which("node")
        found = self.node is not None and subprocess.run(
            [self.node, "-e", "require.resolve('redis')"], capture_output=True).returncode == 0
        need(found, "node-redis (node, and redis on NODE_PATH)")

    def check(self, server, resp, *extra):
        result = subprocess.run([self.node, os.path.join(HERE, "node_redis_check.cjs"),
                                 str(server.port), str(resp), *extra],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "ok")

    def test_every_supported_command(self):
        server = self.start_server("--appendonly", "no")
        for resp in (2, 3):
            with self.subTest(resp=resp):
                self.check(server, resp)

    def test_password(self):
        server = self.start_server("--appendonly", "no", "--requirepass", "s3cret")
        for resp in (2, 3):
            with self.subTest(resp=resp):
                self.check(server, resp, "s3cret")
                self.check(server, resp, "wrong", "--expect-auth-failure")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(f"usage: {sys.argv[0]} <path-to-kosh-server> [unittest args]")
    test_server.SERVER_BINARY = os.path.abspath(sys.argv.pop(1))
    unittest.main()
