# Hash flooding: `std::hash` against the keyed SipHash

[`bench/hash_flood.cpp`](../../hash_flood.cpp) at commit `8ca785d` (the commit that
made `KeyTable` hash with SipHash-1-3 under a random key), output in
[`hash_flood.md`](hash_flood.md).

- **Run:** 2026-11-08, `taskset -c 2 ./hash_flood`, GCC 13, RelWithDebInfo (-O2);
  Intel Core Ultra 5 125H, WSL2 (Linux 6.6). The machine was idle (1–3% CPU on Windows).
- **Repeats:** each figure is the median of 5 rounds.

## The attack

Keys that collide under `std::hash` are easy to find: its seed is fixed, so
the attacker computes them offline. Here, 40,000 keys sharing the low 15 bits
of their hash took **31.6 s on one core**, from 1.31 billion candidates.

Inserting them (after a lookup each, as `SET` does):

| Keys | Colliding, `std::hash` | Colliding, SipHash | Ordinary, `std::hash` | Ordinary, SipHash |
|---:|---:|---:|---:|---:|
| 10,000 | 137.8 ms | 1.22 ms | 0.97 ms | 1.19 ms |
| 40,000 | 2,019 ms | 5.46 ms | 3.96 ms | 5.16 ms |

With `std::hash`, doubling the keys multiplies the time by 3.2–5.3: the
O(n²) of every insert walking one chain. At 40,000 keys the colliding keys
take **370×** as long as with SipHash, which doesn't care which keys it gets.

## The cost for ordinary keys

- One hash of a 16-byte key: `std::hash` 3.43 ns, SipHash-1-3 11.22 ns
  (SipHash-2-4, the original parameters: 19.25 ns).
- Per lookup of random keys, SipHash against `std::hash`:
  - one at a time (each waits for the last): 1.11× at 1,000 keys, 1.15× at
    100,000, 1.10× at 1,000,000;
  - independent (the CPU overlaps them): 1.18×, 1.49×, 1.92×. Hashing is
    a larger share when memory latency is hidden.

What that costs a whole request is measured separately, end to end:
[`../phase-4c/`](../phase-4c/README.md).

## Caveat

Absolute times are about 1.6× those recorded with commit `8ca785d`
(6.7 and 2.1 ns per hash): this laptop's single-thread speed varies from
session to session. The ratios agree with that run (3.2× per hash then, 3.3×
now; 375–420× for the attack then, 370× now); compare ratios, not times.
