# Does the RESP dictionary help the fuzzer?

`tests/fuzz/resp.dict` gives libFuzzer RESP's tokens and every command and
subcommand name, so its mutations can build commands the server knows. This
measures what that buys: the coverage libFuzzer reaches with and without it.

## Method

- The libFuzzer build CI uses: clang 18.1.3, `-DKOSH_LIBFUZZER=ON -DSANITIZE=address,undefined`
  (Ubuntu 24.04 in Docker, the image CI's runner uses).
- Each run starts from the same 27 seed inputs (`tests/fuzz/corpus`) and an
  empty output corpus, and stops after **50,000 inputs**, not after a fixed
  time: a count gives the same work on a busy or a quiet machine, so no quiet
  machine is needed.
- Five seeds; for each, one run without the dictionary and one with it:

  ```bash
  fuzz/fuzz_commands -seed=$S -runs=50000 -timeout=10 out tests/fuzz/corpus
  fuzz/fuzz_commands -seed=$S -runs=50000 -timeout=10 -dict=tests/fuzz/resp.dict out tests/fuzz/corpus
  ```

- The numbers are from libFuzzer's last line (`#50000 DONE cov: … ft: …`):
  `cov` counts the code edges reached, `ft` the finer "features" (edges with
  how often each was taken) that libFuzzer uses to decide what to keep.

## Results (2026-11-08)

| seed | cov, no dictionary | cov, dictionary | ft, no dictionary | ft, dictionary |
|---:|---:|---:|---:|---:|
| 1 | 1806 | 1839 | 6250 | 6468 |
| 2 | 1807 | 1817 | 6140 | 6264 |
| 3 | 1800 | 1802 | 6288 | 6416 |
| 4 | 1806 | 1819 | 6506 | 6291 |
| 5 | 1789 | 1796 | 6535 | 6255 |

- **Edges:** more with the dictionary for every seed, by 2 to 33 (0.1–1.8%);
  median 1,817 against 1,806.
- **Features:** mixed: more with the dictionary for seeds 1–3, fewer for 4 and 5.

## Reading

The dictionary helps, but little. The seed corpus already contains all 43
command names (checked with `grep -iw`), so the fuzzer never has to discover
a name, which is what a dictionary is mostly for. That this is why the gain
is small is likely, not tested. The
dictionary stays in CI: it costs nothing and never reached fewer edges here.
Untested: a longer run, where the gap could grow or vanish, and a run from
an empty corpus, where a dictionary usually matters most.
