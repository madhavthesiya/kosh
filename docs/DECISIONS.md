# Decisions

The problems that shaped Kosh, how each was found, and the trade-offs that were made.

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

**Problem:** On Linux, `select()`'s `fd_set` is a bit array for sockets 0–1023. The 1,021st client got socket 1024, and the server read and wrote past the end of `fd_set`. Measured at [v0-baseline](../bench/results/v0-baseline/README.md#5-the-select-limit-bug-8):
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

**Problem:** In every benchmark run, Kosh's maximum `SET` latency (23–77 ms) was above its maximum `GET` latency (14–22 ms); Redis's `SET`s mostly stayed at 13–17 ms. One max from one run is a noisy sample, so the storage engine was measured directly instead: [`bench/insert_latency`](../bench/insert_latency.cpp) times every insert of 100,000 new keys, with no network.

**Cause:** `std::unordered_map` grows all at once. The insert that crosses its load factor allocates a bucket array twice the size and moves every key into it. In 5 out of 5 repeats, the slowest inserts were exactly those:

| Insert of key | Rehash (buckets) | That one insert |
|---:|---|---:|
| 85,230 | 85,229 → 172,933 | 19.3–21.8 ms |
| 42,044 | 42,043 → 85,229 | 6.8–12.1 ms |
| any other | | p50 about 1 µs |

The server is single-threaded, so such a `SET` makes every client wait: about 800 requests at 50 clients pipelining 16. Only new keys trigger it, which is why only `SET`s showed it.

**First solution (Phase 4):** The table was sized for `--maxkeys` at startup (`reserve()`), as low-latency systems preallocate, so it never rehashed while serving: the slowest insert dropped to 0.15–0.33 ms. The cost was 8 bytes of buckets per key of capacity, up front, and a cap (4,194,304 keys), beyond which the stall came back.

**Solution now (Phase 4B):** a hash table of its own, [`KeyTable.h`](../src/KeyTable.h), that grows incrementally, as Redis's dict does: it allocates a table twice the size and moves one bucket chain per insert or erase, and lookups check both tables meanwhile. Nothing is sized up front and there is no cap. Growing to 1,000,000 keys, the slowest insert took 272–405 µs, at scattered positions ([results](../bench/results/after-memory-fixes/insert-latency.md)). Getting there took a second hunt: Challenge 15.

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

### Challenge 15: The Stall That Was the Allocator

**Problem:** With the incremental table, growing to 1,000,000 keys still had inserts of 2.9–4.1 ms, at the same positions in every repeat: where a rehash starts, and shortly after one ends. Incremental rehashing exists to prevent exactly that.

**How it was found**, one measurement at a time:

| Finding | Evidence | Fix |
|---|---|---|
| When a rehash finished, the old table's every bucket was read again, to free child buckets that the rehash steps had already freed | the stall positions pointed at rehash ends; the code did a full scan there | free the block, no scan |
| `calloc` gives lazily zeroed pages only for blocks glibc maps from the OS, and glibc raises that size after it frees a big block; below it, a new table comes from the heap and is zeroed byte by byte | a stall where a growth *started* | tables of 1 MB or more are `mmap`ed directly |
| Freeing megabytes at once makes the kernel release every page in one call | `strace`: `munmap` of 7 MB took 1.6 ms | the moved part of the old table is given back as the rehash goes, 64 KB at a time (`madvise(MADV_DONTNEED)`) |
| A 2 ms stall 60–136 inserts *after* each rehash ended, with no slow system call | a program timing a bare table, every entry allocated up front, still showed it; switching glibc's fastbins off (`GLIBC_TUNABLES=glibc.malloc.mxfast=0`) cut it to 0.3–0.8 ms | freed child buckets are kept and reused, so they never go back to `malloc` |

The last one is the lesson: a rehash frees thousands of small blocks at once. glibc parks them unmerged in its fastbins and merges them all later, inside one unlucky insert. Redis's dict moves its entry nodes during a rehash rather than freeing them, and it allocates with jemalloc, whose small blocks go back to fixed-size slabs with no merging pass.

**Result:** the bare table's slowest insert went from 2.1–2.4 ms at fixed positions to 0.33–0.8 ms at scattered ones, some outside any rehash, which is this VM's own noise. Through the engine, on a quiet machine: 272–405 µs.

**A null result, kept:** giving back 16 KB at a time instead of 64 KB made no difference that alternating runs could resolve, so 64 KB (fewer system calls) stayed.


## Design Trade-offs

