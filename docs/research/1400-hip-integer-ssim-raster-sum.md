<!-- markdownlint-disable MD013 MD060 -->
# Research-1400: integer SSIM on identical small frames — where the CPU's sum is not exactly 1

- **Status**: Active
- **Workstream**: RC3 HIP lane, `T-HIP-INTEGER-SSIM-TINY-IDENTICAL-DB-2026-09-30`; decision in [ADR-1400](../adr/1400-hip-integer-ssim-raster-sum-small-frames.md)
- **Last updated**: 2026-10-01

## Question

`integer_ssim_hip` scores every identical frame exactly 1 ([ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md)); the CPU `ssim` reports 156.54 dB and 159.55 dB with `enable_db` on identical 1x1 and 2x2 frames. On which frames does the CPU's sum stay below (or above) 1, and what is the cheapest way for the twin to report the same value?

## Sources

- CPU reference: `core/src/feature/integer_ssim.c` (`calc_ssim()`, `ssim_reduce_row_range()`).
- HIP twin: `core/src/feature/hip/integer_ssim_hip.c`, `core/src/feature/hip/integer_ssim/integer_ssim_score.hip`.
- [Research-1377](1377-hip-rc3-cpu-parity.md), which introduced the identical-window rule and reported the 1x1 / 2x2 difference from a host replay.
- Host: `ryzen-4090-arc`, AMD gfx1036 iGPU, ROCm 7.2.4, Linux 7.2.8; `meson setup build-hip core -Denable_hip=true -Denable_hipcc=true -Dhip_gfx_targets=gfx1036 -Denable_cuda=false -Denable_sycl=false --buildtype=release -Db_lto=false`.

## Findings

### The CPU is not exactly 1 on every identical frame above 2x2

A standalone replay of `calc_ssim()` (the horizontal and vertical int64 moment loops and the double expression, `gcc -O2 -ffp-contract=off`) scored identical frames of four kinds: full-range noise, mid-range plus two bits of noise, a ramp with one bit of noise, and a random mix of 0 and the peak. An identical window's term is `((w * f) * g) / (f * g)`, which is `w` up to an ulp; the frame score is exactly 1 only when the running sum absorbs every such ulp.

Flat 8-bit frames, all 256 values: 59 values on 1x1, 119 on 2x2 and 14 on 3x3 score other than 1. A 1x1 frame of 0 scores `1 - 2^-52` (156.535598 dB), a 2x2 frame of 2 and a 3x3 frame of 51 score `1 - 2^-53` (159.545898 dB), and a 1x1 frame of 3 scores `1 + 2^-52`, which the shared emitter reports as `+inf` (`raw_score >= 1.0`).

Thin strips and near-square frames up to 2048 pixels, 400 frames per geometry:

| Bit depth | Largest identical frame that missed 1 | Misses, 1 to 255 px | 256 to 2047 px |
|---|---|---|---|
| 8 | 497x2 (994 px) | 342 of 540400 | 2 of 749200 |
| 10 | 122x2 (244 px) | 181 of 540400 | 0 of 749200 |
| 12 | 343x5 (1715 px) | 605 of 540400 | 13 of 749200 |
| 16 | 1x945 (945 px) | 897 of 540400 | 2 of 749200 |

Random geometries (strips with one side up to 8, and frames with both sides 9 to 408):

| Pixels | Frames per bit depth | 8-bit | 10-bit | 12-bit | 16-bit |
|---|---|---|---|---|---|
| 257 to 4096 | 400000 | 0 | 0 | 18 (largest 791x5) | 1 (6x142) |
| 4097 to 16384 | 200000 | 0 | 0 | 0 | 0 |

So "both report `+inf` from 3x3 up" (Research-1377) holds for the frames that replay happened to try, not in general, and no fixed minimum frame size separates agreement from disagreement. Above 4096 pixels no miss was found.

### The device run reproduces the difference

On the gfx1036, with the kernel of `origin/master` c66d28b2f, `test_hip_ssim_tiny_frames` fails at its first identical frame: `1x1 flat 0: cpu=156.53559774527022 dB hip=inf dB`.

### A sequential sum on one device thread is too slow; per-pixel terms summed on the host are free

Two forms of the CPU-order sum were built and timed on the gfx1036 (`--feature integer_ssim_hip`, synthetic 8-bit clips, `(t(400 frames) - t(40 frames)) / 360`, three repetitions, load average 6 to 8):

| Pass 2 | 64x64 | 16x16 |
|---|---|---|
| Per-block tree (`origin/master`) | 0.072 / 0.076 / 0.078 ms | 0.039 / 0.033 / 0.032 ms |
| One device thread walks the frame and adds the terms | 9.04 / 9.03 / 9.00 ms | 0.684 / 0.683 / 0.661 ms |
| One term per pixel, read back, host adds in index order (chosen) | 0.070 / 0.081 / 0.074 ms | 0.032 / 0.035 / 0.038 ms |

The single device thread spends 2.2 µs a pixel. The per-pixel form costs what the tree costs and its read-back is 16 bytes a pixel, 64 KiB at 4096 pixels.

### Parity after the change

- `test_hip_ssim_tiny_frames` on the gfx1036: 1x1, 2x2, 1x2, 2x1, 3x3, 4x4, 10x5, 2x37, 16x8, 17x9, 1x97, 131x1, 33x31 and 64x64 at 8, 10, 12 and 16 bits; four frames each (identical noise, identical flat, identical low-variance, distorted); every `ssim` value with `enable_db` equals the CPU's. 65x64 and 129x33 (per-block path) at the four bit depths: identical frames equal, the distorted frame within 1e-12.
- 400 frames of 64x64 noise against `--backend cpu --feature ssim` at `--precision max`: 400 of 400 identical.
- Frames above the bound, `--feature ssim` (CPU, 16 threads) against `--feature integer_ssim_hip` at `--precision max`:

| Fixture | Frames | Bit-identical | Max abs diff |
|---|---|---|---|
| Netflix `src01_hrc00/01_576x324` | 48 | 0 | 2.32e-14 |
| `checkerboard_1920_1080_10_3_0_0` vs `_1_0` | 3 | 0 | 1.58e-12 |
| `checkerboard_1920_1080_10_3_0_0` vs `_10_0` | 3 | 0 | 1.06e-11 |
| BBB 3840x2160 | 50 | 0 | 5.58e-13 |

These are the per-block path's summation-order differences, the same values the HIP overview recorded before the change.

## Alternatives explored

See the decision matrix in [ADR-1400](../adr/1400-hip-integer-ssim-raster-sum-small-frames.md). The bound of 4096 pixels is where the replay stops finding misses; 16384 would quadruple the largest read-back to 256 KiB for no observed gain.

## Open questions

- The CUDA twin (`ssim_cuda.c`) sums in the same per-block order without an identical-window rule and documents a difference on identical frames with a side below 12 pixels (Research-1372). The same per-pixel path would close it; not changed here.
- Above 4096 pixels the claim is statistical: no miss in 800000 identical frames of 4097 to 16384 pixels.

## Related

- [ADR-1400](../adr/1400-hip-integer-ssim-raster-sum-small-frames.md), [ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md), [ADR-0564](../adr/0564-integer-ssim-gpu-real-kernels.md), [ADR-1221](../adr/1221-gpu-ms-ssim-db-ceiling.md).
