# Commands

Every command Kosh supports, the data types, and what each costs.

## Supported Data Types

| Type | Commands | Internal Structure |
|------|----------|-------------------|
| **String** | `SET`, `GET`, `SETEX`, `MGET`, `MSET`, `INCR`, `DECR`, `INCRBY`, `DECRBY` | `std::string` |
| **List** | `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LRANGE`, `LLEN` | `std::deque<string>` |
| **Set** | `SADD`, `SREM`, `SMEMBERS`, `SISMEMBER`, `SCARD` | `std::unordered_set<string>` |
| **Hash** | `HSET`, `HGET`, `HDEL`, `HGETALL`, `HLEN` | `std::unordered_map<string,string>` |

All types are stored in a single `std::variant`, with lists, sets and hashes behind a pointer:
- **Benefits:** no virtual calls, type checks at compile time, and strings (most values) stored inline, so a `GET` follows no extra pointer.
- **Why the pointer:** a variant is as large as its largest alternative. With a `std::deque` inline (80 bytes), every value, strings included, took 88 bytes; boxed, it takes 40.

Measured on x86-64 Linux with GCC 13 (libstdc++):

| Type | `sizeof` (bytes) |
|------|:----------------:|
| `std::string` | 32 |
| `std::deque<std::string>` | 80 |
| `std::unordered_set` / `std::unordered_map` | 56 |
| `ValueType` (a string, or a pointer to a list, set or hash) | 40 |
| `CacheEntry` (value, expiry, eviction-queue links, key length, flags) | 72 |
| `KeyTable` bucket (one cache line) | 64 |

A key costs one allocation: its `CacheEntry` with the key's bytes right after it. With 16-byte keys and 3-byte values, that is 100–113 bytes per key in all, about what Redis uses (see [the key table](ARCHITECTURE.md#4-the-key-table-and-eviction)). Other compilers and standard libraries give different sizes.


## Full Command Reference

### String Commands
| Command | Syntax | Description |
|---------|--------|-------------|
| `SET` | `SET key value` | Set key-value pair |
| `SET` | `SET key value EX seconds` | Set with a TTL. Also `PX milliseconds`, `EXAT unix-seconds` and `PXAT unix-milliseconds`. The number must be a positive integer, with Redis's error replies. `NX`, `XX`, `GET` and `KEEPTTL` are not supported and return a syntax error. |
| `SETEX` | `SETEX key seconds value` | Set with TTL (alternative) |
| `GET` | `GET key` | Get value |
| `MGET` | `MGET key1 key2 ...` | Each value, or nil where there is none, or a key holds another type (no `WRONGTYPE`, as in Redis) |
| `MSET` | `MSET key1 value1 key2 value2 ...` | Sets them all at once: one lock, one AOF record. Like `SET`, drops a key's old type and TTL |
| `INCR` / `DECR` | `INCR key` | Adds 1 (or -1) to the integer in the key; a missing key counts from 0. A TTL stays |
| `INCRBY` / `DECRBY` | `INCRBY key n` | Adds `n` (or `-n`). An integer means its canonical form, as in Redis: `"05"` isn't one, so `INCR` never rewrites a string like a zip code. A sum outside 64 bits is an error that changes nothing |

