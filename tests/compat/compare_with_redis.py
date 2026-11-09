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

def fresh(*keys):
    """Deletes the keys one DEL each, so a case starts clean whether or not
    the server takes several keys in one DEL"""
    return b"".join(arr("DEL", k) for k in keys)


# Numbers in arguments and values are read as Redis's string2ll reads them:
# only the canonical form ("5", "-5", "0"), no sign but "-", no leading zero,
# no space, at most 20 bytes
INTEGER_CASES = {
    "EXPIRE 010": fresh("k") + arr("SET", "k", "v") + arr("EXPIRE", "k", "010") + arr("TTL", "k"),
    "EXPIRE -0": fresh("k") + arr("SET", "k", "v") + arr("EXPIRE", "k", "-0") + arr("EXISTS", "k"),
    "EXPIRE +5": fresh("k") + arr("SET", "k", "v") + arr("EXPIRE", "k", "+5") + arr("TTL", "k"),
    "EXPIRE ' 5'": fresh("k") + arr("SET", "k", "v") + arr("EXPIRE", "k", " 5") + arr("TTL", "k"),
    "EXPIRE 0": fresh("k") + arr("SET", "k", "v") + arr("EXPIRE", "k", "0") + arr("EXISTS", "k"),
    "EXPIRE, 20 digits with leading zeros": fresh("k") + arr("SET", "k", "v") + arr("EXPIRE", "k", "0" * 19 + "9"),
    "SET EX 010": fresh("k") + arr("SET", "k", "v", "EX", "010") + arr("TTL", "k"),
    "LRANGE 01": fresh("l") + arr("RPUSH", "l", "a", "b") + arr("LRANGE", "l", "01", "1") + arr("LRANGE", "l", "-0", "1"),
    "LRANGE 2^63": fresh("l") + arr("RPUSH", "l", "a") + arr("LRANGE", "l", 0, "9223372036854775808"),
    "HELLO 03": arr("HELLO", "03"),
}

COUNTER_CASES = {
    "INCR a new key": fresh("c") + arr("INCR", "c") + arr("INCR", "c") + arr("GET", "c"),
    "DECR a new key": fresh("c") + arr("DECR", "c") + arr("GET", "c"),
    "INCRBY, DECRBY": fresh("c") + arr("INCRBY", "c", 10) + arr("DECRBY", "c", 3) + arr("INCRBY", "c", -20) + arr("DECRBY", "c", -1) + arr("GET", "c"),
    "INCR then GET, TYPE": fresh("c") + arr("SET", "c", "41") + arr("INCR", "c") + arr("GET", "c") + arr("TYPE", "c"),
    "INCR abc": fresh("c") + arr("SET", "c", "abc") + arr("INCR", "c") + arr("GET", "c"),
    "INCR an empty string": fresh("c") + arr("SET", "c", "") + arr("INCR", "c"),
    "INCR 05": fresh("c") + arr("SET", "c", "05") + arr("INCR", "c") + arr("GET", "c"),
    "INCR -0": fresh("c") + arr("SET", "c", "-0") + arr("INCR", "c"),
    "INCR +1": fresh("c") + arr("SET", "c", "+1") + arr("INCR", "c"),
    "INCR ' 1' and '1 '": fresh("c", "d") + arr("SET", "c", " 1") + arr("INCR", "c") + arr("SET", "d", "1 ") + arr("INCR", "d"),
    "INCR 1.5": fresh("c") + arr("SET", "c", "1.5") + arr("INCR", "c"),
    "INCR, 21 digits": fresh("c") + arr("SET", "c", "1" + "0" * 20) + arr("INCR", "c"),
    "INCR a list": fresh("l") + arr("RPUSH", "l", "a") + arr("INCR", "l") + arr("DECRBY", "l", 1),
    "INCR at the maximum": fresh("c") + arr("SET", "c", "9223372036854775807") + arr("INCR", "c") + arr("GET", "c"),
    "DECR at the minimum": fresh("c") + arr("SET", "c", "-9223372036854775808") + arr("DECR", "c") + arr("GET", "c"),
    "INCRBY up to the maximum": fresh("c") + arr("SET", "c", "9223372036854775806") + arr("INCRBY", "c", 1) + arr("INCRBY", "c", 1),
    "INCRBY down to the minimum": fresh("c") + arr("INCRBY", "c", "-9223372036854775808") + arr("GET", "c") + arr("DECR", "c"),
    "INCRBY 2^63": fresh("c") + arr("INCRBY", "c", "9223372036854775808"),
    "INCRBY abc, 1.5, 01, +1, empty": fresh("c") + arr("INCRBY", "c", "abc") + arr("INCRBY", "c", "1.5") + arr("INCRBY", "c", "01") + arr("INCRBY", "c", "+1") + arr("INCRBY", "c", ""),
    "DECRBY the minimum": fresh("c") + arr("DECRBY", "c", "-9223372036854775808") + arr("EXISTS", "c"),
    "DECRBY the minimum on a list": fresh("l") + arr("RPUSH", "l", "a") + arr("DECRBY", "l", "-9223372036854775808"),
    "INCRBY abc on a list": fresh("l") + arr("RPUSH", "l", "a") + arr("INCRBY", "l", "abc"),
    "INCR keeps the TTL": fresh("c") + arr("SET", "c", "1", "EX", 100) + arr("INCR", "c") + arr("TTL", "c"),
    "INCRBY 0": fresh("c") + arr("INCRBY", "c", 0) + arr("GET", "c"),
    "wrong arity": arr("INCR") + arr("INCR", "a", "b") + arr("DECR") + arr("INCRBY", "c") + arr("DECRBY", "c", 1, 2),
    "lower case": fresh("c") + arr("incr", "c") + arr("incrby", "c", 5) + arr("decr", "c") + arr("decrby", "c", 2),
    "inside MULTI": fresh("c") + M + arr("INCR", "c") + arr("INCRBY", "c", "x") + arr("INCRBY", "c", 2) + E,
    "RESP3": H3 + fresh("c") + arr("INCR", "c") + arr("INCRBY", "c", "x"),
}

