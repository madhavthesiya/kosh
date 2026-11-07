"""Sends the same command sequences to real Redis and to Kosh and compares
the replies byte for byte: what "Redis-compatible" means here, measured.

Usage:
    python3 tests/compat/compare_with_redis.py --kosh build/kosh-server [--redis redis-server] [--verbose]

Each case runs on a fresh connection to each server, started fresh for its
group (with --requirepass secret where the group says so). The fields of
HELLO's reply that differ by design (the server's name, its version, the
connection id) are masked before comparing. Differences Kosh makes on
purpose are listed in EXPECTED with the reason; any other difference fails
the run (exit code 1).

Written against Redis 7.0.15. Standard library only.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor


def arr(*words):
    words = [w if isinstance(w, bytes) else str(w).encode() for w in words]
    return b"*%d\r\n" % len(words) + b"".join(b"$%d\r\n%s\r\n" % (len(w), w) for w in words)


H3, M, E = arr("HELLO", "3"), arr("MULTI"), arr("EXEC")
CLEAR = arr("DEL", "s") + arr("DEL", "l") + arr("DEL", "h") + arr("DEL", "k")

AUTH_CASES = {
    "PING": arr("PING"),
    "GET k": arr("GET", "k"),
    "an unknown command": arr("NOPE"),
    "GET, wrong arity": arr("GET"),
    "AUTH, no arguments": arr("AUTH"),
    "AUTH wrong": arr("AUTH", "wrong"),
    "AUTH secret": arr("AUTH", "secret"),
    "AUTH default secret": arr("AUTH", "default", "secret"),
    "AUTH DEFAULT secret": arr("AUTH", "DEFAULT", "secret"),
    "AUTH nobody secret": arr("AUTH", "nobody", "secret"),
    "AUTH default anything": arr("AUTH", "default", "anything"),
    "AUTH a b c": arr("AUTH", "a", "b", "c"),
    "QUIT": arr("QUIT"),
    "AUTH, SET, GET": arr("AUTH", "secret") + arr("SET", "k", "v") + arr("GET", "k"),
    "AUTH, AUTH wrong, PING": arr("AUTH", "secret") + arr("AUTH", "wrong") + arr("PING"),
    "inline AUTH, PING": b"AUTH secret\r\nPING\r\n",
    "*11 before AUTH": b"*11\r\n",
    "a 16385-byte bulk before AUTH": b"*2\r\n$4\r\nAUTH\r\n$16385\r\n",
    "AUTH, then a *11 header": arr("AUTH", "secret") + b"*11\r\n",
}

RESP3_CASES = {
    "RESP3: GET missing": H3 + arr("GET", "missing"),
    "RESP3: SET, GET": H3 + arr("SET", "k", "v") + arr("GET", "k"),
    "RESP3: LPOP, RPOP missing": H3 + arr("LPOP", "nolist") + arr("RPOP", "nolist"),
    "RESP3: HGET missing": H3 + arr("HGET", "nohash", "f"),
    "RESP3: HGETALL": H3 + CLEAR + arr("HSET", "h", "f", "1") + arr("HGETALL", "h") + arr("HGETALL", "missing"),
    "RESP3: SMEMBERS": H3 + CLEAR + arr("SADD", "s", "a") + arr("SMEMBERS", "s") + arr("SMEMBERS", "missing"),
    "RESP3: LRANGE": H3 + CLEAR + arr("RPUSH", "l", "a", "b") + arr("LRANGE", "l", 0, -1) + arr("LRANGE", "missing", 0, -1),
    "RESP3: integers": H3 + CLEAR + arr("SET", "k", "v") + arr("EXISTS", "k") + arr("TTL", "k") + arr("SISMEMBER", "s", "x"),
    "RESP3: TYPE": H3 + CLEAR + arr("SET", "k", "v") + arr("TYPE", "k") + arr("TYPE", "missing"),
    "RESP3: CONFIG GET": H3 + arr("CONFIG", "GET", "nosuchparameter"),
    "RESP3: errors": H3 + arr("GET") + arr("NOPE") + arr("LPUSH", "k2"),
    "RESP3: CLIENT GETNAME unset": H3 + arr("CLIENT", "GETNAME"),
    "HELLO 4": arr("HELLO", "4"),
    "HELLO x": arr("HELLO", "x"),
    "HELLO 3abc": arr("HELLO", "3abc"),
    "HELLO -1": arr("HELLO", "-1"),
    "HELLO, no arguments": arr("HELLO"),
    "HELLO 3, then HELLO": H3 + arr("HELLO"),
    "HELLO 2, GET missing": arr("HELLO", "2") + arr("GET", "missing"),
    "HELLO 3, HELLO 2, GET missing": H3 + arr("HELLO", "2") + arr("GET", "missing"),
    "HELLO 3 SETNAME": arr("HELLO", "3", "SETNAME", "myname") + arr("CLIENT", "GETNAME"),
    "HELLO 3 SETNAME, a bad name": arr("HELLO", "3", "SETNAME", "a b") + arr("GET", "missing"),
    "HELLO 3 SETNAME AUTH": arr("HELLO", "3", "SETNAME", "n", "AUTH", "default", "x") + arr("CLIENT", "GETNAME"),
    "HELLO 3 AUTH, no password set": arr("HELLO", "3", "AUTH", "default", "x"),
    "HELLO 3 AUTH, another user": arr("HELLO", "3", "AUTH", "nobody", "x"),
    "HELLO 3 AUTH, a missing argument": arr("HELLO", "3", "AUTH", "default"),
    "HELLO 3, a bad option": arr("HELLO", "3", "BOGUS"),
    "hello, lower case": arr("hello", "3", "auth", "default", "x"),
    "CLIENT SETINFO": arr("CLIENT", "SETINFO", "LIB-NAME", "redis-py") + arr("CLIENT", "SETINFO", "LIB-VER", "8.1.0"),
    "CLIENT SETNAME, GETNAME": arr("CLIENT", "SETNAME", "app") + arr("CLIENT", "GETNAME"),
    "CLIENT GETNAME unset": arr("CLIENT", "GETNAME"),
    "CLIENT, an unknown subcommand": arr("CLIENT", "MAINT_NOTIFICATIONS", "ON"),
    "CLIENT, no arguments": arr("CLIENT"),
    "CLIENT SETNAME, no name": arr("CLIENT", "SETNAME"),
    "CLIENT SETNAME, a space": arr("CLIENT", "SETNAME", "a b") + arr("CLIENT", "GETNAME"),
    "CLIENT SETNAME empty clears it": arr("CLIENT", "SETNAME", "x") + arr("CLIENT", "SETNAME", "") + arr("CLIENT", "GETNAME"),
    "CLIENT GETNAME, an extra argument": arr("CLIENT", "GETNAME", "x"),
    "CLIENT ID, an extra argument": arr("CLIENT", "ID", "x"),
    "client setname, lower case": arr("client", "setname", "lc") + arr("client", "getname"),
}

HELLO_PASSWORD_CASES = {
    "HELLO 3 before AUTH": arr("HELLO", "3"),
    "HELLO 3 AUTH wrong": arr("HELLO", "3", "AUTH", "default", "wrong") + arr("PING"),
    "HELLO 3 AUTH right, GET": arr("HELLO", "3", "AUTH", "default", "secret") + arr("GET", "missing"),
    "HELLO, no arguments, before AUTH": arr("HELLO"),
    "HELLO 2 AUTH right": arr("HELLO", "2", "AUTH", "default", "secret") + arr("GET", "missing"),
}

TRANSACTION_CASES = {
    "MULTI SET GET EXEC": arr("DEL", "k") + M + arr("SET", "k", "v") + arr("GET", "k") + E,
    "an empty transaction": M + E,
    "a nested MULTI": M + M + arr("PING") + E,
    "EXEC without MULTI": E,
    "DISCARD without MULTI": arr("DISCARD"),
    "MULTI, DISCARD": arr("DEL", "k") + M + arr("SET", "k", "v") + arr("DISCARD") + arr("GET", "k"),
    "an unknown command aborts": M + arr("SET", "k", "v") + arr("NOPE") + E + arr("EXISTS", "k"),
    "a wrong arity aborts": arr("DEL", "k") + M + arr("SET", "k", "v") + arr("GET") + E + arr("EXISTS", "k"),
    "a runtime error doesn't abort": arr("DEL", "l") + M + arr("SET", "l", "x") + arr("LPUSH", "l", "a") + arr("GET", "l") + E,
    "EXEC after an abort clears it": M + arr("NOPE") + E + E,
    "MULTI with an argument": M + arr("MULTI", "x") + E,
    "QUIT inside MULTI": M + arr("SET", "q", "1") + arr("QUIT") + arr("PING"),
    "EXEC in RESP3": H3 + arr("DEL", "h") + M + arr("HSET", "h", "f", "1") + arr("HGETALL", "h") + arr("GET", "missing") + E,
    "HELLO inside MULTI": M + arr("HELLO", "3") + arr("GET", "missing") + E + arr("GET", "missing"),
    "WATCH inside MULTI": M + arr("WATCH", "k") + E,
    "multi, lower case": arr("multi") + arr("ping") + arr("exec"),
    "DISCARD with an argument": M + arr("DISCARD", "x") + E,
    "PING inside MULTI": M + arr("PING") + arr("PING", "hi") + E,
}

TRANSACTION_PASSWORD_CASES = {
    "MULTI before AUTH": M + arr("PING") + E,
    "AUTH inside MULTI": M + arr("AUTH", "secret") + E + arr("PING"),
}

MESSAGE_CASES = {
    "PING": arr("PING"),
    "PING hi": arr("PING", "hi"),
    "PING, empty": arr("PING", ""),
    "PING a b": arr("PING", "a", "b"),
    "unknown command": arr("NOPE"),
    "unknown command, arguments": arr("NOPE", "a", "b"),
    "unknown command, a long argument": arr("NOPE", "x" * 200),
    "unknown command, many arguments": arr("NOPE", *(["abcdefghij"] * 20)),
    "unknown command, a long name": arr("N" * 200),
    "unknown command, NUL bytes": arr(b"NO\x00PE", b"a\x00b"),
    "unknown command, a line break": arr("NOPE", "line\r\nbreak"),
    "unknown command, inline": b"nope inline args\r\n",
}

# group name -> (cases, server password or None)
GROUPS = {
    "AUTH, no password set": (AUTH_CASES, None),
    "AUTH, password set": (AUTH_CASES, "secret"),
    "RESP3, HELLO, CLIENT": (RESP3_CASES, None),
    "HELLO, password set": (HELLO_PASSWORD_CASES, "secret"),
    "Transactions": (TRANSACTION_CASES, None),
    "Transactions, password set": (TRANSACTION_PASSWORD_CASES, "secret"),
    "PING and error messages": (MESSAGE_CASES, None),
}

# Differences Kosh makes on purpose, and why
EXPECTED = {
    ("Transactions", "WATCH inside MULTI"): "WATCH isn't supported (see the README's Known Limitations)",
}


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_listening(port, proc, name):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            sys.exit(f"{name} exited at startup (code {proc.returncode})")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    sys.exit(f"{name} did not listen on port {port}")


def start(binary, kind, password, workdir):
    port = free_port()
    if kind == "redis":
        argv = [binary, "--port", str(port), "--bind", "127.0.0.1", "--save", "", "--appendonly", "no",
                "--dir", workdir]
    else:
        argv = [binary, "--port", str(port), "--appendonly", "no"]
    if password:
        argv += ["--requirepass", password]
    proc = subprocess.Popen(argv, cwd=workdir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_listening(port, proc, kind)
    return proc, port


def ask(port, data, quiet=0.3):
    """Sends the bytes on a new connection; returns what comes back until the
    server goes quiet, or closes the connection (then marked)."""
    with socket.create_connection(("127.0.0.1", port), timeout=quiet) as s:
        s.sendall(data)
        out = b""
        try:
            while chunk := s.recv(65536):
                out += chunk
            out += b" <closed>"
        except socket.timeout:
            pass
        return out


def normalize(reply):
    # HELLO's server name, version and connection id differ by design
    reply = re.sub(rb"\$6\r\nserver\r\n\$\d+\r\n[^\r]*\r\n", b"$6\r\nserver\r\n<server>", reply)
    reply = re.sub(rb"\$7\r\nversion\r\n\$\d+\r\n[^\r]*\r\n", b"$7\r\nversion\r\n<version>", reply)
    return re.sub(rb"\$2\r\nid\r\n:\d+\r\n", b"$2\r\nid\r\n<id>", reply)


# Commands after which both servers' INFO counters are compared: reads that
# hit and miss on every type, WRONGTYPE, refused commands, a transaction, an
# expiry. The second list runs once the key "e" has expired.
COUNTED = [("SET", "a", "1"), ("GET", "a"), ("GET", "b"), ("EXISTS", "a"), ("EXISTS", "b"),
           ("TYPE", "a"), ("TYPE", "b"), ("TTL", "a"), ("TTL", "b"), ("LPUSH", "l", "x"),
           ("LRANGE", "l", 0, -1), ("LRANGE", "m", 0, -1), ("HSET", "h", "f", "v"), ("HGET", "h", "f"),
           ("HGET", "h", "g"), ("HGET", "z", "f"), ("HGETALL", "h"), ("SMEMBERS", "s"), ("SADD", "s", "m"),
           ("SISMEMBER", "s", "m"), ("SCARD", "s"), ("LLEN", "l"), ("HLEN", "h"), ("GET", "l"),
           ("NOPE",), ("GET",), ("MULTI",), ("GET", "a"), ("EXEC",), ("SET", "e", "v", "PX", 50)]
COUNTED_AFTER_EXPIRY = [("GET", "e"), ("DEL", "a"), ("LPOP", "l")]
COUNTERS = ("total_commands_processed", "keyspace_hits", "keyspace_misses", "expired_keys")


def info_counters(port):
    text = ask(port, arr("INFO", "stats")).decode(errors="replace")
    return {k: int(v) for k, v in (line.split(":", 1) for line in text.split("\r\n") if ":" in line)
            if k in COUNTERS}


def compare_info_counters(redis_bin, kosh_bin, workdir):
    """The counters INFO reports after the same commands, compared one by one"""
    redis_proc, redis_port = start(redis_bin, "redis", None, workdir)
    kosh_proc, kosh_port = start(kosh_bin, "kosh", None, workdir)
    try:
        for port in (redis_port, kosh_port):
            ask(port, b"".join(arr(*c) for c in COUNTED))
        time.sleep(0.2)
        for port in (redis_port, kosh_port):
            ask(port, b"".join(arr(*c) for c in COUNTED_AFTER_EXPIRY))
        redis, kosh = info_counters(redis_port), info_counters(kosh_port)
    finally:
        for proc in (redis_proc, kosh_proc):
            proc.terminate()
            proc.wait()
    differing = [f"{c}: redis {redis.get(c)}, kosh {kosh.get(c)}" for c in COUNTERS if redis.get(c) != kosh.get(c)]
    for line in differing:
        print(f"  DIFFERS   {line}")
    print(f"INFO counters: {len(COUNTERS) - len(differing)} of {len(COUNTERS)} identical "
          f"({', '.join(f'{c} {kosh.get(c)}' for c in COUNTERS)})")
    return len(COUNTERS) - len(differing), len(differing)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--kosh", required=True, help="path to kosh-server")
    parser.add_argument("--redis", default="redis-server", help="path to redis-server")
    parser.add_argument("--verbose", action="store_true", help="also list the identical cases")
    args = parser.parse_args()

    version = subprocess.run([args.redis, "--version"], capture_output=True, text=True).stdout.strip()
    print(f"Redis: {version}\nKosh: {os.path.abspath(args.kosh)}\n")
    total_same = total_expected = 0
    unexpected = []
    with tempfile.TemporaryDirectory() as workdir, ThreadPoolExecutor(2) as pool:
        for group, (cases, password) in GROUPS.items():
            redis_proc, redis_port = start(args.redis, "redis", password, workdir)
            kosh_proc, kosh_port = start(args.kosh, "kosh", password, workdir)
            same = 0
            try:
                for name, data in cases.items():
                    from_redis, from_kosh = pool.map(lambda port: normalize(ask(port, data)),
                                                     (redis_port, kosh_port))
                    if from_redis == from_kosh:
                        same += 1
                        if args.verbose:
                            print(f"  same      {name}")
                    elif (group, name) in EXPECTED:
                        total_expected += 1
                        print(f"  expected  {name}: {EXPECTED[(group, name)]}")
                    else:
                        unexpected.append((group, name))
                        print(f"  DIFFERS   {name}\n    redis {from_redis[:300]!r}\n    kosh  {from_kosh[:300]!r}")
            finally:
                for proc in (redis_proc, kosh_proc):
                    proc.terminate()
                    proc.wait()
            total_same += same
            print(f"{group}: {same} of {len(cases)} identical")
        counters_same, counters_differing = compare_info_counters(args.redis, args.kosh, workdir)
        total_same += counters_same
        unexpected += [("INFO counters", str(i)) for i in range(counters_differing)]
    total = total_same + total_expected + len(unexpected)
    print(f"\nAll: {total_same} of {total} identical, {total_expected} expected difference(s), "
          f"{len(unexpected)} unexpected")
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
