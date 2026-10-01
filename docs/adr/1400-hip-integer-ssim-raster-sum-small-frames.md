<!-- markdownlint-disable MD013 MD060 -->
# ADR-1400: `integer_ssim_hip` sums small frames in the CPU's raster order

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: hip, gpu-parity, numerics, feature-extractor, fork-local

## Context

The CPU `ssim` extractor (`integer_ssim.c::calc_ssim()`) adds one term per pixel into a running double in raster order and divides by the weight total. `integer_ssim_hip` forms each term with the CPU's own double expression but adds the terms in a tree per 16x8 block and adds the blocks on the host, so only the order of the sum differs (2.3e-14 on the Netflix 576x324 pair).

The order matters on identical frames. The CPU's quotient `((w * f) * g) / (f * g)` on an identical window is the weight `w` up to an ulp, and whether the running sum absorbs that ulp depends on the frame. [ADR-1382](1382-hip-twin-cpu-option-parity.md) made the twin return `w` itself for an identical window, so every identical frame scores exactly 1 (`+inf` with `enable_db`), on the reading that the CPU does the same from 3x3 up. `T-HIP-INTEGER-SSIM-TINY-IDENTICAL-DB-2026-09-30` recorded the two sizes where that reading was known to fail: an identical 1x1 frame reports 156.54 dB on the CPU and an identical 2x2 frame 159.55 dB.

A replay of `calc_ssim()` over random identical frames ([Research-1400](../research/1400-hip-integer-ssim-raster-sum.md)) shows the CPU is not exactly 1 on larger frames either: a flat 3x3 frame of 51 reports 159.55 dB, and frames up to 497x2 (994 pixels) at 8 bits and 343x5 (1715 pixels) at 12 bits missed 1. The rate falls with the pixel count, and none of 800000 identical frames of 4097 to 16384 pixels missed. A fixed minimum size therefore does not separate the frames where the twin agrees with the CPU from those where it does not; reproducing the CPU's sum does.

## Decision

For frames of at most `ISSIM_HIP_RASTER_MAX_PIXELS` = 4096 luma pixels (64x64), pass 2 of `integer_ssim_hip` runs `integer_ssim_vert_terms`, which writes each pixel's CPU term (`issim_cpu_term()`, without the identical-window rule) and weight unreduced, and `collect()` adds them in index order, which is the CPU's raster order. The score is then the CPU's double for every frame of that size, identical or not, at every bit depth, and `enable_db` reports the CPU's value. Larger frames keep the per-block reduction on the device and the ADR-1382 identical-window rule.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-pixel terms read back, host adds them in raster order, up to 4096 pixels (chosen) | Bit-exact against the CPU on every small frame, not only identical ones; same cost as the tree (0.07 ms a frame at 64x64 on a gfx1036); still one read-back per frame | The read-back grows from one pair per block to one pair per pixel, 64 KiB at the bound; two pass-2 kernels | — |
| One device thread adds the terms in raster order (the row's suggestion) | No larger read-back | Measured on the gfx1036: 9.0 ms a frame at 64x64 and 0.68 ms at 16x16 against 0.07 / 0.03 ms for the tree, 2.2 µs a pixel | Over 100 times slower for the same result |
| Declare a 3x3 minimum with an ADR-1324 CPU fallback (the row's other suggestion) | No kernel change | The CPU misses 1 above 3x3 too (3x3 flat 51, 10x5, 497x2), so the fallback boundary would be wrong wherever it is put; a direct request for the twin on a tiny frame would fail | Does not fix the defect it bounds |
| Raster order at every size | Bit-exact everywhere | A 4K frame would read back 133 MB per frame, or run 8.3 M sequential double adds on one device thread | Gives up the device reduction for frames where the sum order moves the score by 1e-14 |
| Leave it: frames that small carry no SSIM information | Nothing to do | The twin disagrees with its reference by an infinite amount in dB on inputs the CLI accepts | A correct small-frame path costs nothing at run time |

## Consequences

- **Positive**: `integer_ssim_hip` equals the CPU `ssim` bit for bit on every frame of at most 4096 pixels, with and without `enable_db` / `clip_db`. On a gfx1036: 14 geometries from 1x1 to 64x64 at 8, 10, 12 and 16 bits, four frames each, all identical to the CPU; 400 frames of 64x64 noise identical. Larger frames are unchanged (Netflix 576x324 2.3e-14, 1080p checkerboards 1.6e-12 / 1.1e-11, BBB 3840x2160 5.6e-13, as before).
- **Negative**: above 4096 pixels an identical frame on which the CPU's sum keeps its ulp still reads `+inf` on the twin. The replay found none in 200000 identical frames of 4097 to 16384 pixels at each of 8, 10, 12 and 16 bits, so this is a bound on what is known, not an observed difference.
- **Neutral / follow-ups**: the CUDA twin documents the same order difference (`ssim_cuda.c`, Research-1372) and can take the same path. Guarded by `test_hip_ssim_tiny_frames` (device, `==` with `enable_db`; fails on the previous code with `1x1 flat 0: cpu=156.53559774527022 dB hip=inf dB`) and four planted regressions in `test_hip_kernel_source_contract.py` (device-free).

## References

- req: RC3 HIP lane brief (2026-10-01): "T-HIP-INTEGER-SSIM-TINY-IDENTICAL-DB-2026-09-30: identical 1x1 and 2x2 frames with enable_db give +inf where the CPU gives 156.54 / 159.55 dB."
- [ADR-1382](1382-hip-twin-cpu-option-parity.md) — the identical-window rule this narrows to frames above the bound.
- [ADR-0564](0564-integer-ssim-gpu-real-kernels.md) — the int64-moment kernels.
- [Research-1400](../research/1400-hip-integer-ssim-raster-sum.md) — the replay and the device measurements.