DEL_EXISTS_CASES = {
    "DEL several": fresh("a", "b", "c") + arr("SET", "a", 1) + arr("SET", "b", 2) + arr("DEL", "a", "b", "c") + arr("EXISTS", "a", "b", "c"),
    "DEL a key twice": fresh("a") + arr("SET", "a", 1) + arr("DEL", "a", "a"),
    "EXISTS a key twice": fresh("a", "b") + arr("SET", "a", 1) + arr("EXISTS", "a", "a", "b"),
    "DEL every type": CLEAR + arr("SET", "k", "v") + arr("RPUSH", "l", "a") + arr("SADD", "s", "m") + arr("HSET", "h", "f", "v") + arr("DEL", "k", "l", "s", "h", "x") + arr("EXISTS", "k", "l", "s", "h"),
    "EXISTS every type": CLEAR + arr("SET", "k", "v") + arr("RPUSH", "l", "a") + arr("SADD", "s", "m") + arr("HSET", "h", "f", "v") + arr("EXISTS", "k", "l", "s", "h", "x") + CLEAR,
    "DEL, EXISTS, no keys": arr("DEL") + arr("EXISTS"),
    "DEL, EXISTS, nothing there": fresh("a", "b") + arr("DEL", "a", "b") + arr("EXISTS", "a", "b"),
    "lower case": fresh("a") + arr("set", "a", 1) + arr("exists", "a", "a") + arr("del", "a", "a"),
    "inside MULTI": fresh("a", "b") + M + arr("SET", "a", 1) + arr("SET", "b", 2) + arr("DEL", "a", "b", "c") + arr("EXISTS", "a", "b") + E,
}

MGET_MSET_CASES = {
    "MGET": fresh("a", "b", "c") + arr("SET", "a", 1) + arr("SET", "b", 2) + arr("MGET", "a", "b", "c"),
    "MGET, a list among them": fresh("a", "l") + arr("RPUSH", "l", "x") + arr("SET", "a", 1) + arr("MGET", "a", "l", "a"),
    "MGET, no keys": arr("MGET"),
    "MSET, MGET": fresh("a", "b") + arr("MSET", "a", 1, "b", 2) + arr("MGET", "a", "b"),
    "MSET, a key twice": fresh("a") + arr("MSET", "a", 1, "a", 2) + arr("GET", "a"),
    "MSET, an odd count": fresh("a") + arr("MSET", "a", 1, "b") + arr("EXISTS", "a"),
    "MSET, too few": arr("MSET") + arr("MSET", "a"),
    "MSET over a list": fresh("l") + arr("RPUSH", "l", "x") + arr("MSET", "l", "v") + arr("TYPE", "l") + arr("GET", "l"),
    "MSET clears a TTL": fresh("a") + arr("SET", "a", 1, "EX", 100) + arr("MSET", "a", 2) + arr("TTL", "a"),
    "MSET, empty and binary values": fresh("a", "b") + arr("MSET", "a", "", "b", b"\x00\r\n\xff") + arr("MGET", "a", "b"),
    "lower case": fresh("a") + arr("mset", "a", 1) + arr("mget", "a"),
    "inside MULTI": fresh("a", "b") + M + arr("MSET", "a", 1, "b", 2) + arr("MGET", "a", "b", "c") + E,
    "an odd MSET aborts MULTI": fresh("a") + M + arr("SET", "a", 1) + arr("MSET", "a") + E + arr("EXISTS", "a"),
    "RESP3": H3 + fresh("a", "b") + arr("SET", "b", 2) + arr("MGET", "a", "b"),
}

