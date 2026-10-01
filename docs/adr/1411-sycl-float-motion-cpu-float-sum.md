<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1411: `float_motion_sycl` adds its SAD in the CPU's order and returns the CPU's scores bit for bit

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `sycl`, `gpu-parity`, `numerics`, `float-motion`, `testing`, `ci`, `rc3`, `fork-local`

## Context

[ADR-1409](1409-float-motion-twins-cpu-float-sum.md) established that the
score of `float_motion.c` is a property of the order of its additions:
`compute_motion_simd()` adds the absolute differences of one row, left to
right, into one `float`, adds the row sums top to bottom into a second
`float`, and divides by the pixel count in `float`. It made `float_motion_cuda`
reproduce that order and left the SYCL, HIP and Metal twins as
`T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01`.

`float_motion_sycl` reduced the differences of each 32x4 work-group on the
device (a sub-group reduction, then the sub-groups of the group) and added the
groups in `double` on the host. Measured on an Arc A380 at `--precision max`
against `--backend cpu`, with the blur already built without FMA contraction
([ADR-1367](1367-sycl-strict-fp-every-feature-tu.md)): 3.1e-6 on the Netflix
576x324 pair, 2.4e-5 on BBB 3840x2160 and 1.36e-4 on both 1920x1080
checkerboard pairs, above the 5e-5 tolerance of
[ADR-0214](0214-gpu-parity-ci-gate.md). Those are the numbers the CUDA twin had
with the same reduction: the blur matched, the reduction did not.

A SYCL kernel has two constraints the CUDA kernel does not: it may not use the
fp64 type ([ADR-0220](0220-sycl-fp64-fallback.md)), and on Arc A-series GPUs
under the xe driver it may not use scratch memory
([ADR-1395](1395-sycl-kernels-no-scratch.md)). A sequential fp32 sum needs
neither.

## Decision

We will make `float_motion_sycl` return the CPU extractor's scores bit for
bit, by the design of ADR-1409.

**Row sums on the device.** `launch_float_motion_row_sad()`
(`core/src/feature/sycl/float_motion_sycl.cpp`) runs one work-item per row.
`fm_row_sad()` adds `|cur[j] - prev[j]|` for `j = 0 .. w - 1` into one fp32
accumulator and stores it; the frame's readback is `h` floats. The blur kernel
writes the blurred plane only. The TU contains no group, sub-group or atomic
reduction.

**Sub-group size 8.** The kernel requests it. The lanes of a hardware thread
are rows, each loop step is two gathers, and the pass is bound by reading the
two blurred planes again; narrow sub-groups put more hardware threads on it.

**Rows on the host.** `collect()` calls
`vmaf_float_motion_score_from_row_sads()`
(`core/src/feature/float_motion_sad.h`), the helper ADR-1409 introduced. The
twin keeps no sum of its own.

**Gate.** `float_motion` lists `sycl` next to `cuda` in `EXACT_TWINS`
(`scripts/ci/cross_backend_calibration.py`): the CPU, CUDA and SYCL cells are
compared with tolerance 0 at `--precision max`. HIP and Metal keep places=4.

## Alternatives considered

Kernel times are per 3840x2160 frame on an Arc A380: the frame time of the
`vmaf` tool minus the 3.51 ms it takes with the row kernel left out. The old
combined blur-and-reduce kernel spent 0.34 ms on its SAD.

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One work-item per row, sub-group size 8 (this ADR) | The CPU's bits on every frame; the simplest kernel; no dependence on how work-items map to lanes; 0.70 ms | 0.36 ms more than the old reduction, because both blurred planes are read a second time | Chosen |
| The same kernel at the compiler's sub-group size (32), or 16 | No attribute | 1.12 ms and 0.82 ms: 68 or 135 hardware threads for 2160 rows | Slower, same result |
| One sub-group of 16 consecutive pixels per row, summed lane by lane with `select_from_group()` | Contiguous loads, one thread per row | 1.10 ms: the shuffle per pixel costs more than the gathers it saves; correctness depends on the sub-group being exactly the work-group | Slower and more fragile |
| One work-group per row, 16-wide vector loads, a chain that is uniform across the lanes | Contiguous loads, scalar adds | 0.80 ms; seven of eight lanes compute a value that is thrown away | Not faster than the simple kernel |
| Keep the per-group sum, raise the tolerance | No code change | Accepts any twin error below the CPU's own rounding error, which grows with the frame area | Rejected by ADR-1409 |
| Read all differences back and add them on the host | Trivially exact | 33 MB of readback per 3840x2160 frame | The rows are independent, so the device can add them |
| Add the rows inside the blur kernel | No second pass over the planes | The sum of a row is sequential across the 32-pixel work-groups of the blur; no kernel that blurs in parallel can produce it | Not possible with a tiled blur |

## Consequences

- **Positive**: `float_motion_sycl` equals `--backend cpu` at
  `--precision max` on every frame of `motion` and `motion2` measured on an
  Arc A380: the Netflix 576x324 pair (48 frames; before 3.1e-6), both
  1920x1080 checkerboard pairs (3 frames each; before 1.36e-4), BBB 3840x2160
  (200 frames; before 2.4e-5), the Netflix pair at 10, 12 and 16 bits and as
  4:2:2 10-bit, and the Netflix pair with `motion_fps_weight` and
  `motion_max_val` set. The same holds against a GCC build of the CPU
  extractor and against the CPU extractor of the icx build itself. The gate
  cell is an equality, so a twin defect of any size fails it.
- **Negative**: 0.38 ms more per 3840x2160 frame on the Arc A380 through the
  `vmaf` tool: 3.85 ms before, 4.23 after (medians of 25 paired 200-frame
  runs, host load 7 to 11; the untouched `float_psnr_sycl` read 3.37 and 3.38
  in the same session). On the Netflix 576x324 pair: 0.15 ms before, 0.22
  after (15 paired 48-frame runs). The twin now reproduces a value that is
  further from the exact sum than the one it returned before; that is the CPU
  extractor's value and the one the models were trained on.
- **Neutral / follow-ups**: `float_motion_hip` and `float_motion_metal` keep
  their per-block sums (`T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01` stays
  open for them). The twin still provides no `motion3`
  (`T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`). Any stored `float_motion_sycl`
  output changes in its low digits (by at most the differences above). The
  row kernel uses no scratch memory (`test_sycl_kernel_scratch`, 110 kernels
  audited on the A380, the ratchet list unchanged). Guards:
  `test_sycl_float_motion_parity` and its 960x540 variant compare every frame
  with `==` at 8, 10 and 12 bits (they fail on the old twin by 2.0e-5 and
  6.1e-5), and `test_sycl_kernel_source_contract.py` rejects a group
  reduction, a strided row loop, a blocked launch, a host sum of the twin's
  own and an fp64 row total.

## References

- `req` (maintainer brief for the SYCL exactness lane, 2026-10-01): "results before speed; a twin reproduces the CPU bit for bit, tuning comes afterwards".
- `req` (maintainer, popup answer recorded in ADR-1397, 2026-10-01): "2, we tune afterwards, thats fixing now, what is the speed worth if the results are wrong".
- [ADR-1409](1409-float-motion-twins-cpu-float-sum.md) (the decision this
  applies to SYCL, and the host helper),
  [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md) (the exact cell),
  [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md) (contraction off),
  [ADR-1395](1395-sycl-kernels-no-scratch.md),
  [ADR-0220](0220-sycl-fp64-fallback.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-1365](1365-sycl-twin-cpu-option-parity.md) (the twin's options).
- `docs/state.md`: `T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01` (SYCL part
  closed by this decision).
