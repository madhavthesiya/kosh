# Mini-Redis — Redis-Compatible In-Memory Key-Value Server in C++17

A Redis-compatible server built from scratch in **C++17**: a **TCP server speaking RESP** (works with `redis-cli` and `redis-benchmark`), a **single-threaded epoll event loop with non-blocking sockets and per-client reply buffers**, **append-only-file (AOF) persistence with group commit and a configurable fsync policy**, **4 data types**, **O(1) LRU eviction**, **lazy TTL expiration**, a **reader–writer-locked storage engine**, and **no heap allocation per request** once warmed up.

This is a learning project, not production software. What it does not handle yet is listed under [Known Limitations](#known-limitations).

---

## Performance

Mini-Redis against real Redis 7.0.15 on the same machine, in the same session, with the same durability (both with `appendfsync no`). 100,000 random keys, 50 clients, middle of 3 runs, the server measured first alternating from run to run. Intel Core Ultra 5 125H laptop, Ubuntu 24.04 under WSL2, GCC 13 `-O2`. Full results, commands and caveats: [after the speed fixes](bench/results/after-speed-fixes/README.md). Earlier stages: [after the correctness fixes](bench/results/after-correctness-fixes/README.md), [after the network fixes](bench/results/after-network-fixes/README.md), and the starting point, [v0-baseline](bench/results/v0-baseline/README.md).

| Test | Mini-Redis | Redis | Mini-Redis / Redis | CPU per request vs Redis | At v0-baseline |
|---|---:|---:|---:|---:|---:|
| SET, pipeline 16, AOF on | 232,148 req/s | 192,485 req/s | **1.21×** | **0.73×** | 0.43× |
| GET, pipeline 16, AOF on | 277,316 req/s | 252,653 req/s | 1.10× | 0.90× | 0.56× |
| SET, pipeline 16, AOF off | 262,743 req/s | 262,688 req/s | 1.00× | 0.99× | 0.50× |
| GET, pipeline 16, AOF off | 247,011 req/s | 261,083 req/s | 0.95× | 1.00× | 0.48× |
| SET, no pipelining, AOF on | 22,701 req/s | 21,044 req/s | 1.08× | | 0.97× |
| GET, no pipelining, AOF on | 23,833 req/s | 24,217 req/s | 0.98× | | 0.97× |
| Startup, replaying a 1,000,000-command AOF (45 MB) | 0.68 s | 0.64 s | 1.07× | | over 750× |

What the measurements show:
- **With AOF on, Mini-Redis serves pipelined `SET`s faster than Redis, on less CPU:**
  - 1.21× the throughput, and 0.73–0.82× the CPU per request, in two sessions;
  - faster in every one of 6 runs, including the runs where Redis was measured first.
  - The change behind it is **group commit**: the AOF records of every command in a pass of the event loop now go out in one `write()`, before any of their replies. 100,000 pipelined `SET`s made 132 `write()` calls where they made 100,002, and SET got 1.42–1.71× faster in every round of a commit-by-commit comparison.
- **Everything else ties with Redis:** pipelined `GET`, and both without AOF (0.90–1.02× the CPU per request). At v0 Mini-Redis was at 0.43–0.56×.
- **p99 is lower than Redis's in all four pipelined tests,** in both sessions: for `SET` with AOF on, 6.0 ms against 7.0. A table that grew all at once used to freeze every client for about 20 ms at 85,000 keys (Challenge 13), so the worst `SET` took 23–77 ms; with the table sized at startup the slowest insert takes 28–112 µs.
- **No request allocates memory:**
  - commands are parsed into reused strings;
  - replies are built straight into the client's buffer, AOF records straight into the log's.
  - [`bench/alloc_count`](bench/alloc_count.cpp) counts 0 allocations per `GET` or `SET`, where Phase 3's code made 5 and 11.
- **What is left is memory latency.** Both are cache misses on separately allocated nodes, and the next things to change:
  - the hash table's bucket walk: 24% of pipelined `SET`'s CPU, and most of the 31% `GET` spends in `cmdGet`, where it is inlined;
  - the LRU list relink: 9–11%.
- **With 1,000 idle clients, `epoll` uses 9–14× less CPU per busy request than `select()`**, which rebuilds and scans every socket on each wakeup.
- **With `--appendfsync always`, both servers wait for the disk and tie**: each syncs once per pass of its event loop, about 390 `SET`s per `fdatasync()` at pipeline 16.
- **Without pipelining, the tie says nothing about the code.** Neither server reaches 100% CPU: the cost of each network round trip sets the pace for both.
- **312 bytes of heap per key** for a 16-byte key and a 3-byte value.

Raw numbers vary between sessions on this machine (WSL2 on a hybrid CPU) by more than most code changes, so comparisons are made within one session, with the order of measurements alternating, and reported with their spread. The full results also say which differences were too small to resolve.

> **Why does pipelining help so much?** Without pipelining, every command waits a full network round trip. With P=16 the client sends 16 commands at once, the server reads them with a single `recv()` and sends all 16 replies with a single `send()`, and the round trip is shared by 16 commands.

Earlier numbers from Windows (one key for every request, unoptimised build) are kept in [stress-test-results/](stress-test-results/); they are not comparable.

---

## Architecture

```mermaid
flowchart TD
    Client1["redis-cli / Client 1"]
    Client2["redis-cli / Client 2"]
    Client3["redis-cli / Client N"]

    Server["TCP Server\n<i>epoll event loop</i>\nI/O multiplexing · RESP protocol"]
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

**Single-threaded event loop:** one thread waits on all sockets, with `epoll` on Linux (`select()` elsewhere), and runs each command to completion, so commands never race each other. Sockets are non-blocking, so the thread never waits on any one client. Redis's main thread uses the same model.

### Layered Design

| Layer | File(s) | Responsibility |
|-------|---------|----------------|
| **Network** | `Server.h/.cpp` | Accepting clients, the event loop, per-client buffers |
| **Readiness** | `Poller.h/.cpp` | Which sockets are ready: `epoll` on Linux, `select()` elsewhere, behind one interface |
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
- **`Poller` interface:** sockets are registered once, with what to watch for: always readability, and writability only while replies are waiting.
  - `EpollPoller` (Linux, the default): the kernel keeps the watched set, so a wait costs O(ready sockets).
  - `SelectPoller` (everywhere): each wait scans every watched socket, and on Linux it can only watch sockets numbered below 1024.
  - Both are level-triggered, so the event loop is the same for both; `--poller` chooses.
- **Group commit:** a pass of the loop reads from every ready client and runs its commands. Then one `write()` hands the OS the AOF records of all of them, and only then are the replies sent (as Redis does in `beforeSleep()`).
- **Client limit:** `--maxclients` (10,000 by default, as in Redis). A client beyond it, or beyond what `select()` can watch, gets `-ERR max number of clients reached` and is disconnected. The server raises its open-file limit at startup to fit them, and keeps a spare file descriptor to refuse clients properly if it runs out anyway (`EMFILE`).
- **No allocation per request:** commands are parsed into argument strings that are reused, replies are built straight into the client's buffer, and AOF records straight into the log's buffer. Once those have grown, a request allocates nothing (counted by [`bench/alloc_count`](bench/alloc_count.cpp)).
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
| Durability | Every write command's record reaches the OS before its reply is sent, so it survives a crash of the process. When it reaches the disk, which is what survives a power cut, is set by `--appendfsync`, as in Redis: `always` (synced before the reply), `everysec` (by a background thread once a second; the default) or `no` (when the OS decides). |
| Group commit | The records of every command in one pass of the event loop go out in one `write()`, and with `always` one `fdatasync()`. Measured with `strace`: 100,000 pipelined `SET`s took 132 `write()` calls, where they took 100,002; with `always`, one sync covers about 390 `SET`s at pipeline 16 and 25 without pipelining. |
| Binary safety | RESP format — spaces, newlines, any byte in values |
| Expiry times | Logged as absolute Unix times in milliseconds (`SET … PXAT`, `PEXPIREAT`), as Redis does, so a key that expired while the server was down stays expired. AOFs with the older `SETEX`/`EXPIRE` records still load. |
| Removals | Every key the server removes on its own, by LRU eviction or because it expired, is logged as a `DEL`. Replay then rebuilds exactly the keys, values and TTLs the server held (see Challenge 11). The LRU order isn't rebuilt: reads aren't logged. |
| Replay on startup | Each record runs through the same command table as client commands. While loading, nothing expires, nothing is evicted and nothing is logged; afterwards, keys beyond `--maxkeys` are evicted, and those evictions are logged. Takes time linear in the file size. |
| Damaged file | An incomplete last record, left by a crash mid-write, is cut off with a warning, as Redis's `aof-load-truncated` does. Damage before the end stops startup with the byte offset of the damage and the `truncate` command that keeps everything before it. |
| Failed writes | If a write fails (a full disk), the bytes not written wait in memory, in order, and are written once the file takes them. Meanwhile, write commands get `-MISCONF Errors writing to the AOF file: …` and reads keep working, as in Redis; a failed `everysec` sync does the same. With `always`, a failed write or sync can no longer be made good, so the server sends none of the waiting replies and stops with exit code 1, as Redis does. |
| Compaction | `BGREWRITEAOF` writes the data to a temporary file, `fsync`s it, renames it over the AOF and `fsync`s the directory, so a crash at any point leaves a complete AOF. Despite the name, it runs in the foreground; see [Known Limitations](#known-limitations). |
| Configuration | `--appendonly no` disables it; `--appendfilename` chooses the file; `--appendfsync` the sync policy |

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
- **The hash table is sized for the capacity at startup,** so it never rehashes while serving. `std::unordered_map` rehashes all at once, moving every key: the insert of the 85,230th key took about 20 ms, during which every client waited (Challenge 13). The bucket array costs 8 bytes per key of capacity, up front; it is capped at 4,194,304 keys (35 MB), beyond which the table grows by rehashing as before.
- **A read relinks the key's LRU node** with `std::list::splice`: no allocation and no second hash lookup.
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

**Later:** on Linux the server now uses `epoll`, which has no such limit, and refuses clients only beyond `--maxclients` (10,000 by default, as in Redis). The `select()` path is still tested, with `--poller select`.

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

### Challenge 13: A 20 ms Freeze Every Time the Table Grew

**Problem:** In every benchmark run, Mini-Redis's maximum `SET` latency (23–77 ms) was above its maximum `GET` latency (14–22 ms); Redis's `SET`s mostly stayed at 13–17 ms. One max from one run is a noisy sample, so the storage engine was measured directly instead: [`bench/insert_latency`](bench/insert_latency.cpp) times every insert of 100,000 new keys, with no network.

**Cause:** `std::unordered_map` grows all at once. The insert that crosses its load factor allocates a bucket array twice the size and moves every key into it. In 5 out of 5 repeats, the slowest inserts were exactly those:

| Insert of key | Rehash (buckets) | That one insert |
|---:|---|---:|
| 85,230 | 85,229 → 172,933 | 19.3–21.8 ms |
| 42,044 | 42,043 → 85,229 | 6.8–12.1 ms |
| any other | | p50 about 1 µs |

The server is single-threaded, so such a `SET` makes every client wait: about 800 requests at 50 clients pipelining 16. Only new keys trigger it, which is why only `SET`s showed it.

**Solution:** The table is sized for `--maxkeys` at startup (`reserve()`), as low-latency systems preallocate, so it never rehashes while serving: the slowest insert dropped to 0.15–0.33 ms. Redis instead rehashes incrementally, a few buckets per operation, which needs a hash table of its own. The cost is 8 bytes of buckets per key of capacity, allocated up front, capped at 4,194,304 keys (35 MB), so a huge `--maxkeys` meant as "unlimited" doesn't allocate gigabytes.

### Challenge 14: A Test That Failed Once in Eight Runs

**Problem:** The randomized replay test (Challenge 11) failed once in a 300-seed run under AddressSanitizer, then passed six identical runs, 1,800 seeds. A test that fails once is either a real bug or a test that can't be trusted, and both need finding.

**How it was found:**
- A sampler ran alongside the repeats, recording the offset between the wall clock and the monotonic clock every 100 ms.
- **WSL2 steps the wall clock every 30 seconds** to follow Windows: 21 steps in 11 minutes, from −341 ms to +60 ms.
- The AOF stores wall-clock expiry times and memory keeps monotonic ones, so a step between a command and the replay legitimately changes the result: a key that expired live can come back with time left. That is the documented trade-off of absolute expiry, not a bug.

**Solution:** The test measures the clock offset after every command. A mismatch counts as a failure only if the wall clock held steady; if it was stepped, the same seed runs again.

**Proof**, with an `LD_PRELOAD` shim that steps `CLOCK_REALTIME` and leaves `CLOCK_MONOTONIC` alone:
- the old test fails exactly as on WSL, and the new one passes;
- with the bug the test exists for put back (expired keys not logged), the new test still fails, on a steady clock and on a stepping one.

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

**97 unit tests** (`tests/test_main.cpp`):

```
========== Mini-Redis Test Suite ==========

[Basic Operations]      7 tests    ✅
[LRU Eviction]          7 tests    ✅  (includes: the table never rehashes up to its capacity)
[TTL Expiration]        5 tests    ✅
[TTL > LRU Priority]    2 tests    ✅
[Persistence]           4 tests    ✅  (the CLI's SAVE/LOAD)
[Controller Validation] 1 test     ✅
[List Operations]       6 tests    ✅
[Set Operations]        5 tests    ✅
[Hash Operations]       5 tests    ✅
[Type Safety]           4 tests    ✅
[LRU with Mixed Types]  1 test     ✅
[AOF Persistence]      26 tests    ✅  (absolute expiry, logged removals, randomized replay check, damaged files, failed writes, group commit, fsync policies)
[Command Table]        11 tests    ✅  (arity, SET options, strict integers, error replies, replies appended in place)
[Pollers]               2 tests    ✅  (select() and epoll pass the same checks)
[RESP Parser]          11 tests    ✅  (malformed input, size limits, every split point, reused argument strings)

Results: 97 passed, 0 failed
```

The randomized replay test runs 4 seeds by default; `RANDOM_TEST_SEEDS=300 ./build/tests` runs more. It compares wall-clock expiry times, so a seed whose results differ while the wall clock was stepped (WSL2 steps it every 30 s, by up to about a third of a second) is run again; a difference on a steady clock fails at once.

**Thread-safety stress tests** (`tests/test_thread_safety.cpp`):
- 5 tests, each running 8 threads at once against one storage instance (20,000 operations in total).
- They check for crashes and invalid results. Build with `-DSANITIZE=thread` to have ThreadSanitizer check for data races.

**Integration tests** (`tests/integration/test_server.py`, Python standard library only):
- 34 tests that start the real server binary on a free port and speak RESP over TCP.
- They cover:
  - basic commands, command-line flags, and AOF replay after a restart;
  - many clients pipelining at once, and `BGREWRITEAOF` in the middle of a pipeline (group commit);
  - a damaged AOF at startup, failed AOF writes under a file size limit (Challenge 12), and `--appendfsync always` stopping the server when they fail;
  - protocol errors and error replies;
  - a client that never reads its replies (Challenge 8);
  - 1,100 connections: refused beyond `select()`'s limit, served with `epoll`; `--maxclients`; running out of file descriptors (Challenge 10);
  - shutdown on `SIGINT`/`SIGTERM`;
  - the client-disconnect regression (Challenge 7).
- `ctest` runs them twice: with the default poller, and with `--poller select`, so the path `epoll` replaced as the default stays tested.

All four `ctest` runs pass with AddressSanitizer + UndefinedBehaviorSanitizer. The thread-safety and unit tests also pass under ThreadSanitizer, and so does the integration suite against a ThreadSanitizer build of the server (where the `everysec` sync thread runs alongside the event loop). The Windows build is compile-checked with MinGW-w64 (GCC 13) but not run.

---

## Project Structure

```
mini-redis/
├── server_main.cpp           # Server entry point (TCP mode, command-line flags)
├── main.cpp                  # CLI entry point (interactive mode)
├── Server.h / .cpp           # TCP server — event loop, per-client buffers, RESP I/O
├── Poller.h / .cpp           # Readiness interface: epoll (Linux) and select()
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
├── bench/                    # Measurement tools: throughput, commit comparison, replay, profile,
│                             #   memory, allocations per request, insert latency, idle clients
│   └── results/              # Measured results, one directory per version
├── stress-test-results/      # Earlier Windows benchmark screenshots (superseded)
└── tests/
    ├── test_main.cpp              # 97 unit tests
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

Ctrl+C (or `SIGTERM`) stops the server cleanly: it closes the connections, writes what is left of the AOF (and syncs it, unless `--appendfsync no`), and exits with code 0. With `--appendfsync always`, an AOF that can't be written or synced stops it with exit code 1.

| Option | Default | Meaning |
|--------|---------|---------|
| `--port <1-65535>` | 6379 | TCP port to listen on |
| `--maxkeys <n>` | 10000 | Keys to hold before LRU eviction |
| `--appendonly <yes\|no>` | yes | Log writes to the AOF and replay it at startup |
| `--appendfilename <path>` | appendonly.aof | AOF file; the server refuses to start if it can't be opened |
| `--appendfsync <always\|everysec\|no>` | everysec | When the AOF reaches the disk: before every reply, once a second, or when the OS decides |
| `--maxclients <n>` | 10000 | Clients served at once; more get an error and are disconnected |
| `--poller <epoll\|select>` | epoll on Linux, else select | How the event loop waits for sockets (`select()` serves about 1,000 clients) |

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
g++ -std=c++17 -O2 -pthread tests/test_main.cpp Poller.cpp Commands.cpp InMemoryStorage.cpp AOFLogger.cpp RedisLite.cpp -o run_tests
g++ -std=c++17 -O2 -pthread tests/test_thread_safety.cpp InMemoryStorage.cpp AOFLogger.cpp RedisLite.cpp -o run_thread_tests
```

On Windows (MinGW), add `-lws2_32` to the server and unit-test commands for Winsock.

---

## Design Trade-offs

| Decision | Why |
|----------|-----|
| **`epoll` on Linux, `select()` elsewhere** | Behind one `Poller` interface, so the event loop is the same for both. `epoll` keeps the watched set in the kernel and returns only ready sockets. `select()` is portable to Windows and macOS. **Cost of `select()`:** every call scans all sockets (O(N)), and about 1,020 clients at most: on Linux it can't watch socket numbers ≥ 1024, and on Windows the set holds 1024 sockets (this code raises Winsock's default of 64). There is no `kqueue` (macOS) or IOCP (Windows) version. |
| **Level-triggered `epoll`** | A ready socket is reported until it is drained, as with `select()`, so the loop can stop reading a client whose replies are waiting and pick it up later. Edge-triggered `epoll` saves wakeups, but every socket would have to be drained completely each time it is reported, or it would never be reported again. Redis uses level-triggered too. |
| **Pause a slow reader instead of disconnecting it** | When a client's replies pile up, the server stops reading from it until they are sent, so TCP slows that client down. Memory per client stays bounded without a limit at which clients are cut off. **Cost:** a client that never reads keeps its connection, and the replies already produced (a batch stops once 1 MB is waiting, after the reply that crossed it), until it closes. |
| **Single-threaded server** | Same model as Redis's main thread: commands run one at a time, so they can't race each other. **Cost:** one slow operation delays every client. |
| **RESP AOF over plain text** | Binary-safe. Values with spaces, newlines, special chars survive persistence. |
| **`everysec` by default, as in Redis** | A background thread syncs the AOF once a second, so a power cut loses at most about a second of writes, without any client waiting for the disk. `always` waits for the disk before every reply, which group commit makes affordable: one `fdatasync()` per pass of the loop, not per command. It syncs a `dup()` of the file descriptor without holding the logger's lock, so the event loop never waits for a slow disk. **Cost:** with `everysec`, up to a second of acknowledged writes can be lost. |
| **Group commit** | One `write()` (and with `always`, one sync) per pass of the event loop, not per command, with replies sent only after it. A reply still means the same as with one `write()` per command. **Cost:** a reply waits for the other clients' commands of the same pass to run. |
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
| The interactive CLI (`mini_redis`) still has `SAVE` and `LOAD`, in a text format that holds string keys only and isn't binary-safe. `LOAD` isn't logged to the AOF, so the next start goes back to the data from before it. | Remove them, as in the server |

**Commands and storage**

| Limitation | Planned fix |
|---|---|
| `SET`'s `NX`, `XX`, `GET` and `KEEPTTL` options are not supported (they return a syntax error rather than being ignored). | Implement them, with tests |
| The hash table is sized at startup for at most 4,194,304 keys. With a larger `--maxkeys`, it grows past that by rehashing all at once, which stalls every client for longer the bigger the table (20 ms at 85,000 keys, Challenge 13). | Incremental rehashing, as Redis does, with the storage rework (one key copy, intrusive LRU links) |

**Future scope:** sharding across nodes (consistent hashing), Pub/Sub (`SUBSCRIBE`/`PUBLISH`), `AUTH`.

---

<div align="center">

**Made by [Madhav Thesiya](https://www.linkedin.com/in/madhavthesiya/)** — If this was useful, drop a ⭐<br>
**© 2026 Madhav Thesiya. All Rights Reserved.**

</div>
