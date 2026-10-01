<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1424: `integer_ssim_cuda` adds its terms in the CPU's raster order, on the host, and returns the CPU's score bit for bit

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `cuda`, `gpu-parity`, `numerics`, `ssim`, `testing`, `ci`, `rc3`, `fork-local`

## Context

`integer_ssim_cuda` (`core/src/feature/cuda/ssim_cuda.c`) is the CUDA twin of
the fixed-point `ssim` extractor (`integer_ssim.c`). Its moments are int64
and equal the CPU's, and since
[ADR-1373](1373-cuda-twin-cpu-option-parity.md) its per-pixel term is the
CPU's double expression operand for operand. One thing was left:
`calc_ssim()` adds every term into one `double`, left to right and top to
bottom, and the kernel added them per warp, per 16x8 block and then on the
host. The score matched the CPU on no measured frame and was up to 1.1e-11
from it (3.6e-10 with `enable_db`): Netflix 576x324 2.3e-14, 1 px
checkerboard 1.6e-12, 10 px checkerboard 1.1e-11, BBB 3840x2160 5.6e-13.

A sum of doubles is its order. There is no reordering of 8.3 million adds
that rounds like the sequential one, so the twin either adds in the CPU's
order or stays an approximation.
[ADR-1400](1400-hip-integer-ssim-raster-sum-small-frames.md) made that
choice for the HIP twin for frames of at most 4096 pixels, where the order
decides between a finite dB value and `+inf` on identical frames, and kept
the device reduction above that size because a full-frame read-back is not
free. The maintainer's direction for the CUDA twins in this lane is results
first, bit for bit, speed afterwards.

## Decision

We will make `integer_ssim_cuda` return the CPU's score bit for bit at every
frame size: `integer_ssim_vert_combine` stores each pixel's term at its
raster position instead of reducing it, the whole plane is read back, and
`ssim_cuda.c::issim_frame_sum()` adds it in index order, which is
`calc_ssim()`'s order. The weights are integers; their sum does not depend
on the order and stays a per-block device reduction.

The gate gets the feature it lacked: `ssim` joins `FEATURE_METRICS` in
`scripts/ci/cross_backend_parity_gate.py`, with the twins' registered names
(`integer_ssim_cuda`, `integer_ssim_sycl`, `integer_ssim_hip`) in
`BACKEND_EXTRACTOR_ALIASES`, and `ssim`: `cuda` joins `EXACT_TWINS`: the
CPU ↔ CUDA cell is compared with tolerance 0 at `--precision max`. The other
`ssim` twins get the places=4 default.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the block reduction; tighten the tolerance to 1.1e-11 | No read-back, no cost | Not the CPU's score; in dB the difference is 30 times larger, and on small identical frames it is the difference between a finite value and `+inf` | The direction is bit for bit |
| Raster order up to 4096 pixels only, as ADR-1400 does for HIP | Exact where the order changes the reported class of value; free | Every larger frame still differs in its last digits | Leaves the twin a tolerance at the sizes people measure |
| One device thread adds the plane in raster order | No read-back | 8.3 million dependent double adds on one device thread; ADR-1400 measured 2.2 µs a pixel on a gfx1036 | Slower than the host by orders of magnitude |
| Per-row device sums, rows added on the host | Small read-back, parallel | `calc_ssim()` has no per-row accumulator: one double runs through the whole frame, so a row's contribution depends on the sum before it | Not the CPU's order |
| Integer accumulation per binade on the device: while the running sum stays in one binade, adding a term is adding its value rounded to that binade's unit, which is an integer sum and can be split across threads | Exact and parallel; read-back of a few values per row | Needs the sum at each row's start to pick the unit, both parities for ties, prefix extrema to certify the binade, and a sequential fallback for the rows that cross one | The tuning candidate, `T-CUDA-SSIM-EXACT-THROUGHPUT-2026-10-01`; this ADR takes the simple exact form first |

## Consequences

- **Positive**: measured on an RTX 4090 at `--precision max` against master
  `5c8b9e9c7`, the score of every frame equals `--backend cpu`: Netflix
  576x324 at 8 bits (48 frames) and at 10, 12 and 16 bits (3 frames each),
  both 1920x1080 checkerboard pairs (3 frames each) and BBB 3840x2160 (50
  frames), 113 of 113; before, 0 of 113. Also with `enable_db` and with
  `enable_db` plus `clip_db` (107 of 107 each), and on frames from 1x1 to
  322x182 with every window truncated. The gate reports 0 on all 200 BBB
  frames and on the Netflix pair.
- **Positive**: a small identical frame now reports the CPU's value, finite
  or `+inf`, at every size; the caveat `docs/metrics/ssim.md` carried for
  frames with a side below 12 pixels is gone.
- **Negative**: a run of the twin alone takes 9.72 ms per 3840x2160 frame
  instead of 2.18 ms (seven alternating pairs of 50 frames, paired
  difference +7.61 ms, quartiles +7.08 to +8.99, host load average 20). The
  frame's 8.3 million terms are read back (66 MB) and added one after the
  other on the host; the adds alone are 3.2 ms. At 576x324 the difference is
  inside the noise (0.02 and 0.13 ms, paired +0.02). `T-CUDA-SSIM-EXACT-THROUGHPUT-2026-10-01`.
- **Negative**: 66 MB more device memory and 66 MB of pinned host memory at
  3840x2160 (one double per pixel), next to the six int64 moment planes
  (398 MB) the twin already holds.
- **Negative**: stored `integer_ssim_cuda` outputs change by up to 1.1e-11.
- **Neutral / follow-ups**:
  - `integer_ssim_sycl`, `integer_ssim_hip` (above 4096 pixels) and
    `integer_ssim_metal` still reduce per block:
    `T-GPU-SSIM-FRAME-SUM-ORDER-2026-10-01`.
  - The contract mirrors `calc_ssim()` and `ssim_reduce_row_range()`. A
    change to the term or to the order of the sum changes the kernel and
    `issim_frame_sum()` in the same PR. `test_cuda_ssim_parity` asserts
    equality on a device and `test_cuda_ssim_exact_contract.py` pins the
    design without one.

## References

- `req` (maintainer brief, 2026-10-01): "results before speed; a twin
  reproduces the CPU bit for bit, and tuning comes afterwards."
- [ADR-1400](1400-hip-integer-ssim-raster-sum-small-frames.md),
  [ADR-1373](1373-cuda-twin-cpu-option-parity.md),
  [ADR-1403](1403-cuda-strict-fp-every-kernel.md),
  [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md),
  [ADR-0564](0564-integer-ssim-gpu-real-kernels.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- [Research-1424](../research/1424-cuda-ssim-cpu-frame-sum.md).
