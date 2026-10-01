<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1419: `float_motion_hip` stores its differences transposed and adds each row in the CPU's order

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `hip`, `gpu-parity`, `numerics`, `float-motion`, `testing`, `ci`, `rc3`, `fork-local`

## Context

[ADR-1409](1409-float-motion-twins-cpu-float-sum.md) established that the
score of `float_motion.c` is a property of the order of its additions: the
absolute differences of one row are added, left to right, into one `float`,
the row sums top to bottom into a second `float`, and the total is divided by
the pixel count in `float`. It made `float_motion_cuda` reproduce that order,
[ADR-1411](1411-sycl-float-motion-cpu-float-sum.md) did the same for SYCL, and
`T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01` stayed open for HIP and Metal.

`float_motion_hip` summed each 16x16 block on the device and the blocks in
`double` on the host. Measured on a gfx1036 at `--precision max` against
`--backend cpu`, with the blur already built without FMA contraction
([ADR-1407](1407-hip-strict-fp-every-kernel.md)): 3.1e-6 on the Netflix
576x324 pair and 1.36e-4 on the 1920x1080 checkerboard pairs, above the 5e-5
tolerance of [ADR-0214](0214-gpu-parity-ci-gate.md); 2.2e-4 with
`motion_add_scale1`. Only the frames whose score is 0 matched.

The HIP twin differs from the other two in two ways. It implements every
option of the CPU extractor
([ADR-1404](1404-hip-float-motion-motion3-and-options.md)): the half-size term
of `motion_add_scale1` and the chroma planes of `motion_add_uv` are sums of
the same kind. And the device it is measured on is an integrated GPU with two
compute units. The kernel ADR-1409 and ADR-1411 use, one thread per row that
reads both blurred planes, takes 145 ms per 3840x2160 frame there, where the
whole twin took 18: the threads of a wave walk different rows, so every step
of a wave reads one cache line per row from memory the GPU shares with the
host.

## Decision

We will make `float_motion_hip` return the CPU extractor's scores bit for bit
by storing every absolute difference first and adding each row afterwards.

**Differences in a parallel kernel, stored transposed.** The blur kernel
already visits every sample; from the second frame on it also stores
`|cur - prev|` of that sample. The differences go into a plane whose rows are
grouped in sets of 64 and in which, within a group, the samples of one column
are adjacent (`vmaf_hip_float_motion_diff_index()`). For `motion_add_scale1` a
second parallel kernel (`float_motion_hip_scale1_diff`) scales both blurred
frames with the CPU's bilinear scaler and stores the differences of the
half-size plane the same way.

**Row sums in the CPU's order.** `float_motion_hip_row_sum` runs one thread
per row, 64 threads per block, one block per group. A thread adds the `width`
differences of its row, left to right, into one fp32 accumulator. At every
step the threads of a block read 64 consecutive floats, so the kernel streams
through memory instead of gathering. The same kernel adds the scale-0 and the
scale-1 differences of every plane.

