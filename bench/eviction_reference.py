#!/usr/bin/env python3
"""Textbook eviction policies, to check the engine's against: the same requests
must give exactly the same number of misses.

eviction_sim --dump-trace writes the requests it ran: a "# trace NAME" line,
then one key id per line (ids are 0 .. distinct-1). This replays them through
a plain implementation of each policy, with the same read-through rule (a miss
inserts the key) and the same capacities, and prints the misses. With
--expect, it reads eviction_sim's report and fails if any count differs.

It also simulates Redis 7.0.15's allkeys-lru (--policy redis-lru), which
the engine doesn't have: what "LRU" means in Redis, to compare SIEVE with.
Its sampling is random, so it runs several seeds and reports the median and
range. A trace can also be read from its CSV (csv:PATH), for the time column
Redis's LRU clock needs.

Usage:
  ./build-bench/eviction_sim --dump-trace trace.txt zipf:0.99 > sim.md
  python3 bench/eviction_reference.py trace.txt --expect sim.md
  python3 bench/eviction_reference.py trace.txt csv:twitter_cluster52.csv --policy redis-lru
"""

import argparse
import multiprocessing
import os
import random
import re
import statistics
import sys
from array import array
from collections import OrderedDict


def lru_misses(ids, capacity):
    """LRU: a hit moves the key to the most recently used end; a miss evicts
    from the least recently used end when full, then inserts."""
    cache = OrderedDict()
    misses = 0
    for key in ids:
        if key in cache:
            cache.move_to_end(key)
        else:
            misses += 1
            if len(cache) >= capacity:
                cache.popitem(last=False)
            cache[key] = None
    return misses


def sieve_misses(ids, capacity):
    """SIEVE, as in the paper's pseudocode (Zhang et al., NSDI 2024): one FIFO
    queue, a hand, and a visited bit per key. A hit sets the bit. A new key
    goes in at the head. To evict, the hand walks from the tail towards the
    head (wrapping to the tail), clearing set bits, and evicts the first key
    whose bit is clear; keys it passes stay where they are, and the hand
    stays at the evicted key's neighbour towards the head."""
    toward_head = {}    # key -> the next key towards the head (None at the head)
    toward_tail = {}    # key -> the next key towards the tail (None at the tail)
    visited = {}
    head = tail = hand = None
    misses = 0
    for key in ids:
        if key in visited:
            visited[key] = True
            continue
        misses += 1
        if len(visited) >= capacity:
            obj = hand if hand is not None else tail
            while visited[obj]:
                visited[obj] = False
                obj = toward_head[obj] if toward_head[obj] is not None else tail
            hand = toward_head[obj]
            # unlink obj
            before, after = toward_tail[obj], toward_head[obj]
            if before is not None:
                toward_head[before] = after
            else:
                tail = after
            if after is not None:
                toward_tail[after] = before
            else:
                head = before
            del toward_head[obj], toward_tail[obj], visited[obj]
        # insert at the head
        toward_head[key], toward_tail[key] = None, head
        if head is not None:
            toward_head[head] = key
        head = key
        if tail is None:
            tail = key
        visited[key] = False
    return misses


POLICIES = {"lru": lru_misses, "sieve": sieve_misses}


# ---- Redis 7.0.15's allkeys-lru, step by step as its source does it ----
#
# From the redis/redis source at tag 7.0.15:
#   maxmemory-samples, default 5          config.c:3076
#   EVPOOL_SIZE 16                        evict.c:54
#   the LRU clock: seconds, 24 bits       server.h:847-849 (LRU_CLOCK_RESOLUTION 1000 ms),
#                                         refreshed by serverCron (server.c:1263) 10 times
#                                         a second (CONFIG_DEFAULT_HZ, server.h:104)
#   a read sets the key's lru to it       db.c:120 (lookupKey); a new key too, object.c:54
#   idle = clock - lru                    evict.c:92-99 (estimateObjectIdleTime)
#   the eviction pool                     evict.c:146-240 (evictionPoolPopulate)
#   picking the victim                    evict.c:593-633 (performEvictions)
#   sampling a run of buckets             dict.c:711-788 (dictGetSomeKeys)
#   the table                             dict.c:344 (a new key at the head of its chain),
#                                         dict.c:1010-1038 (doubling when keys = buckets)
# What differs: hashes are a seeded 64-bit mix standing in for SipHash under
# Redis's random key (only their randomness matters), the table is rehashed at
# once rather than incrementally (it only grows while the cache fills), and
# the 24-bit clock never wraps (that takes 194 days).

