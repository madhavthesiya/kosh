# Mini-Redis — Redis-Compatible In-Memory Key-Value Server in C++17

A Redis-compatible server built from scratch in **C++17**: a **TCP server speaking RESP** (works with `redis-cli` and `redis-benchmark`), a **single-threaded event loop with non-blocking sockets and per-client reply buffers**, **append-only-file (AOF) persistence**, **4 data types**, **O(1) LRU eviction**, **lazy TTL expiration**, and a **reader–writer-locked storage engine**.

This is a learning project, not production software. What it does not handle yet is listed under [Known Limitations](#known-limitations).

---

## Performance

Mini-Redis against real Redis 7.0.15 on the same machine, in the same session, with the same durability: both write their AOF to the OS without `fsync`. 100,000 random keys, 50 clients, middle of 3 runs. Intel Core Ultra 5 125H laptop, Ubuntu 24.04 under WSL2, GCC 13 `-O2`. Full results, commands and caveats: [after the correctness fixes](bench/results/after-correctness-fixes/README.md). Earlier stages: [after the network fixes](bench/results/after-network-fixes/README.md), and the starting point, [v0-baseline](bench/results/v0-baseline/README.md).

| Test | Mini-Redis | Redis | Mini-Redis / Redis | After network fixes | At v0-baseline |
|---|---:|---:|---:|---:|---:|
| SET, no pipelining, AOF on | 28,395 req/s | 30,734 req/s | 0.92× | 0.97× | 0.97× |
| GET, no pipelining, AOF on | 31,097 req/s | 30,500 req/s | 1.02× | 1.08× | 0.97× |
| SET, pipeline 16, AOF on | 191,666 req/s | 228,990 req/s | 0.84× | 0.81× | 0.43× |
| GET, pipeline 16, AOF on | 277,577 req/s | 293,686 req/s | 0.95× | 0.96× | 0.56× |
| SET, pipeline 16, AOF off | 276,855 req/s | 277,393 req/s | 1.00× | 1.00× | 0.50× |
| Startup, replaying a 1,000,000-command AOF (45 MB) | 0.72 s | 0.65 s | 1.12× | not re-measured | over 750× |

What the measurements show:
- **AOF replay is now linear, and close to Redis.** It used to erase each command from the front of a buffer holding the rest of the file, moving the remaining bytes once per command. 400,000 commands took 2 min 54 s and 1,000,000 over 15 minutes; they now take 0.30 s and 0.72 s.
- **The correctness fixes cost no measurable throughput.** Against Redis, the ratios are the same as after the network fixes (0.84× vs 0.81× for pipelined `SET` with AOF on, 0.95× vs 0.96× for `GET`).
- **The event-loop rewrite made pipelined throughput 1.45–1.71× faster** (before and after measured in one session). Nearly all of it came from sending the replies to a whole batch of commands with one `send()`, where there used to be one call per command.
- **Pipelined, Mini-Redis reaches about the same throughput as Redis, but uses more CPU for it:** 1.03–1.14× Redis's CPU per request, and 1.30× for `SET` with AOF on.
- **The largest remaining costs were visible in the last profile (after the network fixes).**
  - AOF logging took 43% of pipelined `SET`, because it makes one `write()` per command where Redis writes once per event-loop pass.
  - Hash lookups took 15–27%, because each command looks its key up two or three times.
- **Without pipelining, the tie says nothing about the code.** Neither server reaches 100% CPU: the cost of each network round trip sets the pace for both.
- **316 bytes of heap per key** for a 16-byte key and a 3-byte value.

Raw numbers vary between sessions on this machine (WSL2 on a hybrid CPU) by more than most code changes, so comparisons are made within one session and reported with their spread.

> **Why does pipelining help so much?** Without pipelining, every command waits a full network round trip. With P=16 the client sends 16 commands at once, the server reads them with a single `recv()` and sends all 16 replies with a single `send()`, and the round trip is shared by 16 commands.

Earlier numbers from Windows (one key for every request, unoptimised build) are kept in [stress-test-results/](stress-test-results/); they are not comparable.

---

## Architecture

```mermaid
flowchart TD
    Client1["redis-cli / Client 1"]
    Client2["redis-cli / Client 2"]
    Client3["redis-cli / Client N"]

    Server["TCP Server\n<i>select() event loop</i>\nI/O multiplexing · RESP protocol"]
    Storage["InMemoryStorage\n<i>std::variant · shared_mutex</i>\nLRU · TTL · Type dispatch"]
    AOF["AOFLogger\n<i>Binary RESP format</i>\nAppend-only file"]

    Client1 --> Server
    Client2 --> Server
    Client3 --> Server
    Server --> Storage
    Storage --> AOF

    style Client1 fill:#3b82f6,color:#fff,stroke:none
    style Client2 fill:#3b82f6,color:#fff,stroke:none
    style Client3 fill:#3b82f6,color:#fff,stroke:none
    style Server fill:#6366f1,color:#fff,stroke:none
    style Storage fill:#8b5cf6,color:#fff,stroke:none
    style AOF fill:#a855f7,color:#fff,stroke:none
```

**Single-threaded event loop:** one thread waits on all sockets with `select()` and runs each command to completion, so commands never race each other. Sockets are non-blocking, so the thread never waits on any one client. Redis's main thread uses the same model, but with `epoll`/`kqueue` instead of `select()`.

### Layered Design

| Layer | File(s) | Responsibility |
|-------|---------|----------------|
| **Network** | `Server.h/.cpp` | Accepting clients, the event loop, per-client buffers |
| **Readiness** | `Poller.h/.cpp` | Which sockets are ready (`select()` today, behind an interface `epoll` can implement) |
| **Portability** | `Socket.h` | Winsock vs BSD sockets: types, non-blocking mode, error codes |
| **Protocol** | `RespParser.h` | RESP parsing (binary-safe, with limits) + inline command support |
| **Commands** | `Commands.h/.cpp` | One table of commands (name, number of arguments, write flag, handler), used by both the server and AOF replay |
| **Controller** | `RedisLite.h/.cpp` | Input validation, command routing for the interactive CLI |
| **Engine** | `InMemoryStorage.h/.cpp` | LRU, TTL with an expiry index, type dispatch, AOF integration |
| **Persistence** | `AOFLogger.h/.cpp` | Append-only file: appends, crash-safe rewrite, failed writes |
| **Entry points** | `server_main.cpp`, `main.cpp` | Server (command-line flags) and interactive CLI |

---

## Supported Data Types

| Type | Commands | Internal Structure |
|------|----------|-------------------|
| **String** | `SET`, `GET`, `SETEX` | `std::string` |
| **List** | `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LRANGE`, `LLEN` | `std::deque<string>` |
| **Set** | `SADD`, `SREM`, `SMEMBERS`, `SISMEMBER`, `SCARD` | `std::unordered_set<string>` |
| **Hash** | `HSET`, `HGET`, `HDEL`, `HGETALL`, `HLEN` | `std::unordered_map<string,string>` |

All types are stored in a single `std::variant`:
- **Benefits:** no virtual calls, no separate heap object per value, and type checks at compile time.
- **Cost:** size. Every value is as large as the largest alternative plus a type tag.

Measured on x86-64 Linux with GCC 13 (libstdc++):

| Type | `sizeof` (bytes) |
|------|:----------------:|
| `std::string` | 32 |
| `std::deque<std::string>` (largest alternative) | 80 |
| `std::unordered_set` / `std::unordered_map` | 56 |
| `ValueType` (the variant) | 88 |
| `CacheEntry` (variant + TTL + LRU iterator) | 112 |

So a 3-byte string value still occupies 88 bytes inside the variant. Other compilers and standard libraries give different sizes.

---

## Core Features

### 1. TCP Server with RESP Protocol

Works with `redis-cli` and `redis-benchmark`: it answers the `COMMAND`, `CLIENT` and `CONFIG GET` calls they make at startup. It supports:
- **RESP arrays** (`*3\r\n$3\r\nSET\r\n...`) — binary-safe, length-prefixed
- **Inline commands** (`PING\r\n`) — for telnet/debugging
- **Per-client receive buffers** — handles TCP stream reassembly correctly
- **Pipelining** — runs every complete command found in each read, and sends all their replies with one `send()`
- **Protocol errors** — malformed input gets `-ERR Protocol error: …` and the connection is closed, as in Redis. Limits from Redis's defaults: bulk strings up to 512 MB, inline commands up to 64 KB, 1 GB of unfinished input per client
- **`TCP_NODELAY`** — disables Nagle's algorithm, so small replies aren't held back to be batched

```bash
# Connect with redis-cli
redis-cli -p 6379
127.0.0.1:6379> SET name "hello world"
OK
127.0.0.1:6379> GET name
"hello world"
```

### 2. Event Loop with Non-Blocking I/O

```
Before: accept() → handle client → BLOCKS → accept() next
After:  wait until some sockets are ready → handle those, never waiting on any one client
```

- **One thread** serves every client, with no thread per connection.
- **Non-blocking sockets:** `recv()`, `send()` and `accept()` return at once. Each client has an input buffer (bytes until they form whole commands) and an output buffer (replies until its socket takes them). A partial `send()` is normal; the rest goes when the socket is writable again.
- **Backpressure:** a client's next commands run only once its earlier replies are sent, and a batch stops at 1 MB of waiting replies. Meanwhile the server stops reading from that client, so TCP flow control slows it down. A client that never reads its replies can't stall anyone else or make the server's memory grow without limit.
- **`Poller` interface:** sockets are registered once, with what to watch for: always readability, and writability only while replies are waiting. `SelectPoller` implements it with `select()`; an `epoll` version is planned.
- **Cost of `select()`:** each call scans every socket (O(N)), and on Linux it can only watch sockets numbered below 1024. A client beyond that gets `-ERR max number of clients reached` and is disconnected. The server raises its open-file limit at startup so it can reply at all, and keeps a spare file descriptor to refuse clients properly if it runs out (`EMFILE`).
- **Clean shutdown:** Ctrl+C or `SIGTERM` sets an atomic flag, the loop stops, connections and the AOF are closed, and the process exits with code 0.
- **SIGPIPE ignored (Linux/macOS):** a client that disconnects early can't kill the server. Writes to it fail with `EPIPE` instead (see Challenge 7).

### 3. AOF Persistence

Every write command is appended to `appendonly.aof` in **binary RESP format**:

```
Plain text:  "SET greeting hello world"     → BREAKS on replay (spaces)
RESP format: "*3\r\n$3\r\nSET\r\n$8\r\ngreeting\r\n$11\r\nhello world\r\n"
             ↑ length-prefixed → binary-safe ✓
```

| Feature | Details |
|---------|---------|
| Durability | Each write command's record is handed to the OS with `write()` before its reply is sent. It survives a crash of the process, but without `fsync` a power cut or OS crash can lose recent writes. |
| Binary safety | RESP format — spaces, newlines, any byte in values |
| Expiry times | Logged as absolute Unix times in milliseconds (`SET … PXAT`, `PEXPIREAT`), as Redis does, so a key that expired while the server was down stays expired. AOFs with the older `SETEX`/`EXPIRE` records still load. |
| Removals | Every key the server removes on its own, by LRU eviction or because it expired, is logged as a `DEL`. Replay then rebuilds exactly the keys, values and TTLs the server held (see Challenge 11). The LRU order isn't rebuilt: reads aren't logged. |
| Replay on startup | Each record runs through the same command table as client commands. While loading, nothing expires, nothing is evicted and nothing is logged; afterwards, keys beyond `--maxkeys` are evicted, and those evictions are logged. Takes time linear in the file size. |
| Damaged file | An incomplete last record, left by a crash mid-write, is cut off with a warning, as Redis's `aof-load-truncated` does. Damage before the end stops startup with the byte offset of the damage and the `truncate` command that keeps everything before it. |
| Failed writes | If a write fails (a full disk), the bytes not written wait in memory, in order, and are written once the file takes them. Meanwhile, write commands get `-MISCONF Errors writing to the AOF file: …` and reads keep working, as in Redis. |
| Compaction | `BGREWRITEAOF` writes the data to a temporary file, `fsync`s it, renames it over the AOF and `fsync`s the directory, so a crash at any point leaves a complete AOF. Despite the name, it runs in the foreground; see [Known Limitations](#known-limitations). |
| Configuration | `--appendonly no` disables it; `--appendfilename` chooses the file |

### 4. O(1) LRU Eviction

```mermaid
graph LR
    subgraph HashMap["std::unordered_map"]
        A["key → CacheEntry"]
    end

    subgraph DLL["std::list (Doubly Linked List)"]
        MRU["Most Recent"] --> N2["..."] --> LRU["Least Recent"]
    end

    subgraph Entry["CacheEntry"]
        V["std::variant value"]
        E["has_expiry + expiry_time"]
        I["lru_iterator ──────────┐"]
    end

    A --> Entry
    I -.->|points to| DLL

    style HashMap fill:#1e293b,color:#e2e8f0,stroke:#334155
    style DLL fill:#1e293b,color:#e2e8f0,stroke:#334155
    style Entry fill:#1e293b,color:#e2e8f0,stroke:#334155
```

- HashMap for O(1) lookup + doubly-linked list for O(1) reordering
- Iterator stored in each entry — no linear scan to find position
- Capacity is set with `--maxkeys` (default 10,000)
- **Expired keys go before live ones.** Keys with a TTL are also kept in an index ordered by expiry time (a `std::set` of expiry time and a pointer to the key in the hash map). When a new key needs room, expired keys are removed from the front of the index first, and a live key is evicted only when none is left. Only as many are removed as are needed, so one insert never sweeps a large backlog.
- The pointers stay valid because `std::unordered_map` keeps references to its elements valid until they are erased, even when it rehashes.

### 5. Thread Safety

`std::shared_mutex` (reader-writer lock) — concurrent reads, exclusive writes.

| Method | Lock Type | Why |
|--------|-----------|-----|
| `GET`, `EXISTS` | `unique_lock` | Lazy expiration may trigger write |
| `SET`, `DEL`, `EXPIRE` | `unique_lock` | Modifies data |
| `TTL`, `TYPE`, `LRANGE` | `shared_lock` | Pure read |
| `SAVE` (CLI only) | `shared_lock` | Snapshot is read-only |

The single-threaded server doesn't need this lock. It exists so the storage engine is safe to use from several threads, which the stress tests do.

---

## Challenges Faced & Solutions

### Challenge 1: TCP Stream Reassembly

**Problem:** TCP doesn't preserve message boundaries. One `recv()` call might return half a command, or three commands concatenated together.

**Wrong approach:** Assume each `recv()` = one complete command. This works in testing but breaks under load.

**Solution:** Per-client receive buffer. Data is appended on each `recv()`. The parser extracts complete commands and leaves partial data in the buffer for the next `recv()`.

```cpp
// Process ALL complete commands, leave incomplete data for next recv()
size_t pos = 0;
while(true) {
    auto parsed = RespParser::parse(std::string_view(client.in).substr(pos));
    if(parsed.status == ParseStatus::NeedMore) break;   // incomplete: wait for more data
    if(parsed.status == ParseStatus::Error) { /* reply, then disconnect */ return; }
    pos += parsed.consumed;
    client.out += executeCommand(ctx, parsed.args);    // via the command table
}
client.in.erase(0, pos);    // once per batch, not once per command
```

A unit test splits a stream of commands at every possible point, into two or three reads and one byte at a time, and checks the parser always produces the same commands.

### Challenge 2: Binary Safety in Persistence

**Problem:** Values with spaces (`"hello world"`) break plain-text AOF parsing. `istringstream >> val` stops at the first space.

```
AOF line:   SET greeting hello world
Replay:     key="greeting", value="hello"  ← "world" is LOST
```

**Solution:** Switched AOF format from plain text to RESP (length-prefixed binary). `$11\r\nhello world\r\n` tells the parser "read exactly 11 bytes" — spaces are preserved.

### Challenge 3: Iterator Invalidation in the Event Loop

**Problem:** When processing client data, a client might disconnect (QUIT or error). `removeClient()` erases it from the client map. If we're iterating the map, this is undefined behavior.

**Solution:** The poller returns the ready sockets as a separate list, and the loop goes through that list instead of the map. A client removed earlier in the same pass is skipped:

```cpp
for(const auto& event : ready) {            // ready: filled by poller_->wait()
    if(event.readable && clients_.count(event.fd)) handleReadable(event.fd);
    if(event.writable && clients_.count(event.fd)) serveClient(event.fd);
}
```

An earlier version copied every client into a vector on each pass for the same reason.

### Challenge 4: Expired Keys Must Behave as Absent

**Problem:** Expiry is lazy: an expired key stays in memory until something touches it. `DEL` didn't check expiry, so `DEL` on a key whose TTL had passed returned 1 ("deleted") and logged a `DEL` to the AOF. Redis returns 0, because logically the key no longer exists.

**Solution:** `DEL` now checks `isExpired()` first, like `GET` and `EXISTS`. An expired key is removed and `DEL` returns 0. The removal is logged as an expiration, a `DEL` record, like every key the server removes on its own (Challenge 11).

### Challenge 5: BGREWRITEAOF Was Silently Broken

**Problem:** The server's `BGREWRITEAOF` command called `dumpState()` but threw away the result. Compaction appeared to work but did nothing.

**Root cause:** Server didn't have access to AOFLogger — only InMemoryStorage did.

**Solution:** Pass `AOFLogger*` to Server constructor. BGREWRITEAOF now calls `aof_->rewrite(commands)`.

**A second problem:** the rewrite truncated the live AOF before writing the new contents, so a crash or a full disk in between lost the file. Challenge 12 covers the fix.

### Challenge 6: Cross-Platform Socket Portability

**Problem:** Socket APIs differ between Windows (Winsock) and Linux (POSIX). Even `close()` vs `closesocket()`, `SOCKET` vs `int`.

**Solution:** Platform abstraction via preprocessor macros:

```cpp
#ifdef _WIN32
    using socket_t = SOCKET;
    #define CLOSE_SOCKET closesocket
#else
    using socket_t = int;
    #define CLOSE_SOCKET close
#endif
```

### Challenge 7: One Disconnecting Client Could Kill the Server

**Problem:** On Linux, calling `send()` on a socket whose client has already closed raises `SIGPIPE`, and the default action of `SIGPIPE` terminates the process. A client that sent a batch of commands and disconnected without reading the replies took the whole server down, for every client.

**How it was found:** an integration test sends 10,000 pipelined `PING`s, closes the socket without reading, then checks that the server is still alive. It failed with `server died (killed by SIGPIPE)`.

**Solution:** The server ignores `SIGPIPE` at startup (`std::signal(SIGPIPE, SIG_IGN)`), as Redis does. `send()` then fails with `EPIPE`, and the server removes that client. Windows has no `SIGPIPE`.

### Challenge 8: One Client That Didn't Read Stalled Everyone

**Problem:** Sockets were blocking, and each reply went straight to `send()`. A client that pipelined commands without reading the replies filled its socket buffer; the next `send()` never returned, and the single-threaded server stopped serving every other client.

**How it was found:** an integration test sends 50,000 `GET`s of a 10 KB value from one client that never reads, then times a `PING` from a second client. It timed out.

**Solution:** Non-blocking sockets and a reply buffer per client, sent as far as the socket takes it. The subtle part is pausing and resuming. While a client's replies are waiting, the server neither runs its next commands nor reads from it, which bounds memory and lets TCP slow that client down. When the replies have gone, commands it already sent must run straight away rather than waiting for new data; the client may have sent everything and be waiting for those replies, so waiting for input would deadlock. The second client's `PING` now answers in 0.3–1.3 ms, and a test checks that 5,000 pipelined 10 KB replies all arrive, in order, through the pause and resume.

### Challenge 9: Garbage Looked Like an Incomplete Command

**Problem:** The parser returned "0 bytes consumed" both for "wait for more data" and for "this can never be valid". Malformed input such as `*abc` or `$-5` therefore made the server wait forever, and `$2000000000` made it wait for 2 GB. `std::stoi` also read `"3abc"` as 3.

**Solution:** Three results: `Ok`, `NeedMore`, `Error`. Numbers are parsed with `std::from_chars`, which must use the whole field. Redis's size limits apply. On `Error` the client gets `-ERR Protocol error: …` and is disconnected, as in Redis.

**Related:** error replies quote client input, such as an unknown command's name. A name containing `\r\n` ended the reply early, and the rest was read as the reply to the next command, so the connection fell out of step for good. As in Redis, line breaks in error messages now become spaces, and at most 128 bytes of a name are quoted.

### Challenge 10: The 1,021st Client Crashed the Server

**Problem:** On Linux, `select()`'s `fd_set` is a bit array for sockets 0–1023. The 1,021st client got socket 1024, and the server read and wrote past the end of `fd_set`. Measured at [v0-baseline](bench/results/v0-baseline/README.md#5-the-select-limit-bug-8):
- the optimised build was killed by glibc's `_FORTIFY_SOURCE` check;
- the sanitizer build stopped at the out-of-bounds `FD_ISSET`;
- a build with neither kept running while corrupting its stack.

**Solution:** The poller says whether it can watch a socket. One it can't gets `-ERR max number of clients reached` and is closed, as Redis does at `maxclients`.

Testing exposed two more problems:
- **Socket 1024 could not even be created.** At the common open-file limit of 1024, `accept()` failed with `EMFILE` before the check was reached. The server now raises its limit at startup, as Redis does.
- **Running out of descriptors made the server spin.** When the limit can't be raised, the connection stays queued and `select()` keeps reporting it. The server spun at 100% CPU, logging `accept() failed (error 24)` about 6,000 times a second. It now keeps one spare descriptor, as nginx and libev do: on `EMFILE` it frees it, accepts the connection, refuses it properly, then takes the spare back.

**How it was found:** a test opens 1,100 connections against a server started with a 1024-file limit. Another limits the server to 64 files and checks that it stays idle (under 0.2 s of CPU in a second) while refusing the excess.

### Challenge 11: A Restart Didn't Bring Back the Same Data

**Problem:** The AOF is only correct if replaying it rebuilds exactly the data the server held. Two things broke that:
- **TTLs were logged as relative seconds** (`SETEX key 10 …`). Replay gave every key its full TTL again, counted from the restart, so a key that expired while the server was down came back.
- **Keys the server removed on its own weren't logged.** Replay re-ran the `SET`s and evicted keys by its own LRU order. But `GET`s change that order and aren't in the AOF, so replay evicted different keys. After a restart, a key the server had evicted could be back, and a key it still held could be gone.

**Solution**, as in Redis:
- **Expiry times are logged as absolute Unix times in milliseconds** (`SET k v PXAT …`, `PEXPIREAT k …`).
  - In memory they stay on the monotonic clock (`std::chrono::steady_clock`), so changing the system clock doesn't move them.
  - Only the file uses wall-clock time, because the monotonic clock starts again from an arbitrary point when the machine reboots.
  - Clients choose these numbers, so the conversions saturate instead of overflowing; signed overflow is undefined behaviour.
- **Every eviction and expiry is logged as a `DEL`.** While loading, nothing expires, nothing is evicted and nothing is logged, so replay does exactly what the file says.

**How it's tested:**
- **Expiry:** keys that expired before a restart must stay expired after it, and a rewrite must keep a TTL with under a second left. Both tests failed before; the second found a rewrite bug that dropped such TTLs.
- **Removals:** a randomized test, which failed on its first seed before the fix.
  - It runs 2,500 random commands through the command table: 8 keys in room for 5, short and long TTLs, reads, and the odd rewrite.
  - It then replays the AOF into a fresh instance and compares the two key by key: type, contents and expiry.
  - It now passes 300 seeds, also under AddressSanitizer and UndefinedBehaviorSanitizer.

### Challenge 12: A Crash or a Full Disk Could Lose the AOF

Three problems, each reproduced by a test before it was fixed. The tests make writes fail with a file size limit (`RLIMIT_FSIZE`), which fails them with `EFBIG` the way a full disk does.

**A failed rewrite destroyed the AOF.** `BGREWRITEAOF` truncated the live file, then wrote the new contents into it. Under a 4 KB limit, a 26,490-byte AOF was left at 4,096 bytes, and the client was told `OK`. The rewrite now:
1. writes a temporary file;
2. `fsync`s it, so the rename can't reach the disk before the data does;
3. renames it over the AOF, which replaces the file atomically;
4. `fsync`s the directory, which makes the rename itself durable.

A crash at any point leaves either the old file or the new one, whole. `strace` of a live rewrite shows the calls in this order.

**A crash mid-write lost every later write.** The incomplete record stayed at the end of the file, new records were appended after it, and at the next start they were read as its missing part. Now, as with Redis's `aof-load-truncated`:
- an incomplete last record is cut off at startup, with a warning;
- damage before the end stops startup and gives its byte offset.

**A full disk lost writes silently.** Failed writes were ignored: clients got `OK`, and every record from then on was missing. Past a file size limit it was worse: `SIGXFSZ` killed the server.
- Now the bytes not written wait in memory, in order, and write commands get `-MISCONF` until the file takes them. Each refused write first retries, so writes resume as soon as there is room. Reads keep working.
- The server ignores `SIGXFSZ`, so it gets the error instead of being killed.
- The integration test raises the server's file size limit while it runs (`prlimit`). It then checks that writes resume, and that after a restart every acknowledged key is there.

---

## Full Command Reference

### String Commands
| Command | Syntax | Description |
|---------|--------|-------------|
| `SET` | `SET key value` | Set key-value pair |
| `SET` | `SET key value EX seconds` | Set with a TTL. Also `PX milliseconds`, `EXAT unix-seconds` and `PXAT unix-milliseconds`. The number must be a positive integer, with Redis's error replies. `NX`, `XX`, `GET` and `KEEPTTL` are not supported and return a syntax error. |
| `SETEX` | `SETEX key seconds value` | Set with TTL (alternative) |
| `GET` | `GET key` | Get value |

### Key Commands
| Command | Syntax | Description |
|---------|--------|-------------|
| `DEL` | `DEL key` | Delete key |
| `EXISTS` | `EXISTS key` | Check existence |
| `TYPE` | `TYPE key` | Get data type |
| `EXPIRE` | `EXPIRE key seconds` | Set TTL; 0 or less makes the key expire at once |
| `PEXPIREAT` | `PEXPIREAT key unix-milliseconds` | Set the expiry as an absolute time (the form the AOF records) |
| `TTL` | `TTL key` | Remaining TTL in seconds, rounded to the nearest second; -1 without a TTL, -2 if the key doesn't exist |

### List Commands
| Command | Syntax | Description |
|---------|--------|-------------|
| `LPUSH` | `LPUSH key val1 val2 ...` | Push to head |
| `RPUSH` | `RPUSH key val1 val2 ...` | Push to tail |
| `LPOP` | `LPOP key` | Pop from head |
| `RPOP` | `RPOP key` | Pop from tail |
| `LRANGE` | `LRANGE key start stop` | Get range (supports negative indices) |
| `LLEN` | `LLEN key` | Get list length |

### Set Commands
| Command | Syntax | Description |
|---------|--------|-------------|
| `SADD` | `SADD key m1 m2 ...` | Add members |
| `SREM` | `SREM key m1 m2 ...` | Remove members |
| `SMEMBERS` | `SMEMBERS key` | Get all members |
| `SISMEMBER` | `SISMEMBER key member` | Check membership |
| `SCARD` | `SCARD key` | Get set size |

### Hash Commands
| Command | Syntax | Description |
|---------|--------|-------------|
| `HSET` | `HSET key f1 v1 f2 v2 ...` | Set fields |
| `HGET` | `HGET key field` | Get field value |
| `HDEL` | `HDEL key f1 f2 ...` | Delete fields |
| `HGETALL` | `HGETALL key` | Get all field-value pairs |
| `HLEN` | `HLEN key` | Get number of fields |

### Server Commands
| Command | Description |
|---------|-------------|
| `PING` | Returns PONG |
| `INFO` | Server info (version, connected clients) |
| `BGREWRITEAOF` | Compacts the AOF through a temporary file and an atomic rename. Runs in the foreground, blocking other clients. Returns an error when AOF is disabled, or when the rewrite fails; the AOF is then unchanged. |
| `QUIT` | Replies `OK`, then closes the connection once the reply is sent. Commands after it in the same pipeline don't run. |

There is no `SAVE`: the AOF is the server's only persistence. A wrong number of arguments gets Redis's `-ERR wrong number of arguments for '…' command`. While AOF writes are failing, write commands get `-MISCONF` (see AOF Persistence).

---

## Complexity

| Operation | Time | Space |
|-----------|:----:|:-----:|
| SET / GET / DEL | O(1) average, plus O(log T) when a TTL is set or removed | O(1) |
| Making room for a new key | O(log T) per expired key removed, then O(1) to evict | O(1) |
| LPUSH / RPUSH | O(1) per element | O(1) per element |
| LPOP / RPOP | O(1) | O(1) |
| LRANGE | O(K) | O(K) |
| SADD / SREM / SISMEMBER | O(1) average per member | O(1) |
| HSET / HGET / HDEL | O(1) average per field | O(1) |
| BGREWRITEAOF | O(N) | O(N) |
| AOF replay at startup | O(F) | O(F) |
| SAVE / LOAD (CLI only) | O(N) | O(N) |

N = number of stored keys (bounded by `--maxkeys`), T = keys with a TTL, K = elements returned, F = size of the AOF: replay reads the whole file into memory first. Overall space: **O(N)**.

---

## Tests

**85 unit tests** (`tests/test_main.cpp`):

```
========== Mini-Redis Test Suite ==========

[Basic Operations]      7 tests    ✅
[LRU Eviction]          5 tests    ✅
[TTL Expiration]        5 tests    ✅
[TTL > LRU Priority]    2 tests    ✅
[Persistence]           4 tests    ✅  (the CLI's SAVE/LOAD)
[Controller Validation] 1 test     ✅
[List Operations]       6 tests    ✅
[Set Operations]        5 tests    ✅
[Hash Operations]       5 tests    ✅
[Type Safety]           4 tests    ✅
[LRU with Mixed Types]  1 test     ✅
[AOF Persistence]      20 tests    ✅  (absolute expiry, logged removals, randomized replay check, damaged files, failed writes)
[Command Table]        10 tests    ✅  (arity, SET options, strict integers, error replies)
[RESP Parser]          10 tests    ✅  (malformed input, size limits, every split point)

Results: 85 passed, 0 failed
```

The randomized replay test runs 4 seeds by default; `RANDOM_TEST_SEEDS=300 ./build/tests` runs more.

**Thread-safety stress tests** (`tests/test_thread_safety.cpp`):
- 5 tests, each running 8 threads at once against one storage instance (20,000 operations in total).
- They check for crashes and invalid results. Build with `-DSANITIZE=thread` to have ThreadSanitizer check for data races.

**Integration tests** (`tests/integration/test_server.py`, Python standard library only):
- 29 tests that start the real server binary on a free port and speak RESP over TCP.
- They cover:
  - basic commands, command-line flags, and AOF replay after a restart;
  - a damaged AOF at startup, and failed AOF writes under a file size limit (Challenge 12);
  - protocol errors and error replies;
  - a client that never reads its replies (Challenge 8);
  - 1,100 connections, and running out of file descriptors (Challenge 10);
  - shutdown on `SIGINT`/`SIGTERM`;
  - the client-disconnect regression (Challenge 7).

All three suites run under `ctest` and pass with AddressSanitizer + UndefinedBehaviorSanitizer. The thread-safety tests also pass under ThreadSanitizer. The Windows build is compile-checked with MinGW-w64 (GCC 13) but not run.

---

## Project Structure

```
mini-redis/
├── server_main.cpp           # Server entry point (TCP mode, command-line flags)
├── main.cpp                  # CLI entry point (interactive mode)
├── Server.h / .cpp           # TCP server — event loop, per-client buffers, RESP I/O
├── Poller.h / .cpp           # Readiness interface + select() implementation
├── Socket.h                  # Winsock / BSD socket differences (header-only)
├── RespParser.h              # RESP protocol parser + serializer (header-only)
├── Commands.h / .cpp         # Command table — dispatch for the server and AOF replay
├── RedisLite.h / .cpp        # Controller — input validation, command routing (CLI)
├── StorageInterface.h        # Abstract interface (pure virtual base class)
├── InMemoryStorage.h / .cpp  # Storage engine — LRU, TTL, variant types, AOF
├── AOFLogger.h / .cpp        # Append-only file — appends, crash-safe rewrite, failed writes
├── Result.h                  # Result type with error codes
├── CMakeLists.txt            # CMake build (C++17, warnings, optional sanitizers)
├── .gitignore
├── bench/                    # Measurement scripts (throughput, replay, profile, memory)
│   └── results/              # Measured results, one directory per version
├── stress-test-results/      # Earlier Windows benchmark screenshots (superseded)
└── tests/
    ├── test_main.cpp              # 85 unit tests
    ├── test_thread_safety.cpp     # Concurrency stress tests (5 tests x 8 threads)
    └── integration/
        └── test_server.py         # Integration tests against the real server binary
```

---

## Build & Run

**Prerequisites:**
- A C++17 compiler with `<charconv>`, e.g. GCC 8 or newer. Tested with GCC 13 on Ubuntu 24.04.
- CMake 3.16+.
- Python 3, optional, for the integration tests.

### Build and test (CMake)

```bash
cmake -B build                            # defaults to RelWithDebInfo: -O2 with debug symbols
cmake --build build -j
cd build && ctest --output-on-failure     # unit, thread-safety and integration tests
```

### Run the server

```bash
./build/mini_redis_server                 # port 6379, 10,000 keys, AOF in ./appendonly.aof
./build/mini_redis_server --port 6380 --maxkeys 1000000 --appendonly no
./build/mini_redis_server --help

# Connect with redis-cli (in another terminal)
redis-cli -p 6379
```

Ctrl+C (or `SIGTERM`) stops the server cleanly: it closes the connections and the AOF and exits with code 0.

| Option | Default | Meaning |
|--------|---------|---------|
| `--port <1-65535>` | 6379 | TCP port to listen on |
| `--maxkeys <n>` | 10000 | Keys to hold before LRU eviction |
| `--appendonly <yes\|no>` | yes | Log writes to the AOF and replay it at startup |
| `--appendfilename <path>` | appendonly.aof | AOF file; the server refuses to start if it can't be opened |

### Benchmark

The numbers in this README come from the scripts in [`bench/`](bench/). They build an optimised binary, run Mini-Redis and real Redis the same way, repeat each test 3 times, and record the machine, compiler and commit next to the results. [bench/README.md](bench/README.md) explains the method.

```bash
python3 bench/bench.py --out bench/results/my-run      # Linux; about 25 minutes
```

For a quick manual run:

```bash
rm -f appendonly.aof                      # the AOF keeps growing across runs
./build/mini_redis_server --maxkeys 1000000
redis-benchmark -p 6379 -t set,get -n 1000000 -r 100000 -c 50
```

- `-r 100000` spreads requests over 100,000 random keys; without it, every request hits one key.
- `--maxkeys` must be at least that many, or most `SET`s also evict.
- Leave out `-q` to see latency percentiles.

### Sanitizers (GCC/Clang)

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer, all tests
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address,undefined
cmake --build build-asan -j
(cd build-asan && ctest --output-on-failure)

# ThreadSanitizer, concurrency tests
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread
cmake --build build-tsan -j --target test_threads
setarch "$(uname -m)" -R ./build-tsan/test_threads
```

> GCC 13's ThreadSanitizer stops with `unexpected memory mapping` on Linux kernels with high address-space randomisation (e.g. Ubuntu 24.04, WSL2). `setarch -R` disables randomisation for that one process.

### Interactive CLI

```bash
./build/mini_redis
```

### Without CMake

```bash
# Linux / macOS
g++ -std=c++17 -O2 -pthread server_main.cpp Server.cpp Poller.cpp Commands.cpp InMemoryStorage.cpp AOFLogger.cpp RedisLite.cpp -o mini_redis_server
g++ -std=c++17 -O2 -pthread main.cpp Commands.cpp InMemoryStorage.cpp AOFLogger.cpp RedisLite.cpp -o mini_redis
g++ -std=c++17 -O2 -pthread tests/test_main.cpp Commands.cpp InMemoryStorage.cpp AOFLogger.cpp RedisLite.cpp -o run_tests
g++ -std=c++17 -O2 -pthread tests/test_thread_safety.cpp InMemoryStorage.cpp AOFLogger.cpp RedisLite.cpp -o run_thread_tests
```

On Windows (MinGW), add `-lws2_32` to the server command for Winsock.

---

## Design Trade-offs

| Decision | Why |
|----------|-----|
| **`select()` over `epoll`/IOCP** | Portable across Windows, Linux and macOS, and behind a `Poller` interface so `epoll` can be added without touching the event loop. **Cost:** every call scans all sockets (O(N)), and about 1,020 clients at most: on Linux `select()` can't watch socket numbers ≥ 1024, and on Windows the set holds 1024 sockets (this code raises Winsock's default of 64). Clients beyond that are refused. |
| **Pause a slow reader instead of disconnecting it** | When a client's replies pile up, the server stops reading from it until they are sent, so TCP slows that client down. Memory per client stays bounded without a limit at which clients are cut off. **Cost:** a client that never reads keeps its connection, and the replies already produced (a batch stops once 1 MB is waiting, after the reply that crossed it), until it closes. |
| **Single-threaded server** | Same model as Redis's main thread: commands run one at a time, so they can't race each other. **Cost:** one slow operation delays every client. |
| **RESP AOF over plain text** | Binary-safe. Values with spaces, newlines, special chars survive persistence. |
| **`write()` without `fsync()`** | Each record is handed to the OS with `write()`. `fsync()` would wait until it is on disk, which is much slower. **Cost:** a power cut or OS crash can lose writes the OS hasn't written out yet; Linux typically writes them within about 30 seconds. |
| **Monotonic clock in memory, Unix time in the AOF** | `steady_clock` can't jump when the system clock is changed, so a TTL lasts as long as it says. But it starts again from an arbitrary point when the machine reboots, so the file needs wall-clock time. **Cost:** expiry is converted between the two clocks at the AOF, and a change of the system clock moves the expiry of every key loaded at the next start (Redis has the same property). |
| **Refuse writes while the AOF can't be written** | Same as Redis's `MISCONF`. Acknowledging writes that can't be saved would lose them silently at the next restart; exiting would take reads down too. **Cost:** a full disk makes the server read-only until there is room. |
| **Reject unsupported `SET` options** | `NX`, `XX`, `GET` and `KEEPTTL` return a syntax error. Ignoring `NX` would let `SET lock me NX` take a lock that someone else holds. |
| **`TCP_NODELAY` enabled** | Disables Nagle's algorithm. Trades bandwidth efficiency for latency — correct for request-response protocols. |
| **`std::variant` over inheritance** | No vtable pointer, no separate heap object per value, compile-time type checks. **Cost:** every value is as large as the largest alternative (88 bytes on x86-64 GCC 13, even for a 3-byte string). |
| **Lazy TTL over active expiration** | No background thread or timer. O(1) per access. **Cost:** expired keys use memory until they're accessed or a new key needs their room. Redis also samples keys with a TTL about 10 times a second. |
| **`shared_mutex` over lock-free** | Correct and easy to reason about. Lock-free structures can scale better under contention but are much harder to get right. The single-threaded server needs neither. |
| **Assert-based tests over gtest** | Zero dependencies (the integration tests use only Python's standard library). Tests are the spec — readable by anyone. |

---

## Known Limitations

Found while reviewing the code. Each fix starts with a test that reproduces the problem.

**Networking**

| Limitation | Planned fix |
|---|---|
| `select()` serves about 1,020 clients at most (see Design Trade-offs); more are refused, and each call scans every socket. | An `epoll` `Poller` on Linux |
| A command that arrives over many reads is parsed again from its start after each read. Nothing is copied, but a command with very many arguments costs O(n²) to scan. Redis keeps the parser's state between reads instead. | Parser that resumes where it stopped |
| After a protocol error, the server closes the connection at once. If the client is still sending, unread data makes the kernel answer with RST, which can discard the error reply before the client reads it (Redis behaves the same way). | Read and discard the rest before closing (a lingering close) |

**Persistence**

| Limitation | Planned fix |
|---|---|
| No `fsync`: a power cut or OS crash can lose writes the OS hasn't written to disk yet. | Configurable policy: `always` / `everysec` / `no` |
| The write command that first hits a failed AOF write has already changed the data, so it gets its normal reply, but its record only waits in memory. If the server stops before the disk has room, that write is lost. Redis does the same unless `appendfsync always`. | With the `always` policy, stop the server instead, as Redis does |
| `BGREWRITEAOF` runs in the foreground: every client waits while the whole data set is written. | A background rewrite in a `fork()`ed child |
| A record whose length field is damaged into a larger number looks like an incomplete last record. Startup then cuts it off with everything after it, with a warning giving the number of bytes. Nothing in the file tells the two apart; Redis has the same ambiguity. | None yet; a checksum per record would tell them apart |
| Replay reads the whole AOF into memory before running it, so startup briefly needs memory for the file as well as the data. | Read it in fixed-size chunks |
| The interactive CLI (`mini_redis`) still has `SAVE` and `LOAD`, in a text format that holds string keys only and isn't binary-safe. `LOAD` isn't logged to the AOF, so the next start goes back to the data from before it. | Remove them, as in the server |

**Commands and storage**

| Limitation | Planned fix |
|---|---|
| `SET`'s `NX`, `XX`, `GET` and `KEEPTTL` options are not supported (they return a syntax error rather than being ignored). | Implement them, with tests |

**Future scope:** sharding across nodes (consistent hashing), Pub/Sub (`SUBSCRIBE`/`PUBLISH`), `AUTH`.

---

<div align="center">

**Made by [Madhav Thesiya](https://www.linkedin.com/in/madhavthesiya/)** — If this was useful, drop a ⭐<br>
**© 2026 Madhav Thesiya. All Rights Reserved.**

</div>