### Key Commands
| Command | Syntax | Description |
|---------|--------|-------------|
| `DEL` | `DEL key1 key2 ...` | Deletes the keys; replies how many there were |
| `EXISTS` | `EXISTS key1 key2 ...` | How many of the keys exist; a key given twice counts twice, as in Redis |
| `TYPE` | `TYPE key` | Get data type |
| `EXPIRE` | `EXPIRE key seconds` | Set TTL; 0 or less deletes the key at once, as in Redis |
| `PEXPIRE` | `PEXPIRE key milliseconds` | Set TTL in milliseconds; 0 or less deletes the key at once |
| `PEXPIREAT` | `PEXPIREAT key unix-milliseconds` | Set the expiry as an absolute time (the form the AOF records) |
| `PERSIST` | `PERSIST key` | Remove the TTL; 1 if there was one, 0 if not or no key |
| `TTL` | `TTL key` | Remaining TTL in seconds, rounded to the nearest second; -1 without a TTL, -2 if the key doesn't exist |
| `PTTL` | `PTTL key` | Remaining TTL in milliseconds; -1 without a TTL, -2 if the key doesn't exist |
| `DBSIZE` | `DBSIZE` | How many keys there are. Expiry is lazy, so it first removes keys whose TTL has run out (Redis's background cycle would have removed most of them) |
| `SCAN` | `SCAN cursor [MATCH pattern] [COUNT n] [TYPE type]` | Iterate the keys a few at a time; start at 0, stop when the reply's cursor is 0. A key present for the whole iteration comes back at least once, even if the table grows meanwhile (it can come twice). `MATCH` takes Redis's glob patterns (`*`, `?`, `[a-z]`, `[^x]`, `\`) |

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
| `PING` | `PING [message]`: PONG, or the message back |
| `AUTH` | `AUTH password` or `AUTH default password`: authenticates the connection when the server has a password (see [Who can connect](RUNNING.md#run-the-server)). A wrong one gets `-WRONGPASS` and changes nothing. |
| `HELLO` | `HELLO [2\|3] [AUTH default password] [SETNAME name]`: picks the connection's protocol, RESP2 (the default) or RESP3, can authenticate and name it, and describes the server. In RESP3, a missing value is `_`, `HGETALL` and `CONFIG GET` reply with maps, `SMEMBERS` with a set, and `INFO` with a verbatim string, as in Redis. |
| `CLIENT` | `ID`, `GETNAME`, `SETNAME name`, `HELP`. Other subcommands get Redis's "unknown subcommand" error, which clients skip. |
| `INFO` | `INFO [section ...]`: the server's state, in Redis's format and with its field names: Server, Clients, Memory (resident memory, the key limit), Persistence, Stats (connections, commands processed, ops per second, keyspace hits and misses, expired and evicted keys) and Keyspace. The counters count what Redis counts (checked on the same commands). There's no `redis_version`: Kosh isn't a complete Redis of any version, and tools decide what to send by it. |
| `FLUSHALL`, `FLUSHDB` | `FLUSHALL [ASYNC\|SYNC]`: removes every key and gives the key table's memory back. Kosh has one database, so `FLUSHDB` does the same. Always at once: `ASYNC` is accepted and served synchronously. Written to the AOF, so a restart replays it. |
| `BGREWRITEAOF` | Compacts the AOF through a temporary file and an atomic rename. Runs in the foreground, blocking other clients. Returns an error when AOF is disabled, or when the rewrite fails; the AOF is then unchanged. |
| `QUIT` | Replies `OK`, then closes the connection once the reply is sent. Commands after it in the same pipeline don't run. |

There is no `SAVE`: the AOF is the server's only persistence. A wrong number of arguments gets Redis's `-ERR wrong number of arguments for '…' command`, and an unknown command Redis's `-ERR unknown command '…', with args beginning with: …`, byte for byte. While AOF writes are failing, write commands get `-MISCONF` (see AOF Persistence).

### Transactions
| Command | Description |
|---------|-------------|
| `MULTI` | Starts a transaction: until `EXEC`, each command is checked and queued (`+QUEUED`) instead of run |
| `EXEC` | Runs the queued commands one after another and replies with an array of their replies. Nothing else runs in between. |
| `DISCARD` | Drops the queued commands |

They behave as Redis's, replies included (redis-py's `pipeline()` uses them by default):
- **A command refused while it queues aborts the transaction.** That means an unknown command, the wrong number of arguments, or a write while the AOF is failing. `EXEC` then replies `-EXECABORT`, and nothing runs.
- **A command that fails as it runs doesn't stop the others,** for example `WRONGTYPE`. Its error is one element of `EXEC`'s array. There is no rollback.
- **In the AOF, a transaction that writes is logged between `MULTI` and `EXEC` records.** At startup it is applied only once its `EXEC` is read. One the server stopped writing partway is cut off, so none of it comes back, not a part of it.
- **Not supported:** `WATCH`. `BGREWRITEAOF` is refused inside a transaction (Redis allows it), because Kosh's rewrite runs in the foreground and would split the transaction's records in the AOF.


## Complexity

| Operation | Time | Space |
|-----------|:----:|:-----:|
| SET / GET / DEL | O(1) average, plus O(log T) when a TTL is set or removed | O(1) |
| MGET / MSET / DEL / EXISTS with K keys | O(K) average | O(K) for MGET's reply |
| INCR / DECR / INCRBY / DECRBY | O(1) average | O(1) |
| Making room for a new key | O(log T) per expired key removed, then O(1) amortized to evict (SIEVE's hand clears one bit per step; LRU takes the oldest) | O(1) |
| LPUSH / RPUSH | O(1) per element | O(1) per element |
| LPOP / RPOP | O(1) | O(1) |
| LRANGE | O(K) | O(K) |
| SADD / SREM / SISMEMBER | O(1) average per member | O(1) |
| HSET / HGET / HDEL | O(1) average per field | O(1) |
| BGREWRITEAOF | O(N) | O(N) |
| AOF replay at startup | O(F) | O(F) |
| SAVE / LOAD (CLI only) | O(N) | O(N) |

N = number of stored keys (bounded by `--maxkeys`), T = keys with a TTL, K = elements returned, F = size of the AOF: replay reads the whole file into memory first. Overall space: **O(N)**.
