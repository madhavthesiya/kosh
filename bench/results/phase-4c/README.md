# Phase 4C: did it cost speed? And what does SipHash cost a request?

One paired session per key count, four versions measured round by round
([`compare_commits.py`](../../compare_commits.py), 5 rounds, the order
rotating and reversing so no version always goes first):

| Label | Commit | What it adds over the previous |
|---|---|---|
| v2.5 | `b5a65a9` | Phase 4B, tag `v2.5-memory` |
| pre-siphash | `b997903` | C1–C3b: rename, bind 127.0.0.1, protected mode, passwords |
| siphash | `8ca785d` | C3c: SipHash-1-3 for `KeyTable` |
| tip | `da7362e` | C3d–C7: fuzzing, RESP3, INFO counters, transactions, new commands, CI |

- [`keys-100k/commits.md`](keys-100k/commits.md): 100,000 random keys;
  GET and SET; pipeline 1 and 16; AOF on and off.
- [`keys-1m/commits.md`](keys-1m/commits.md): 1,000,000 random keys; AOF off.
- 2026-11-07/08, 96 minutes in all, laptop idle and on mains power; machine
  details at the top of each report. Raw runs in each `commits-runs.csv`.

## Results

**Phase 4C as a whole: no regression the session could detect.** Tip
against v2.5, 12 tests: no confidence interval lies below 1.00. The one that
leaves out 1.00 is in Kosh's favour (GET, P=1, AOF on, 100k keys: 1.07×, CI
1.01–1.12). The intervals are wide (often ±10–20%), so this rules out a
large slowdown, not a small one.

**SipHash (siphash / pre-siphash): one resolved slowdown, far larger than
the hash itself can cost.**

- SET, P=1, AOF on, 100k keys: **0.93×**, slower in all 5 rounds, CI 0.88–0.99.
  The only resolved change.
- Server CPU per SET, 100k keys: 21.70 → 23.87 µs (P=1, AOF on),
  2.78 → 3.02 µs (P=16, AOF on), 2.35 → 2.53 µs (P=16, AOF off): +8–10%.
  GET's moves 0–4%.
- At 1M keys, no change is resolved either way.

How much of that can SipHash be? It costs 7.8 ns more than `std::hash` per
16-byte key ([`../hash-flooding/`](../hash-flooding/README.md)). A `SET` of
a key that exists hashes it once (the lookup); a new key's `SET` hashes it
again to insert it, and each time the table grows, the keys it moves are
hashed again. Here (1,000,000 SETs over 100,000 keys, so about 90% overwrite
a key) that comes to about 1.1–1.3 hashes per SET: about 10 ns. The measured
increase is 180–240 ns per SET at P=16 and 2,000 ns at P=1, so the hash
itself is at most about 6% of it. The rest is something this session can't
separate: noise, or a side effect of the change such as code layout
(`bench/layout_bias.py` measures how much layout alone moves results here).
**So this is not claimed as SipHash's cost**, and hashing once per `SET`
(an idea from before this measurement) would save about one hash per new
key: not worth doing for speed.

## Not explained

At 100k keys, pre-siphash was faster than v2.5 in all 5 rounds for 4 of the
8 tests (1.04–1.14×). Its commits (rename, `--bind`, protected mode,
passwords) shouldn't make requests faster, and at 1M keys the same pair
shows nothing (0.94–1.01×). Recorded as observed; not claimed.
