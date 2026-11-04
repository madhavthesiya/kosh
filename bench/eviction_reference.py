#!/usr/bin/env python3
"""Textbook eviction policies, to check the engine's against: the same requests
must give exactly the same number of misses.

eviction_sim --dump-trace writes the requests it ran: a "# trace NAME" line,
then one key id per line (ids are 0 .. distinct-1). This replays them through
a plain implementation of each policy, with the same read-through rule (a miss
inserts the key) and the same capacities, and prints the misses. With
--expect, it reads eviction_sim's report and fails if any count differs.

Usage:
  ./build-bench/eviction_sim --dump-trace trace.txt zipf:0.99 > sim.md
  python3 bench/eviction_reference.py trace.txt --expect sim.md
"""

import argparse
import re
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
    print("self-test passed")


def main():
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        return
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("trace", help="file written by eviction_sim --dump-trace, or --self-test")
    p.add_argument("--capacities", default="0.001,0.01,0.1")
    p.add_argument("--policy", default="lru", choices=sorted(POLICIES))
    p.add_argument("--expect", help="eviction_sim's report, to compare the misses with")
    args = p.parse_args()

    fractions = [float(c) for c in args.capacities.split(",")]
    expected = read_expected(args.expect, args.policy) if args.expect else None
    mismatches = checked = 0
    print("| Trace | Capacity | Keys | Misses (reference) | Misses (engine) |")
    print("|---|---:|---:|---:|---:|")
    for name, ids in read_traces(args.trace):
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
