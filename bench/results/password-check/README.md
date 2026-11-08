# Password check timing (Phase 4C, C3b)

Measured on 2026-11-04 with [`bench/auth_compare.cpp`](../../auth_compare.cpp), built at commit `fbed411` (RelWithDebInfo, GCC 13.3 `-O2`), on the same machine as the earlier results: Intel Core Ultra 5 125H laptop, Ubuntu 24.04 under WSL2 (kernel 6.6.87.2), pinned with `taskset -c 2`.

**The question:** does checking an `AUTH` password take longer when more of the guess is right? If so, a client can find the password a byte at a time: try every value of the first byte, keep the slowest, and go on to the next.

**Result:**
- **`constantTimeEquals`, which `AUTH` uses, took 176–183 ns for every guess, at both lengths: at most 1.03× between the slowest and the fastest guess.** In four earlier runs at 512 bytes, the spread was 1.03–1.06×, and the slowest guess changed from run to run: noise, not a pattern.
- **A byte loop that stops at the first difference leaks every byte.** For guesses of the password's length, it took 1.6 ns when the first byte was wrong, against 9.5 ns for the right one at 32 bytes (6.1×) and 124 ns at 512 (79×): about 0.25 ns per right byte.
- **`std::string`'s `==` (memcmp) leaks in coarse steps.** It compares many bytes at a time, so among guesses of the right length its time changes only 1.17× at 32 bytes and 2.0× at 512. It shows roughly where the first wrong block is, not each byte, but that is still a leak.
- **Cost:** about 180 ns per `AUTH`, once per connection.

```bash
cmake --build build --target auth_compare
taskset -c 2 ./build/auth_compare --length 32
taskset -c 2 ./build/auth_compare --length 512 --calls 50000
```

Each cell is the median of 15 rounds. The rounds interleave every cell, in an order that rotates each round, so drift in clock speed affects all cells alike.

## A 32-byte password

Median ns per comparison, 200,000 calls per cell per round.

| Guess | std::string == | loop, stops early | constantTimeEquals |
|---|---:|---:|---:|
| wrong at byte 0 | 2.87 | 1.55 | 181.05 |
| wrong in the middle | 2.87 | 5.47 | 180.59 |
| wrong at the last byte | 2.91 | 9.00 | 183.10 |
| right | 2.48 | 9.50 | 179.85 |
| one byte short | 0.88 | 1.09 | 178.25 |
| **slowest / fastest** | **3.29x** | **8.68x** | **1.03x** |

## A 512-byte password (the longest allowed)

Median ns per comparison, 50,000 calls per cell per round.

| Guess | std::string == | loop, stops early | constantTimeEquals |
|---|---:|---:|---:|
| wrong at byte 0 | 2.93 | 1.57 | 176.87 |
| wrong in the middle | 4.72 | 66.84 | 181.05 |
| wrong at the last byte | 5.98 | 123.54 | 178.32 |
| right | 5.53 | 124.21 | 180.57 |
| one byte short | 0.91 | 1.09 | 175.71 |
| **slowest / fastest** | **6.56x** | **113.52x** | **1.03x** |

For `==` and the loop, the "one byte short" guess is fastest because they compare the lengths first. That reveals only the length of the guess, which the client chose.

## The guess's length, against the password's

`constantTimeEquals` copies both strings into zero-padded buffers, so its time could depend on their lengths. Median ns, 9 rounds:

| Password | Guess of the same length | Guess one byte shorter |
|---:|---:|---:|
| 31 bytes | 178.53 | 178.80 |
| 32 bytes | 184.59 | 179.04 |
| 33 bytes | 176.88 | 182.31 |

No pattern beyond the noise.

## A measurement mistake, caught

In the first version of the tool, each guess was a separate `std::string` on the heap. Two cells came out 10–23% slower than the rest:
- the 31-byte guess, every time;
- once, the 512-byte guess wrong at byte 0.

A difference in where each string sits in memory (alignment) would change how fast it is copied, independently of the content. So the tool now copies every guess into one 64-byte-aligned buffer before timing it. Both differences disappeared, and the table above is from that version.

Without that check, the 31-byte cell would have suggested that `constantTimeEquals` leaks the length of the guess. It doesn't, but a measurement has to rule that out before it can show it.

## Limits

- **In-process timing.** Over a network, jitter is far larger than these differences. But an attacker can average many tries to recover small differences, so the safe design is not to have them.
- **Only the comparison is timed.** The parser and the rest of `AUTH` are the same for every guess of a given length.
- **One machine, one compiler.** Another compiler could in principle turn the loop into one that stops early. The `volatile` accumulator is there to prevent that, and this tool is how to check a new build.
- **WSL2 can't choose the core type** on this hybrid CPU, which is part of the run-to-run noise. Only the ratios within a run are compared.