REDIS_SAMPLES = 5
EVPOOL_SIZE = 16
MASK64 = (1 << 64) - 1


def splitmix64(x):
    x = (x + 0x9E3779B97F4A7C15) & MASK64
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & MASK64
    return x ^ (x >> 31)


class RedisDict:
    """Redis's dict, as far as sampling needs it: a power-of-two table of chains"""

    def __init__(self, rng):
        self.rng = rng
        self.salt = rng.getrandbits(64)
        self.buckets = []
        self.used = 0

    def _index(self, key):
        return splitmix64(key ^ self.salt) & (len(self.buckets) - 1)

    def add(self, key):
        if not self.buckets:
            self._resize(4)                                 # DICT_HT_INITIAL_SIZE
        elif self.used >= len(self.buckets):
            self._resize(1 << self.used.bit_length())       # the next power of two above used
        self.buckets[self._index(key)].insert(0, key)       # at the head of its chain
        self.used += 1

    def remove(self, key):
        self.buckets[self._index(key)].remove(key)
        self.used -= 1

    def _resize(self, size):
        old, self.buckets = self.buckets, [[] for _ in range(size)]
        for chain in old:
            for key in chain:                               # each to the head of its new chain
                self.buckets[self._index(key)].insert(0, key)

    def get_some_keys(self, count):
        """dictGetSomeKeys: from a random bucket on, the keys of consecutive
        buckets, jumping elsewhere after a run of empty ones, and taking a
        chain past `count` by reservoir sampling. Not uniform over keys; at
        most count * 10 buckets are looked at."""
        count = min(count, self.used)
        maxsteps = count * 10
        mask = len(self.buckets) - 1
        i = self.rng.getrandbits(64) & mask
        emptylen = stored = 0
        out = []
        while stored < count and maxsteps > 0:
            maxsteps -= 1
            chain = self.buckets[i]
            if not chain:
                emptylen += 1
                if emptylen >= 5 and emptylen > count:
                    i = self.rng.getrandbits(64) & mask
                    emptylen = 0
            else:
                emptylen = 0
                for key in chain:
                    if stored < count:
                        out.append(key)
                    else:
                        r = self.rng.getrandbits(64) % (stored + 1)
                        if r < count:
                            out[r] = key
                    stored += 1
                if stored >= count:
                    break
            i = (i + 1) & mask
        return out


def pool_insert(pool, key, idle):
    """evictionPoolPopulate's insertion: the pool is sorted by idle time,
    ascending, empty slots (None) at the right. A key ties to the left of
    keys with the same idle time (the comparison is strict). The same key can
    be in the pool twice, and an entry's idle time is never updated."""
    k = 0
    while k < EVPOOL_SIZE and pool[k] is not None and pool[k][1] < idle:
        k += 1
    if k == 0 and pool[EVPOOL_SIZE - 1] is not None:
        return                                          # worse than all, and no room
    if k < EVPOOL_SIZE and pool[k] is None:
        pass                                            # an empty slot
    elif pool[EVPOOL_SIZE - 1] is None:
        pool[k + 1:] = pool[k:EVPOOL_SIZE - 1]          # room on the right: shift right
    else:
        k -= 1                                          # full: drop the best-kept on the left
        pool[0:k] = pool[1:k + 1]
    pool[k] = (key, idle)


def redis_lru_misses(ids, capacity, clock, seed, sample_all=False):
    """allkeys-lru, with the same read-through rule as the others (a miss SETs
    the key) and Redis's order: a SET that takes memory over the limit is
    served, and the next command evicts first (processCommand calls
    performEvictions before every command). So the key just written is a
    candidate. clock(i) is the LRU clock, in seconds, at request i.
    sample_all: sample every key (for the self-test)."""
    rng = random.Random(seed)
    table = RedisDict(rng)
    lru = {}                        # key -> the clock when last used (robj->lru)
    pool = [None] * EVPOOL_SIZE     # (key, idle when sampled)
    misses = 0
    for i, key in enumerate(ids):
        now = clock(i)
        if len(lru) > capacity:
            victim = None
            while victim is None:
                for sampled in (list(lru) if sample_all else table.get_some_keys(REDIS_SAMPLES)):
                    pool_insert(pool, sampled, now - lru[sampled])
                for k in range(EVPOOL_SIZE - 1, -1, -1):    # from the best candidate down
                    if pool[k] is None:
                        continue
                    candidate = pool[k][0]
                    pool[k] = None
                    if candidate in lru:                    # else a ghost: evicted since
                        victim = candidate
                        break
            table.remove(victim)
            del lru[victim]
        if key in lru:
            lru[key] = now
        else:
            misses += 1
            table.add(key)
            lru[key] = now
    return misses


