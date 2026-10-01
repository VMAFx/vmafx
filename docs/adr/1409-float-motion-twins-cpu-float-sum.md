<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1409: `float_motion_cuda` adds its SAD in the CPU's order and returns the CPU's scores bit for bit

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `cuda`, `gpu-parity`, `numerics`, `float-motion`, `testing`, `ci`, `rc3`, `fork-local`

## Context

`float_motion.c` scores a frame as the mean absolute difference between its
blurred luma plane and the previous frame's. `compute_motion_simd()` adds the
absolute differences of one row, left to right, into one `float`
(`float_sad_line_c()`; the AVX2, AVX-512 and NEON versions vectorise the
subtraction and add in the same order), adds the row sums top to bottom into a
second `float`, and divides by the pixel count in `float`. Every addition
rounds, and how it rounds depends on the sum so far, so the score is a
property of that order and type, not only of the differences.

`float_motion_cuda` summed the differences of each 16x16 block on the device
and the blocks in `double` on the host. That is closer to the exact sum and it
is not the CPU's value. Measured on an RTX 4090 at `--precision max`, with the
blur already built without FMA contraction ([ADR-1403](1403-cuda-strict-fp-every-kernel.md)):
1.36e-4 on the two 1920x1080 checkerboard pairs, above the 5e-5 tolerance of
[ADR-0214](0214-gpu-parity-ci-gate.md); 3.1e-6 on the Netflix 576x324 pair;
2.4e-5 on BBB 3840x2160. The gate runs the Netflix pair only and passed. A
host replay of the checkerboard showed where the distance comes from: the
5-tap blur with separate fp32 multiplies and adds, followed by the CPU's
running sums, reproduces the CPU score bit for bit, and the fp64 sum of the
same differences is 1.2e-4 to 1.4e-4 lower. The blur matched; the reduction
did not.

[ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md) settled the same question for
`psnr_hvs`: the CPU extractor is the reference, its golden assertions are not
modified ([ADR-0024](0024-netflix-golden-preserved.md)), so a twin reproduces
the CPU's accumulation.

## Decision

We will make `float_motion_cuda` return the CPU extractor's scores bit for
bit, by adding the SAD in the CPU's order.

**Row sums on the device.** `float_motion_row_sad`
(`core/src/feature/cuda/float_motion/float_motion_score.cu`) runs one thread
per row. The thread adds `|cur[j] - prev[j]|` for `j = 0 .. w - 1` into one
fp32 accumulator and stores it; the frame's readback is `h` floats. The two
blur kernels write the blurred plane only. The kernel file contains no block,
warp or atomic reduction.

**Rows on the host.** `vmaf_float_motion_score_from_row_sads()`
(`core/src/feature/float_motion_sad.h`) adds the row sums top to bottom into
one `float` and divides by `(float)(int)(w * h)`, the expression
`compute_motion_simd()` evaluates. Every twin that adopts this contract calls
it; no twin keeps a sum of its own.

**Blur.** Unchanged in arithmetic: `convolution_f32_c_s()` tap for tap,
vertical pass then horizontal, with the fatbin built by the ADR-1403 flag
list. The exactness of the score depends on that flag.

**Gate.** `float_motion` gets a `cuda` entry in `EXACT_TWINS`
(`scripts/ci/cross_backend_calibration.py`): the CPU and CUDA cell is compared
with tolerance 0 at `--precision max`. The SYCL, HIP and Metal twins still
reduce per block and keep the places=4 tolerance.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One thread per row on the device, rows on the host (this ADR) | The CPU's bits on every frame; the readback shrinks from one float per 16x16 block to one per row; no measurable time | The row kernel does not use the device's width: `h` threads each walk `w` samples | Chosen |
| Keep the per-block sum, raise the tolerance to cover 1.4e-4 | No code change | Accepts any twin error below the CPU's own rounding error, which grows with the frame area; two backends keep reporting different scores | Hides the difference |
| Sum in `double` on the CPU | Removes the rounding at its source | Changes `motion` / `motion2` of the CPU extractor, which the Netflix golden assertions pin, and every model trained on them | Golden assertions are not modified |
| Read all differences back and add them on the host | The simplest exact form (the `psnr_hvs_cuda` design) | 33 MB of readback and 8.3 million sequential host additions per 3840x2160 frame, where the row kernel needs 8.6 kB and 2160 | The row sums are independent, so the device can do them |
| Add the rows on the device as well | No host loop | 2160 sequential additions in one thread to save 2160 on the host; the readback then needs a second kernel or a serial tail | No gain |

## Consequences

- **Positive**: `float_motion_cuda` equals `--backend cpu` at
  `--precision max` on every frame and output measured (`motion`, `motion2`,
  `motion3`): the Netflix 576x324 pair (48 frames; before 3.1e-6), both
  1920x1080 checkerboard pairs (3 frames each; before 1.36e-4), BBB 3840x2160
  (200 frames in the gate; before 2.4e-5 on the first 50), a 1280x720 10-bit
  clip, a 573x163 4:4:4 crop, and the Netflix pair with `motion_fps_weight`,
  `motion_max_val`, `motion_blend_factor` and `motion_blend_offset` set. The
  gate cell is an equality, so a twin defect of any size fails it.
- **Negative**: none measured. Paired runs of the `vmaf` tool on BBB
  3840x2160, RTX 4090, 200 frames, 25 pairs, host load 12 to 14: 3.00 ms per
  frame before and 2.98 after, paired difference median -0.12 ms (quartiles
  -0.57 to +0.25); the unchanged `float_psnr_cuda` in the same session reads
  2.84 and 2.80 (-0.03, quartiles -0.23 to +0.49). The twin now reproduces a
  value that is further from the exact sum than the one it returned before;
  that is the CPU extractor's value and the one the models were trained on.
- **Neutral / follow-ups**: `float_motion_sycl`, `float_motion_hip` and
  `float_motion_metal` keep their per-block sums
  (`T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01`); the host helper is backend
  neutral. Any stored `float_motion_cuda` output changes in its low digits (by
  at most the differences above). Guards: `test_cuda_float_motion_parity` and
  its 960x540 variant compare with `==` at 8, 10 and 12 bits (they fail on the
  old twin by 2.0e-5 and 6.2e-5), `test_float_motion_sad` pins the host
  helper's order and types without a device, and
  `test_cuda_kernel_source_contract.py` rejects a block reduction, a strided
  row loop, an fp64 row total, an fp64 division and a host sum of the twin's
  own.

## References

- `req` (maintainer, popup answer recorded in ADR-1397, 2026-10-01): "2, we tune afterwards, thats fixing now, what is the speed worth if the results are wrong".
- `req` (maintainer brief for the RC3 lanes, 2026-10-01): "Correctness before speed: a GPU twin matches the CPU extractor bit for bit where the row or its ADR says so" and "Bugs you find on the way get fixed (own small PR when out of scope), not just recorded."
- [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md) (the exact cell and the
  precedent), [ADR-1403](1403-cuda-strict-fp-every-kernel.md) (contraction
  off; found this difference), [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-0024](0024-netflix-golden-preserved.md),
  [ADR-0192](0192-gpu-long-tail-batch-3.md) (the twin),
  [ADR-1373](1373-cuda-twin-cpu-option-parity.md) (its options).
- `docs/state.md`: `T-CUDA-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01` (closed by
  this decision).
