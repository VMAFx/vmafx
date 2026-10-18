<!-- markdownlint-disable MD013 MD060 -->
# Research-1430: Which `speed_chroma_cuda` outputs differ from a glibc CPU, and that `log2f` is the only reason

- **Status**: Active
- **Workstream**: [ADR-1430](../adr/1430-cuda-speed-chroma-log2f-bound.md), [ADR-1380](../adr/1380-cuda-speed-device-resident-pipeline.md), [ADR-1426](../adr/1426-cuda-ciede-cpu-arithmetic.md)
- **Last updated**: 2026-10-01

## Question

`speed_chroma_cuda` is documented as bit-identical to the CPU except where
glibc misrounds `log2f`. Which outputs and frames are those on this lane's
fixtures, is `log2f` really the only cause, and what bound should the parity
gate use for the cell?

## Sources

- CPU: `core/src/feature/speed.c` (the `log2f` calls of the entropy and
  scoring steps).
- CUDA: `core/src/feature/cuda/speed/speed_score.cu::speed_log2()`
  (correctly rounded, `feature/speed_log2_hard_cases.h`), unchanged here.
- Reference workstation: RTX 4090, CUDA 13.4, gcc 16.2.1, glibc 2.44. `meson setup
  build-cuda core -Denable_cuda=true -Denable_sycl=false --buildtype=release
  -Db_lto=false`, master `c2af74494`.
- Fixtures: the Netflix 576x324 pair at 8 bits (48 frames) and at 10, 12 and
  16 bits (3 frames each), both 1080p checkerboard pairs (3 frames each), BBB
  3840x2160 (200 frames). Three outputs per frame: 789 values.

## Method

Three runs per fixture at `--precision max` with one binary:

1. `--backend cpu --feature speed_chroma`;
2. the same command with a correctly rounded `log2f` preloaded;
3. `--backend cuda --feature speed_chroma_cuda`.

The preload is two lines:

```sh
printf '#include <math.h>\nfloat log2f(float x) { return (float)log2((double)x); }\n' > crlog2f.c
cc -O2 -fPIC -shared crlog2f.c -o crlog2f.so -lm
LD_PRELOAD=$PWD/crlog2f.so build-cuda/tools/vmaf --backend cpu --feature speed_chroma ...
```

The fp64 `log2` is accurate to well under one unit in its last place, 29
bits below a float's, so its rounding to float is the correctly rounded
`log2f` except for arguments whose logarithm lies within about 2^-29 of a
float rounding boundary. It replaces every `log2f` call of the process and
nothing else.

## Findings

1. **The twin equals the CPU with a correctly rounded `log2f` on all 789
   values.** Run 3 against run 2: 789 of 789 identical.

2. **Against glibc's `log2f`, 13 of 789 values differ.** Run 3 against run 1,
   every difference:

   | Fixture | Frame | Output | Difference | CPU score |
   |---|---:|---|---:|---:|
   | Netflix 576x324, 8 bit | 3 | `speed_chroma_v` | 1.192e-6 | 3.0815 |
   | Netflix 576x324, 8 bit | 3 | `speed_chroma_uv` | 9.537e-7 | 9.2359 |
   | BBB 3840x2160 | 17 | `speed_chroma_u` | 9.537e-7 | 4.6404 |
   | BBB 3840x2160 | 17 | `speed_chroma_uv` | 9.537e-7 | 5.7867 |
   | BBB 3840x2160 | 21 | `speed_chroma_v` | 1.431e-6 | 6.9267 |
   | BBB 3840x2160 | 21 | `speed_chroma_uv` | 4.768e-7 | 5.7934 |
   | BBB 3840x2160 | 103 | `speed_chroma_v` | 9.537e-7 | 10.6324 |
   | BBB 3840x2160 | 138 | `speed_chroma_v` | 9.537e-7 | 10.0488 |
   | BBB 3840x2160 | 138 | `speed_chroma_uv` | 9.537e-7 | 8.6938 |
   | BBB 3840x2160 | 150 | `speed_chroma_u` | 9.537e-7 | 7.1248 |
   | BBB 3840x2160 | 150 | `speed_chroma_uv` | 9.537e-7 | 8.9033 |
   | BBB 3840x2160 | 191 | `speed_chroma_v` | 9.537e-7 | 10.3509 |
   | BBB 3840x2160 | 191 | `speed_chroma_uv` | 9.537e-7 | 8.4151 |

   The 10-, 12- and 16-bit Netflix frames and both checkerboard pairs are
   identical. The scores are fp32 values, so a difference is a whole number
   of float steps: five at 3.08 (step 2.4e-7), one to three between 4 and 8
   (step 4.8e-7), one between 8 and 16 (step 9.5e-7).

3. **The CPU moves on the same 13 values by the same amounts.** Run 2 against
   run 1 differs on exactly the rows above and nowhere else. The whole
   difference between twin and CPU is therefore on the CPU's side of the
   `log2f` call.

4. **How often glibc 2.44's `log2f` is not the nearest float.** Every float
   of four binades against `(float)log2((double)x)`:

   | Binade | Arguments that differ | Share |
   |---|---:|---:|
   | [1, 2) | 81 390 of 8 388 608 | 0.97 % |
   | [2, 4) | 10 277 | 0.12 % |
   | [512, 1024) | 1 271 | 0.015 % |
   | [2^-10, 2^-9) | 1 271 | 0.015 % |

   Never by more than one float step. `speed_chroma` calls `log2f` 626 times
   per 576x324 frame and 32 430 times per 3840x2160 frame (counted with a
   preloaded `log2f`), so a 4K frame has tens of such results. They reach the
   score only when the float sums that follow do not round the difference
   away, which is why about one output in sixty moves.

5. **The gate cell.** `scripts/ci/cross_backend_parity_gate.py --features
   speed_chroma --backends cpu cuda` reports `max_abs_diff=1.431e-06` on the
   200 BBB frames and `1.192e-06` on the Netflix pair, against
   `tol=5.0e-06 (libm:ADR-1426)`.

6. **The parity test's fixture never reached the scoring path.**
   `test_cuda_speed_chroma_parity` used a 768x432 ramp: 8 blocks for a 25x25
   covariance, singular on every frame (the geometry note in
   `test_cuda_speed_singular_parity.c` says so). On a 960x960 hash texture (36
   blocks, regular) the scores are about 22 and one frame differs by 3.815e-6
   on `speed_chroma_v` and `speed_chroma_uv`: two float steps at that
   magnitude, 1.7e-7 of the score. With the correctly rounded `log2f`
   preloaded all six values are identical. Relative to the score the largest
   difference on any fixture is 3.9e-7 (Netflix frame 3, `speed_chroma_v`).

7. **Timing.** No source of the twin changes. For the record, the twin alone
   takes about 1.7 ms per 3840x2160 frame here (three runs of
   `(t(52) - t(2)) / 50`: 0.8, 1.7 and 2.3 ms at a host load average of 14)
   and the CPU extractor 6.6 ms on sixteen threads.

## Open questions

- `speed_temporal_cuda` is identical on the same fixtures with BBB cut to 50
  frames (113 of 113 values against glibc); it has no gate cell, and whether
  it stays identical on other content has not been measured.
- The same measurement on a host with another glibc gives another set of
  outputs (see the 2.43 numbers in
  [SpEED](../metrics/speed_qa.md#the-cpu-reference-and-log2f)); the bound was
  chosen from one host.
