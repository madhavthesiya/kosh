# Changelog

What changed in each release of Kosh. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and from 3.0.0 on,
versions follow [Semantic Versioning](https://semver.org/): a change that can
break a working setup raises the major number.

Earlier milestones were tagged by name (`v1-correct`, `v2-fast`,
`v2.5-memory`), not as semantic versions; they are listed at the end.

## [Unreleased]

### Changed

- The source files moved from the repository's root to `src/`. The code
  itself didn't change.

## [4.0.0] - 2026-11-09

A new major version because the server's design changed: it can now run on
several threads, each owning its keys. Nothing breaks a working setup: with one
thread, the default, the server behaves as before.

### Added

- `kosh-loadgen` (Linux): an open-loop load generator for Redis-compatible
  servers. Latency counts from each request's intended send time, and a
  percentile the run can't support prints as N/A.
- `--threads N` (Linux, `epoll`): shard-per-core. Each thread owns its keys,
  its share of `--maxkeys` and its own AOF file; commands for another thread
  travel through lock-free rings. With one thread, the default, nothing changes.
- A `{tag}` in a key routes it by the tag, as in Redis Cluster. `MSET` and
  `MULTI` with keys on several threads are refused with `-CROSSSLOT`; `MGET`,
  `DEL` and `EXISTS` are split across threads.
- `kosh-loadgen --ports` drives several separate servers.
- `INFO` reports `peak_pending_replies`, the most replies one client has had in
  flight between threads.

### Changed

- The README is now a short front page, and the detail moved to `docs/`.

## [3.1.0] - 2026-11-08

### Added

- `PTTL`, `PEXPIRE`, `PERSIST` and `DBSIZE`, with Redis 7.0.15's replies.
  `PERSIST` is written to the AOF as `PERSIST`, as Redis writes it.
  `EXPIRE`/`PEXPIRE` options (`NX`, `XX`, `GT`, `LT`) are not supported.
- `SCAN cursor [MATCH pattern] [COUNT n] [TYPE type]`, as Redis's: a key
  present for the whole iteration comes back at least once, however the
  table grows between calls. `MATCH` takes Redis's glob patterns, matched
  as Redis matches them, with the same 1,000-level depth limit as the Redis
  7.0.15 Ubuntu ships.
- `FLUSHALL` and `FLUSHDB` (`[ASYNC|SYNC]`; one database, so the same; served
  synchronously), written to the AOF.
- CI runs tair-opensource's resp-compatibility suite against Kosh, as a report.
- GitHub Actions pinned by commit and Docker base images by digest, kept
  current by Dependabot.

### Changed

- `EXPIRE`, `PEXPIRE` and `PEXPIREAT` with a time already past delete the key
  at once, as Redis does, logged as `DEL` and not counted in `INFO`'s
  `expired_keys` (Redis doesn't count them either). Before, the key stayed
  until something touched it.

## [3.0.0] - 2026-11-08

The first release under the name Kosh, and the first meant for others to run.

### Breaking

- The project is renamed from Mini-Redis to Kosh. The binaries are now
  `kosh-server` (was `mini_redis_server`) and `kosh-cli` (was `mini_redis`).
  The repository moved to `github.com/madhavthesiya/kosh`; GitHub redirects
  the old address.
- The server listens on 127.0.0.1 by default (it listened on every interface).
  `--bind 0.0.0.0` listens everywhere; with no password, protected mode then
  refuses clients on other machines, as Redis's does.
- Integer arguments are read as Redis reads them: `EXPIRE k 010`, `-0` and
  `LRANGE l 01 1` are now errors.
- `CLIENT` with an unknown subcommand is an error (it replied `OK`).

### Added

- RESP3 and `HELLO`, so client libraries that open with `HELLO 3` connect
  (redis-py 8, node-redis 6); `CLIENT ID`, `GETNAME`, `SETNAME`, `HELP`.
- Transactions: `MULTI`, `EXEC`, `DISCARD`, written to the AOF all or nothing.
  `WATCH` is not supported.
- `INCR`, `DECR`, `INCRBY`, `DECRBY`, `MGET`, `MSET`; `DEL` and `EXISTS` take
  several keys.
- `INFO` with Redis's sections and field names: hits, misses, expired and
  evicted keys, commands, memory, clients, keyspace.
- Passwords: `--requirepass-file` (or `--requirepass`) and `AUTH`, compared in
  constant time, with Redis's limits on what an unauthenticated client may send.
- `--bind` and `--protected-mode`.
- `--version`; the start-up log names the version.
- A Dockerfile: multi-stage, runs as a non-root user, 37.5 MB.
- `SECURITY.md`: how to report a vulnerability, and the threat model.
- Released under the MIT License.

### Changed

- Keys are hashed with SipHash-1-3 under a random key per process, so a client
  can't choose keys that all collide (hash flooding).
- Replies match Redis's byte for byte: an unknown command's error, `PING` with
  a message. 178 of 179 test commands give the same bytes as Redis 7.0.15
  (the 179th is `WATCH`).

### Fixed

- The log is written a line at a time when stdout is a file or a pipe
  (systemd, `docker logs`); a line could appear only after dozens more.
- On Windows, another socket could bind the port Kosh listens on.

## Before semantic versions

- **`v2.5-memory`** (2026-11-04): SIEVE eviction, the default (exact LRU with
  `--eviction lru`); `KeyTable`, a hash table with cache-line buckets, one
  allocation per key and no stall when it grows.
- **`v2-fast`** (2026-11-03): epoll on Linux, `--maxclients`, `--poller`;
  `--appendfsync always|everysec|no`; group commit; no heap allocation per
  request once warmed up.
- **`v1-correct`** (2026-11-03): network and persistence correctness: non-blocking
  sockets with reply buffers, parser limits, clean shutdown on SIGTERM, AOF
  rewrite through a temporary file, damage detection at start-up, retried AOF
  writes, absolute expiry times in the AOF.
- **`v0-baseline`** (2026-10-31): the project before any of this work.

[Unreleased]: https://github.com/madhavthesiya/kosh/compare/v4.0.0...HEAD
[4.0.0]: https://github.com/madhavthesiya/kosh/compare/v3.1.0...v4.0.0
[3.1.0]: https://github.com/madhavthesiya/kosh/compare/v3.0.0...v3.1.0
[3.0.0]: https://github.com/madhavthesiya/kosh/compare/v2.5-memory...v3.0.0