**Rows on the host.** `collect()` builds each plane's score with
`vmaf_hip_float_motion_plane_score()`: the fp32 mean of
`vmaf_float_motion_score_from_row_sads()` (ADR-1409's helper), plus the fp32
scale-1 mean, and adds the planes in `double`, as
`motion.c::vmaf_image_sad_c()` and `float_motion.c::motion_score_pair()` do.

**One header.** The difference, the layout index, the row sum, the bilinear
sample and the plane score live in
`core/src/feature/hip/float_motion/float_motion_rows.h`, plain C and HIP C++.
The kernels and the extractor compile it, and a device-free test compiles the
same lines against `motion.c::compute_motion()`.

**Gate.** `float_motion` lists `hip` next to `cuda` and `sycl` in
`EXACT_TWINS` (`scripts/ci/cross_backend_calibration.py`).

## Alternatives considered

Times are per 3840x2160 frame of `--backend hip --feature float_motion` on the
gfx1036, steady state inside the process, with other jobs loading the host.
The twin took 18 ms before.

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Parallel differences stored transposed, then one thread per row (this ADR) | The CPU's bits with every option; 20 ms; consecutive loads in the serial kernel | One more device plane per picture plane (`width` x `height` floats, rounded up to 64 rows), and a second one with `motion_add_scale1` | Chosen |
| One thread per row reading both blurred planes (the CUDA and SYCL kernel) | No extra plane; the kernel of ADR-1409 unchanged | 145 ms per frame, and 229 ms with `motion_add_scale1`, where the scaler also runs inside the serial loop | Eight times slower than the twin was: the lanes of a wave read 32 or 64 different cache lines at every step |
| Differences stored in picture order, one thread per row | No transposed index | Half the loads of the option above, still one cache line per lane and step | The gathers are the cost, not the subtraction |
| Read the differences back and add them on the host | Trivially exact; the host's cache handles a row sum well | 33 MB of readback per plane and frame at 3840x2160, and the sum runs on the thread that drives every extractor | The rows are independent, so the device can add them once it reads them in order |
| Keep the per-block sum, keep the tolerance | No code change | 1.36e-4 at 1080p is above the tolerance; any tolerance accepts a twin defect below it | Rejected by ADR-1409 |

## Consequences

- **Positive**: `float_motion_hip` equals `--backend cpu` at `--precision max`
  on every value of `motion`, `motion2` and `motion3` measured on a gfx1036:
  the Netflix 576x324 pair at 8 bits (48 frames) and 10 bits (3), both
  1920x1080 checkerboard pairs (3 each) and BBB 3840x2160 (20 frames), 231
  values per option set, for the default options, `motion_add_scale1`,
  `motion_add_uv`, both together, `motion_filter_size` 3 and 1, and the fps
  weight, blend and cap options: 1617 of 1617 values, where 237 of 1617 matched
  before (the zero frames, and the capped scores of the last set). The gate
  cell is an equality, so a twin defect of any size fails it.
- **Negative**: the stored differences and the second kernel cost time on the
  gfx1036. Medians of seven interleaved runs of both builds:
  2.50 to 2.78 ms per 1920x1080 frame and 18.09 to 19.76 per 3840x2160 frame
  with the default options, 3.03 to 3.74 and 21.02 to 24.68 with
  `motion_add_scale1`, 19.05 to 20.52 at 3840x2160 with `motion_add_uv`.
  `--model version=vmaf_float_v0.6.1` reads 49.98 and 50.11 ms at 1920x1080
  and 192.66 and 199.17 at 3840x2160 (five runs each), inside the spread of
  the runs. The twin holds one more float plane per picture plane on the
  device, two with `motion_add_scale1`. It now
  reproduces a value that is further from the exact sum than the one it
  returned before; that is the CPU extractor's value and the one the models
  were trained on.
- **Neutral / follow-ups**: `float_motion_metal` keeps its per-block sum
  (`T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01` stays open for it). Any
  stored `float_motion_hip` output changes in its low digits. A discrete AMD
  GPU is unmeasured; it has the lanes to run the gathering kernel fast, and
  the transposed layout costs it nothing. Guards: `test_hip_float_motion_rows`
  (the header against `compute_motion()`, no device, with and without
  `motion_add_scale1`, odd sizes), `test_hip_float_motion_parity` and its
  960x540 variant (`==` on `motion`, `motion2` and `motion3` of four frames,
  with the default options and with `motion_add_scale1` + `motion_add_uv`; 10
  of 12 scores differ on the old twin) and `test_hip_kernel_source_contract.py`
  (a wave reduction, an fp64 row sum, an untransposed read, an fp64 scale sum
  and a host sum of the twin's own are each rejected).

## References

- `req` (maintainer brief for the HIP lane, 2026-10-01): "T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM, HIP part: reproduce the CPU's per-row fp32 running sums like PR #1699 does for CUDA (core/src/feature/float_motion_sad.h)."
- `req` (maintainer brief for the HIP lane, 2026-10-01): "The gfx1036 is slow: correctness evidence first, timings second."
- [ADR-1409](1409-float-motion-twins-cpu-float-sum.md) (the decision this
  applies to HIP, and the host helper),
  [ADR-1411](1411-sycl-float-motion-cpu-float-sum.md) (SYCL),
  [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md) (the exact cell),
  [ADR-1407](1407-hip-strict-fp-every-kernel.md) (contraction off),
  [ADR-1404](1404-hip-float-motion-motion3-and-options.md) (the twin's
  options), [ADR-0214](0214-gpu-parity-ci-gate.md).
- `docs/state.md`: `T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01` (HIP part
  closed by this decision).