def read_csv_trace(path):
    """(name, ids, times) from a trace CSV: the time in column 1 (seconds),
    the key in column 2; ids in order of first appearance, as eviction_sim
    numbers them, and the name its file name, as eviction_sim's"""
    seen, ids, times = {}, array("I"), array("q")
    with open(path) as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            columns = line.split(",")
            ids.append(seen.setdefault(columns[1].strip(), len(seen)))
            times.append(int(columns[0]))
    return os.path.basename(path), ids, times


def read_traces(path):
    """Yields (name, ids) from an --dump-trace file, one trace at a time, the
    ids in a compact array (4 bytes each: a list of Python ints would take
    about 36, over a gigabyte for a few 10-million-request traces)."""
    name, ids = None, None
    with open(path) as f:
        for line in f:
            if line.startswith("# trace "):
                if name is not None:
                    yield name, ids
                name, ids = line[len("# trace "):].strip(), array("I")
            elif line.strip():
                ids.append(int(line))
    if name is not None:
        yield name, ids


def read_expected(path, policy):
    """{(trace, fraction): misses} from eviction_sim's Markdown table, for
    the rows of one policy, finding the columns by their headers"""
    expected = {}
    columns = {}
    for line in open(path):
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if "Misses" in cells:
            columns = {name: i for i, name in enumerate(cells)}
        elif columns and len(cells) == len(columns) and re.fullmatch(r"\d+", cells[columns["Misses"]]):
            if "Eviction" in columns and cells[columns["Eviction"]] != policy:
                continue
            expected[(cells[columns["Trace"]], float(cells[columns["Capacity"]]))] = \
                int(cells[columns["Misses"]])
    return expected


def self_test():
    """Small cases worked out by hand, where the policies must differ."""
    # Capacity 2, A B B A C A. LRU: the hits leave A most recent, so C evicts
    # B, and the last A hits: 3 misses. SIEVE: A and B are both visited; the
    # hand clears A (the tail), then B, wraps round and evicts A, which keeps
    # its place in the queue whatever its hits: the last A misses: 4.
    trace = list("ABBACA")
    assert lru_misses(trace, 2) == 3, lru_misses(trace, 2)
    assert sieve_misses(trace, 2) == 4, sieve_misses(trace, 2)
    # SIEVE's hand stays where it stopped. Capacity 3, A B C, A hit, then D:
    # the hand clears A and evicts B, and stops at C. E then evicts C (not A,
    # which is the tail but was already passed): A still hits.
    trace = list("ABCADEA")
    assert sieve_misses(trace, 3) == 5, sieve_misses(trace, 3)
    # Capacity 1: every new key evicts the only one
    assert lru_misses(list("AABBA"), 1) == 3 and sieve_misses(list("AABBA"), 1) == 3

    # Redis's pool keeps a key's idle time from when it was sampled. Capacity
    # 3, every key sampled. At t11 the pool gets Z 11, X 10, Y 2, W 1 and Z is
    # evicted; X is then read. At t13 the pool still holds "X 10", higher than
    # any key's idle time now (Y's is 4), so Redis evicts X, read two seconds
    # before, and the last X misses: 6. Exact LRU evicts Y and the X hits: 5.
    trace, times = [0, 1, 2, 3, 1, 4, 1], [0, 1, 9, 10, 11, 12, 13]     # Z X Y W X V X
    assert redis_lru_misses(trace, 3, times.__getitem__, 1, sample_all=True) == 6
    assert lru_misses(trace, 3) == 5
    # Ties go left: with one clock tick, the pool keeps sampling order, and
    # the rightmost entry, sampled first, goes. A B C, then D: A is evicted.
    assert redis_lru_misses([0, 1, 2, 3, 0], 3, lambda i: 0, 1, sample_all=True) == 5
    # dictGetSomeKeys returns 5 keys that exist, and reaches every key
    table, rng = RedisDict(random.Random(7)), random.Random(8)
    for key in range(1000):
        table.add(key)
    for key in range(0, 1000, 2):
        table.remove(key)
    reached = set()
    for _ in range(2000):
        keys = table.get_some_keys(REDIS_SAMPLES)
        assert len(keys) == REDIS_SAMPLES and all(k % 2 == 1 for k in keys), keys
        reached.update(keys)
    assert len(reached) == 500, len(reached)
    print("self-test passed")