# PTTL's milliseconds change between asking Redis and asking Kosh, so a key
# with an expiry is checked with TTL (seconds); PTTL only for -1 and -2. Each
# group gets fresh servers, so ALL clears every key this group uses and
# DBSIZE's count is the same on both.
ALL = fresh("k", "l", "a", "b")
EXPIRY_CASES = {
    "PTTL, no key, no expiry": ALL + arr("PTTL", "k") + arr("SET", "k", "v") + arr("PTTL", "k"),
    "PEXPIRE, then TTL": ALL + arr("SET", "k", "v") + arr("PEXPIRE", "k", 100000) + arr("TTL", "k"),
    "PEXPIRE a missing key": ALL + arr("PEXPIRE", "k", 1000) + arr("EXISTS", "k"),
    "PEXPIRE 0 and -1 delete at once": ALL + arr("SET", "a", 1) + arr("SET", "b", 2) + arr("PEXPIRE", "a", 0) + arr("PEXPIRE", "b", -1) + arr("DBSIZE") + arr("EXISTS", "a", "b"),
    "EXPIRE in the past, then DBSIZE": ALL + arr("SET", "a", 1) + arr("SET", "b", 2) + arr("EXPIRE", "a", -5) + arr("DBSIZE"),
    "PEXPIREAT in the past, then DBSIZE": ALL + arr("SET", "a", 1) + arr("PEXPIREAT", "a", 1) + arr("DBSIZE"),
    "PEXPIRE, bad numbers": ALL + arr("SET", "k", "v") + arr("PEXPIRE", "k", "abc") + arr("PEXPIRE", "k", "9223372036854775807") + arr("PEXPIRE", "k", "1.5") + arr("PEXPIRE", "k", "-9223372036854775808") + arr("EXISTS", "k"),
    "PERSIST": ALL + arr("SET", "k", "v", "EX", 100) + arr("PERSIST", "k") + arr("TTL", "k") + arr("PERSIST", "k") + arr("PERSIST", "a"),
    "PERSIST a list": ALL + arr("RPUSH", "l", "x") + arr("EXPIRE", "l", 100) + arr("PERSIST", "l") + arr("PTTL", "l") + arr("TYPE", "l"),
    "SET clears a TTL; PERSIST then has none": ALL + arr("SET", "k", "v", "EX", 100) + arr("SET", "k", "w") + arr("PERSIST", "k"),
    "DBSIZE, empty and counted": ALL + arr("DBSIZE") + arr("SET", "a", 1) + arr("RPUSH", "l", "x") + arr("DBSIZE") + arr("DEL", "a") + arr("DBSIZE"),
    "wrong arity": arr("PTTL") + arr("PTTL", "a", "b") + arr("PEXPIRE", "k") + arr("PEXPIRE", "k", 1, 2) + arr("PERSIST") + arr("PERSIST", "a", "b") + arr("DBSIZE", "x"),
    "lower case": ALL + arr("set", "k", "v") + arr("pexpire", "k", 100000) + arr("ttl", "k") + arr("persist", "k") + arr("pttl", "k") + arr("dbsize"),
    "inside MULTI": ALL + M + arr("SET", "k", "v") + arr("PEXPIRE", "k", 100000) + arr("PERSIST", "k") + arr("PTTL", "k") + arr("DBSIZE") + E,
    "RESP3": H3 + ALL + arr("PTTL", "k") + arr("SET", "k", "v") + arr("PERSIST", "k") + arr("DBSIZE"),
}