| Decision | Why |
|----------|-----|
| **`epoll` on Linux, `select()` elsewhere** | Behind one `Poller` interface, so the event loop is the same for both. `epoll` keeps the watched set in the kernel and returns only ready sockets. `select()` is portable to Windows and macOS. **Cost of `select()`:** every call scans all sockets (O(N)), and about 1,020 clients at most: on Linux it can't watch socket numbers ≥ 1024, and on Windows the set holds 1024 sockets (this code raises Winsock's default of 64). There is no `kqueue` (macOS) or IOCP (Windows) version. |
| **Level-triggered `epoll`** | A ready socket is reported until it is drained, as with `select()`, so the loop can stop reading a client whose replies are waiting and pick it up later. Edge-triggered `epoll` saves wakeups, but every socket would have to be drained completely each time it is reported, or it would never be reported again. Redis uses level-triggered too. |
| **Pause a slow reader instead of disconnecting it** | When a client's replies pile up, the server stops reading from it until they are sent, so TCP slows that client down. Memory per client stays bounded without a limit at which clients are cut off. **Cost:** a client that never reads keeps its connection, and the replies already produced (a batch stops once 1 MB is waiting, after the reply that crossed it), until it closes. |
| **One thread by default** | Same model as Redis's main thread: commands run one at a time, so they can't race each other. **Cost:** one slow operation delays every client. |
| **Shard-per-core for several threads, not one locked map** | Each thread owns its keys, so no two threads touch the same data. Measured against N event loops over one locked map: 2.3× at 4 cores on GET, and the locked map gets slower as threads are added. **Cost:** a command for another thread costs a trip through a ring (CPU per request grew from 2.6 µs to 5.5 µs at 4 cores), and commands across threads lose atomicity or are refused. |
| **Refuse `MSET` and `MULTI` across threads** | Redis documents `MSET` as atomic, and Redis Cluster refuses it across slots, so a promise is either kept or refused. A `{tag}` keeps keys together. **Cost:** an application must tag its keys. |
| **One AOF file per thread** | One shared file would make every thread queue on one lock. **Cost:** after a power cut, the files can stop at different points, and the thread count can't change between restarts. |
| **RESP AOF over plain text** | Binary-safe. Values with spaces, newlines, special chars survive persistence. |
| **`everysec` by default, as in Redis** | A background thread syncs the AOF once a second, so a power cut loses at most about a second of writes, without any client waiting for the disk. `always` waits for the disk before every reply, which group commit makes affordable: one `fdatasync()` per pass of the loop, not per command. It syncs a `dup()` of the file descriptor without holding the logger's lock, so the event loop never waits for a slow disk. **Cost:** with `everysec`, up to a second of acknowledged writes can be lost. |
| **Group commit** | One `write()` (and with `always`, one sync) per pass of the event loop, not per command, with replies sent only after it. A reply still means the same as with one `write()` per command. **Cost:** a reply waits for the other clients' commands of the same pass to run. |
| **Monotonic clock in memory, Unix time in the AOF** | `steady_clock` can't jump when the system clock is changed, so a TTL lasts as long as it says. But it starts again from an arbitrary point when the machine reboots, so the file needs wall-clock time. **Cost:** expiry is converted between the two clocks at the AOF, and a change of the system clock moves the expiry of every key loaded at the next start (Redis has the same property). |
| **Refuse writes while the AOF can't be written** | Same as Redis's `MISCONF`. Acknowledging writes that can't be saved would lose them silently at the next restart; exiting would take reads down too. **Cost:** a full disk makes the server read-only until there is room. |
| **Reject unsupported `SET` options** | `NX`, `XX`, `GET` and `KEEPTTL` return a syntax error. Ignoring `NX` would let `SET lock me NX` take a lock that someone else holds. |
| **`TCP_NODELAY` enabled** | Disables Nagle's algorithm. Trades bandwidth efficiency for latency — correct for request-response protocols. |
| **`std::variant` over inheritance** | No vtable pointer, compile-time type checks, strings stored inline. Lists, sets and hashes are behind a pointer, so the variant is 40 bytes, not the 88 of its largest alternative. **Cost:** creating a list, set or hash allocates once more, and reading one follows one more pointer. |
| **A hash table of its own over `std::unordered_map`** | Cache-line buckets with one-byte hash tags: a lookup is two dependent memory reads, not three or four, and the key lives inside its entry (one allocation per key). It grows incrementally, so no insert moves every key. **Cost:** code to own and test (a differential test against `std::unordered_map`, planted bugs), 14% slower lookups when everything fits in cache, and it never shrinks. Flat tables such as `absl::flat_hash_map` were ruled out: they move elements when they grow, and the eviction queue and expiry index point at entries. |
| **SIEVE by default, LRU on request** | A SIEVE hit writes one bit in the entry the lookup already read; an LRU hit relinks three entries. SIEVE missed 13–17% less on Zipf traces and up to 5% less on Twitter's, and 5–19% less than Redis's own sampled LRU. **Cost:** it isn't scan-resistant on some block-storage workloads (per its authors), so LRU stays available. |
| **Lazy TTL over active expiration** | No background thread or timer. O(1) per access. **Cost:** expired keys use memory until they're accessed or a new key needs their room. Redis also samples keys with a TTL about 10 times a second. |
| **`shared_mutex` in the storage, a lock-free ring between threads** | The engine's lock is correct and easy to reason about, and in the server it is never contended, as a thread owns its storage. The queue between threads is where contention would be, so that is the one lock-free structure: two threads per ring, so acquire and release suffice. A locked inbox cost about 30× as much per message. |
| **Assert-based tests over gtest** | Zero dependencies (the integration tests use only Python's standard library). Tests are the spec — readable by anyone. |