# name -> (ids, times or None); loaded before the worker processes fork, so
# they share it instead of each reading the traces
TRACES = {}


def redis_job(name, capacity, seed, clock_kind, rate):
    ids, times = TRACES[name]
    if clock_kind == "exact":
        clock = lambda i: i                     # a tick per request: no ties
    elif times is not None:
        clock = times.__getitem__               # the trace's own seconds
    else:
        clock = lambda i: i // rate             # `rate` requests per second
    return redis_lru_misses(ids, capacity, clock, seed)


def run_redis_lru(args, fractions):
    seeds = [int(s) for s in args.seeds.split(",")]
    jobs = [(name, max(1, int(f * (max(ids) + 1))), seed, args.clock, args.rate)
            for name, (ids, _) in TRACES.items() for f in fractions for seed in seeds]
    if "fork" in multiprocessing.get_all_start_methods() and args.jobs > 1:
        with multiprocessing.get_context("fork").Pool(args.jobs) as workers:
            results = workers.starmap(redis_job, jobs)
    else:
        results = [redis_job(*job) for job in jobs]
    print("| Trace | Capacity | Keys | Clock | Seeds | Misses (median) | Fewest | Most | Miss ratio |")
    print("|---|---:|---:|---|---:|---:|---:|---:|---:|")
    for at in range(0, len(jobs), len(seeds)):
        name, capacity = jobs[at][0], jobs[at][1]
        misses = results[at:at + len(seeds)]
        requests = len(TRACES[name][0])
        clock = ("a tick per request" if args.clock == "exact" else
                 "the trace's, 1 s" if TRACES[name][1] is not None else f"1 s, {args.rate} requests/s")
        fraction = fractions[(at // len(seeds)) % len(fractions)]
        print(f"| {name} | {fraction:g} | {capacity} | {clock} | {len(seeds)} | "
              f"{statistics.median(misses):.0f} | {min(misses)} | {max(misses)} | "
              f"{statistics.median(misses) / requests:.4f} |", flush=True)


def main():
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        return
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("traces", nargs="+", help="files written by eviction_sim --dump-trace, or csv:PATH")
    p.add_argument("--capacities", default="0.001,0.01,0.1")
    p.add_argument("--policy", default="lru", choices=sorted(POLICIES) + ["redis-lru"])
    p.add_argument("--expect", help="eviction_sim's report, to compare the misses with")
    p.add_argument("--seeds", default="1,2,3,4,5", help="redis-lru: one run per seed")
    p.add_argument("--clock", default="trace", choices=["trace", "exact"],
                   help="redis-lru's LRU clock: 'trace', in seconds from the CSV's time column, "
                        "or --rate requests a second; 'exact', a tick per request (no ties)")
    p.add_argument("--rate", type=int, default=2000,
                   help="redis-lru: requests per second, for a trace without times")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="redis-lru: processes")
    args = p.parse_args()

    for spec in args.traces:
        if spec.startswith("csv:"):
            name, ids, times = read_csv_trace(spec[len("csv:"):])
            TRACES[name] = (ids, times)
        else:
            for name, ids in read_traces(spec):
                TRACES[name] = (ids, None)
    fractions = [float(c) for c in args.capacities.split(",")]
    if args.policy == "redis-lru":
        run_redis_lru(args, fractions)
        return

    expected = read_expected(args.expect, args.policy) if args.expect else None
    mismatches = checked = 0
    print("| Trace | Capacity | Keys | Misses (reference) | Misses (engine) |")
    print("|---|---:|---:|---:|---:|")
    for name, (ids, _) in TRACES.items():
        distinct = max(ids) + 1
        for fraction in fractions:
            # The same arithmetic as eviction_sim: truncate, at least 1
            capacity = max(1, int(fraction * distinct))
            misses = POLICIES[args.policy](ids, capacity)
            engine = expected.get((name, fraction)) if expected is not None else None
            if engine is not None:
                checked += 1
                mismatches += engine != misses
            mark = "" if engine is None or engine == misses else "  <-- differs"
            print(f"| {name} | {fraction:g} | {capacity} | {misses} | "
                  f"{'-' if engine is None else engine}{mark} |", flush=True)
    if expected is not None:
        if checked == 0:
            sys.exit("no row of the report matched a trace and capacity")
        if mismatches:
            sys.exit(f"{mismatches} of {checked} counts differ")
        print(f"\nAll {checked} counts match.")


if __name__ == "__main__":
    main()