# Kosh and Redis hash keys differently, so SCAN returns keys in a different
# order; each case keeps at most one key that could come back. Glob cases
# set one key and ask whether MATCH lets it through.
GLOB_PAIRS = [
    ("h?llo", "hello"), ("h?llo", "hllo"), ("h*llo", "hllo"), ("h*llo", "heeeello"),
    ("h[ae]llo", "hallo"), ("h[ae]llo", "hillo"), ("h[^e]llo", "hallo"), ("h[^e]llo", "hello"),
    ("h[a-b]llo", "hbllo"), ("h[b-a]llo", "hbllo"), ("h\\*llo", "h*llo"), ("h\\*llo", "hello"),
    ("[", "["), ("a[", "a"), ("a[^", "ab"), ("[\\]]", "]"), ("[a-]", "-"), ("[a-]", "a"),
    ("a*", "a"), ("a**", "a"), ("*a", "a"), ("**", "x"), ("a*b*c", "abc"), ("a*b*c", "aXbYc"),
    ("a*b*c", "acb"), ("\\", "\\"), ("abc\\", "abc\\"), (b"[a-\xff]", b"\x80"), (b"a?c", b"a\x00c"),
    ("a*a*a*a*a*a*a*a*a*b", "a" * 40), ("", "x"), ("*x*", "axb"), ("[]a]", "a"), ("[!a]", "!"),
    # The nesting limit: matches at 1,001 levels, not at 1,002 (Ubuntu's 7.0.15)
    ("a*" * 1001, "a" * 1001), ("a*" * 1002, "a" * 1002),
]
GLOB_KEYS = list(dict.fromkeys([k for _, k in GLOB_PAIRS] + ["k", "l", "s"]))
SCAN_CLEAR = fresh(*GLOB_KEYS)
SCAN_CASES = {
    "empty": SCAN_CLEAR + arr("SCAN", 0) + arr("SCAN", 0, "COUNT", 1000),
    "one key": SCAN_CLEAR + arr("SET", "k", "v") + arr("SCAN", 0, "COUNT", 100),
    "cursor forms": SCAN_CLEAR + arr("SCAN", "+0") + arr("SCAN", "-1") + arr("SCAN", "") + arr("SCAN", "18446744073709551615"),
    "invalid cursors": arr("SCAN", "abc") + arr("SCAN", " 0") + arr("SCAN", "0 ") + arr("SCAN", "1x") + arr("SCAN", "18446744073709551616") + arr("SCAN", "0.5"),
    "COUNT": SCAN_CLEAR + arr("SCAN", 0, "COUNT", 0) + arr("SCAN", 0, "COUNT", -1) + arr("SCAN", 0, "COUNT", "abc") + arr("SCAN", 0, "COUNT", "9223372036854775807") + arr("SCAN", 0, "COUNT"),
    "unknown option, missing value": arr("SCAN", 0, "BOGUS", 1) + arr("SCAN", 0, "MATCH") + arr("SCAN", 0, "TYPE") + arr("SCAN", 0, "COUNT", 5, "MATCH"),
    "options in any case and order": SCAN_CLEAR + arr("SET", "k", "v") + arr("scan", 0, "count", 50, "match", "k", "type", "STRING") + arr("SCAN", 0, "TYPE", "string", "MATCH", "nomatch", "MATCH", "k"),
    "MATCH *": SCAN_CLEAR + arr("SET", "k", "v") + arr("SCAN", 0, "MATCH", "*", "COUNT", 100),
    "TYPE": SCAN_CLEAR + arr("RPUSH", "l", "a") + arr("SCAN", 0, "TYPE", "list", "COUNT", 100) + arr("SCAN", 0, "TYPE", "LIST", "COUNT", 100) + arr("SCAN", 0, "TYPE", "string", "COUNT", 100) + arr("SCAN", 0, "TYPE", "bogus", "COUNT", 100),
    "TYPE set and hash": SCAN_CLEAR + arr("SADD", "s", "m") + arr("SCAN", 0, "TYPE", "set", "COUNT", 100) + arr("SCAN", 0, "TYPE", "hash", "COUNT", 100),
    "wrong arity": arr("SCAN"),
    "inside MULTI": SCAN_CLEAR + M + arr("SET", "k", "v") + arr("SCAN", 0, "COUNT", 100) + arr("SCAN", "x") + E,
    "RESP3": H3 + SCAN_CLEAR + arr("SET", "k", "v") + arr("SCAN", 0, "COUNT", 100),
}
for pattern, key in GLOB_PAIRS:
    name = f"MATCH {pattern!r:.30} ({len(pattern)} bytes) on {key!r:.20} ({len(key)} bytes)"
    assert name not in SCAN_CASES, name
    SCAN_CASES[name] = SCAN_CLEAR + arr("SET", key, "v") + arr("SCAN", 0, "MATCH", pattern, "COUNT", 100)

