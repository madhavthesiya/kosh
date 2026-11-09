<p align="center">
  <a href="../README.md"><picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark.svg">
    <img alt="Kosh" src="assets/logo-light.svg" height="32">
  </picture></a>
</p>

<p align="center">
  <a href="RUNNING.md">Running</a> &nbsp;·&nbsp;
  <a href="ARCHITECTURE.md">Architecture</a> &nbsp;·&nbsp;
  <a href="DECISIONS.md">Decisions</a> &nbsp;·&nbsp;
  <a href="COMMANDS.md">Commands</a> &nbsp;·&nbsp;
  <b>Compatibility</b> &nbsp;·&nbsp;
  <a href="TESTING.md">Testing</a> &nbsp;·&nbsp;
  <a href="BENCHMARKS.md">Benchmarks</a> &nbsp;·&nbsp;
  <a href="HISTORY.md">History</a>
</p>

# Compatibility and limitations

What Kosh does the same as Redis, measured, and what it doesn't do.

**On this page:** [Compatibility with Redis](#compatibility-with-redis) &nbsp;·&nbsp; [Known limitations](#known-limitations)

## Compatibility with Redis

Measured, not assumed: [`tests/compat/compare_with_redis.py`](../tests/compat/compare_with_redis.py) sends the same command sequences to Redis 7.0.15 and to Kosh, and compares the replies byte for byte. `ctest` runs it wherever `redis-server` is installed.

| What | Identical to Redis 7.0.15 |
|---|---:|
| `AUTH`, without and with a password | 19 of 19, and 19 of 19 |
| RESP3, `HELLO`, `CLIENT` | 39 of 39 |
| `HELLO` with a password | 5 of 5 |
| Transactions (`MULTI`, `EXEC`, `DISCARD`) | 17 of 18; the other uses `WATCH`, not supported |
| Transactions with a password | 2 of 2 |
| `PING` and error messages | 12 of 12 |
| Integer arguments (`010`, `-0`, `+5` are refused, as Redis refuses them) | 10 of 10 |
| `INCR`, `DECR`, `INCRBY`, `DECRBY`: the edges of 64 bits, values that aren't integers | 28 of 28 |
| `DEL` and `EXISTS` with several keys | 9 of 9 |
| `MGET`, `MSET` | 14 of 14 |
| `PTTL`, `PEXPIRE`, `PERSIST`, `DBSIZE` (times already past, bad numbers, inside `MULTI`, RESP3) | 14 of 15; the other gives `PEXPIRE` an option, not supported |
| `SCAN`: 36 `MATCH` patterns (ranges, escapes, unclosed brackets, NUL and high bytes, the depth limit), cursor forms, `COUNT`, `TYPE`, errors | 49 of 49 |
| `FLUSHALL`, `FLUSHDB`: `SYNC`/`ASYNC`, bad arguments, an empty server, inside `MULTI`, RESP3 | 9 of 9 |
| `INFO`'s counters after the same commands | 4 of 4 |

`HELLO`'s server name, version and connection id are masked before comparing: they differ by design.

**At 4 threads**, 169 of 170 cases are identical to Redis. Cases using `MSET`, `MULTI` or `SCAN` aren't sent to either server there, as those differ by design and depend on which thread a key lands on; [`tests/integration/test_threads.py`](../tests/integration/test_threads.py) covers them.

**Redis's whole command set**, by version: CI runs tair-opensource's [resp-compatibility](https://github.com/tair-opensource/resp-compatibility) suite (pinned at `5e90c25`) against Kosh on every push. It measures how much of each Redis version a server covers, so most of it tests commands Kosh doesn't have:

| Redis version | 1.0.0 | 2.8.0 | 3.2.0 | 4.0.0 | 5.0.0 | 6.0.0 | 6.2.0 | 7.0.0 | 7.2.0 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Tests passed | 28 of 50 | 48 of 150 | 48 of 184 | 51 of 196 | 51 of 220 | 51 of 228 | 54 of 295 | 54 of 350 | 54 of 352 |

Every failure was checked. All but 13 test commands Kosh doesn't have (sorted sets, streams, Pub/Sub, `KEYS`, `RENAME` and more). The 13 that test a command Kosh has use an option it doesn't support (`EXPIRE`/`PEXPIRE`/`PEXPIREAT` with `NX`/`XX`/`GT`/`LT`; `SET` with `NX`/`XX`/`GET`/`KEEPTTL`; `LPOP`/`RPOP` with a count), or set up their key with a missing command (`scan with TYPE` starts with `GEOADD`). All are listed under [Known limitations](#known-limitations).

**Deep patterns:** a glob matcher recurses once per `*` it has to try, so a client's pattern of 100,000 `a*` could run a server's stack out. Upstream Redis 7.0.15's source has no limit; the Redis 7.0.15 that Ubuntu 24.04 ships carries a later fix that stops at 1,000 levels, and Kosh stops at the same depth ([`Glob.h`](../src/Glob.h)). Measured on both: `a*` × 1,001 matches a key of 1,001 `a`s, × 1,002 doesn't; the comparison above includes both.

**Client libraries**, tested through their own APIs ([`tests/integration/test_clients.py`](../tests/integration/test_clients.py)):

| Client | Version | RESP2 | RESP3 | With a password |
|---|---|:-:|:-:|:-:|
| redis-py | 8.1.0 | Yes | Yes | Yes |
| node-redis | 6.3.1 | Yes | Yes | Yes |
| `redis-cli`, `redis-benchmark` | 7.0.15 | Yes | – | – |

Both libraries open every connection with `HELLO 3`, and gave up on Kosh before it had one; redis-py's `pipeline()` needs `MULTI`/`EXEC`.

**Different from Redis on purpose:**
- Kosh listens on 127.0.0.1 by default; Redis, without a config file, listens on every interface.
- `INFO` has no `redis_version`.
- Keys are limited by `--maxkeys`, not `maxmemory`. The AOF is the only persistence: there are no RDB snapshots.
- `BGREWRITEAOF` is refused inside `MULTI`.

**Not supported yet:** `WATCH`, `KEYS`, `SSCAN`/`HSCAN`, `SELECT` (one database), sorted sets, Pub/Sub, Lua scripts, streams, IPv6, TLS. A command Kosh doesn't have gets Redis's unknown-command error.

## Known limitations

Found while reviewing the code. Each fix starts with a test that reproduces the problem.

**Networking**

| Limitation | Planned fix |
|---|---|
| A command that arrives over many reads is parsed again from its start after each read. Nothing is copied, but a command with very many arguments costs O(n²) to scan. Redis keeps the parser's state between reads instead. | Parser that resumes where it stopped |
| After a protocol error, the server closes the connection at once. If the client is still sending, unread data makes the kernel answer with RST, which can discard the error reply before the client reads it (Redis behaves the same way). | Read and discard the rest before closing (a lingering close) |

**Persistence**

| Limitation | Planned fix |
|---|---|
| With `everysec` or `no`, the write command that first hits a failed AOF write has already changed the data, so it gets its normal reply, but its record only waits in memory. If the server stops before the disk has room, that write is lost. Redis does the same; use `--appendfsync always` to make the server stop instead. | None: it is the trade-off those policies make |
| With `everysec`, the event loop's `write()` of the AOF can wait behind a slow background sync of the same file, and a sync longer than a second widens what a power cut can lose. Redis holds back its own AOF write (for up to 2 s) while a sync is still running, for the first reason. Not measured here. | Measure on a slow disk, then hold back the write as Redis does |
| `BGREWRITEAOF` runs in the foreground: every client waits while the whole data set is written. | A background rewrite in a `fork()`ed child |
| A record whose length field is damaged into a larger number looks like an incomplete last record. Startup then cuts it off with everything after it, with a warning giving the number of bytes. Nothing in the file tells the two apart; Redis has the same ambiguity. | None yet; a checksum per record would tell them apart |
| Replay reads the whole AOF into memory before running it, so startup briefly needs memory for the file as well as the data. | Read it in fixed-size chunks |
| The interactive CLI (`kosh-cli`) still has `SAVE` and `LOAD`, in a text format that holds string keys only and isn't binary-safe. `LOAD` isn't logged to the AOF, so the next start goes back to the data from before it. | Remove them, as in the server |

**Commands and storage**

| Limitation | Planned fix |
|---|---|
| `SET`'s `NX`, `XX`, `GET` and `KEEPTTL` options are not supported (they return a syntax error rather than being ignored). | Implement them, with tests |
| `EXPIRE`, `PEXPIRE` and `PEXPIREAT` take no options (Redis 7.0's `NX`, `XX`, `GT`, `LT`): a third argument gets "wrong number of arguments", where Redis would apply the option or say "Unsupported option". | Implement them, with tests |
| `LPOP` and `RPOP` take no count (Redis 6.2's `LPOP key count`, popping several at once). | Implement it, with tests |
| `WATCH` (optimistic locking for transactions) is not supported: it gets "unknown command", which inside `MULTI` aborts the transaction. | A hook that sees every change to a watched key: writes, deletes, expiry and eviction |
| `BGREWRITEAOF` is refused inside `MULTI`, which Redis allows. | A background rewrite, which would also lift this |
| The key table never shrinks: after many deletes, its buckets stay allocated (64 bytes per 7 slots), and so do spare child buckets. | Shrink incrementally when it is mostly empty, as Redis does |
| On systems other than Linux and macOS (Windows), the key table's big arrays come from `calloc` and are freed at once, so growth can stall for longer there (Challenge 15). | Map and release them with the OS's own calls |

**Several threads** (`--threads` above 1, Linux with `epoll` only)

| Limitation | Planned fix |
|---|---|
| `MSET` and `MULTI`/`EXEC` with keys on several threads are refused with `-CROSSSLOT`; a `{tag}` keeps keys together. | None planned: it would need a locking protocol and a commit across files |
| `MGET`, `DEL` and `EXISTS` across threads aren't atomic as a whole, and neither is `FLUSHALL` to other clients. | None planned |
| Inside `MULTI`, whole-server commands and `AUTH`, `HELLO` and `CLIENT` are refused. | None planned |
| The thread count can't change between restarts, and a single-thread AOF can't be loaded by several threads. Starting is refused. | Rewrite every file with a new manifest as the commit point |
| After a power cut with `everysec` or `no`, the threads' files can stop at different points, so a later write on one thread can survive an earlier write on another. A single file is always a prefix of history. | None planned: Redis Cluster has the same limit across its nodes |

**Future scope:** sharding across nodes (consistent hashing), Pub/Sub (`SUBSCRIBE`/`PUBLISH`), TLS.

<br>

<p align="center">
  <sub><a href="COMMANDS.md">← Commands</a> &nbsp;&nbsp;|&nbsp;&nbsp; <a href="TESTING.md">Testing →</a></sub>
</p>