FLUSH_CASES = {
    "FLUSHALL": arr("SET", "a", 1) + arr("RPUSH", "l", "x") + arr("SET", "t", "v", "EX", 100) + arr("FLUSHALL") + arr("DBSIZE") + arr("EXISTS", "a", "l", "t") + arr("SCAN", 0),
    "FLUSHDB": arr("SET", "a", 1) + arr("FLUSHDB") + arr("DBSIZE") + arr("GET", "a"),
    "on an empty server": arr("FLUSHALL") + arr("FLUSHDB") + arr("DBSIZE"),
    "SYNC and ASYNC, any case": arr("SET", "a", 1) + arr("FLUSHALL", "SYNC") + arr("SET", "a", 1) + arr("FLUSHALL", "async") + arr("SET", "a", 1) + arr("FLUSHDB", "Async") + arr("FLUSHDB", "sync") + arr("DBSIZE"),
    "bad arguments": arr("SET", "a", 1) + arr("FLUSHALL", "now") + arr("FLUSHDB", "SYNC", "ASYNC") + arr("FLUSHALL", "") + arr("DBSIZE"),
    "then writes work": arr("SET", "a", 1) + arr("FLUSHALL") + arr("SET", "b", 2) + arr("INCR", "n") + arr("MGET", "a", "b", "n") + arr("DBSIZE"),
    "lower case": arr("set", "a", 1) + arr("flushall") + arr("flushdb") + arr("dbsize"),
    "inside MULTI": arr("SET", "a", 1) + M + arr("FLUSHALL") + arr("SET", "b", 2) + arr("FLUSHDB", "bad") + E + arr("DBSIZE"),
    "RESP3": H3 + arr("SET", "a", 1) + arr("FLUSHALL") + arr("DBSIZE"),
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
    "Integer arguments": (INTEGER_CASES, None),
    "INCR, DECR, INCRBY, DECRBY": (COUNTER_CASES, None),
    "DEL and EXISTS with several keys": (DEL_EXISTS_CASES, None),
    "MGET, MSET": (MGET_MSET_CASES, None),
    "PTTL, PEXPIRE, PERSIST, DBSIZE": (EXPIRY_CASES, None),
    "SCAN": (SCAN_CASES, None),
    "FLUSHALL, FLUSHDB": (FLUSH_CASES, None),
}

# Differences Kosh makes on purpose, and why
EXPECTED = {
    ("Transactions", "WATCH inside MULTI"): "WATCH isn't supported (see the README's Known Limitations)",
    ("PTTL, PEXPIRE, PERSIST, DBSIZE", "wrong arity"): "EXPIRE and PEXPIRE take no options (Redis 7.0's NX, XX, GT, LT): "
        "a third argument is an arity error, not Redis's 'Unsupported option' (see the README's Known Limitations)",
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
        if os.environ.get("KOSH_THREADS"):
            argv += ["--threads", os.environ["KOSH_THREADS"]]
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


def comparable_with_threads(data):
    """With several threads (KOSH_THREADS), keys land on threads at random, so
    the commands below differ from Redis by design, and differently from run to
    run: MSET and MULTI are refused when their keys are on several threads, and
    one SCAN call covers one thread. Such a case is sent to neither server, so
    the two stay in step. tests/integration/test_threads.py covers them."""
    if not os.environ.get("KOSH_THREADS"):
        return True
    upper = data.upper()
    return not any(word in upper for word in (b"MSET", b"MULTI", b"SCAN"))


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
           ("NOPE",), ("GET",), ("MULTI",), ("GET", "a"), ("EXEC",), ("MGET", "a", "b", "l"),
           ("EXISTS", "a", "b", "a"), ("INCR", "n"), ("MSET", "x", 1, "y", 2), ("DEL", "x", "y", "z"),
           ("SET", "e", "v", "PX", 50), ("SCAN", 0, "TYPE", "string", "COUNT", 100)]
COUNTED_AFTER_EXPIRY = [("GET", "e"), ("DEL", "a"), ("LPOP", "l")]
if os.environ.get("KOSH_THREADS"):      # as comparable_with_threads()
    COUNTED = [c for c in COUNTED if c[0] not in ("MSET", "SCAN")]
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
    total_same = total_expected = total_skipped = 0
    unexpected = []
    with tempfile.TemporaryDirectory() as workdir, ThreadPoolExecutor(2) as pool:
        for group, (cases, password) in GROUPS.items():
            redis_proc, redis_port = start(args.redis, "redis", password, workdir)
            kosh_proc, kosh_port = start(args.kosh, "kosh", password, workdir)
            same = 0
            try:
                for name, data in cases.items():
                    if not comparable_with_threads(data):
                        total_skipped += 1
                        continue
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
    if total_skipped:
        print(f"{total_skipped} cases weren't sent to either server: see comparable_with_threads()")
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
