<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1412: `float_vif_cuda` computes the CPU's arithmetic, adds in the CPU's order and returns its scores bit for bit

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `cuda`, `gpu-parity`, `numerics`, `float-vif`, `testing`, `ci`, `rc3`, `fork-local`

## Context

After [ADR-1403](1403-cuda-strict-fp-every-kernel.md) took FMA contraction out
of every CUDA kernel, `float_vif_cuda` still matched the CPU extractor on no
frame and used 3.8e-5 of its 5e-5 tolerance on the Netflix pair
(`T-CUDA-FLOAT-VIF-RESIDUAL-2026-10-01`). The row named two suspects, the
device `log2f` and the order of the sums. A host replay of the kernel, cause by
cause, found four differences, and the largest was neither suspect
([Research-1412](../research/1412-cuda-float-vif-cpu-arithmetic.md)):

1. **The Gaussian taps.** Since the upstream sync of
   [ADR-0416](0416-vif-upstream-onthefly-filter-sync.md) (#758) `float_vif.c` derives
   its four filters at run time with `vif_get_filter()`: an fp64 `exp`, an
   fp32 sum, an fp32 division per tap. The kernel kept the decimal table the
   sync deleted from the CPU (`vif_filter1d_table_s`). 26 of the 34 taps
   differ from the computed ones, by one to three units in the last place and
   the centre tap of scale 0 by 14. This alone is 3.8e-5 on the Netflix pair.
2. **`log2`.** `vif_options.h` defines `VIF_OPT_FAST_LOG2`, so
   `vif_tools.c` never calls libm: `log2f` there is `log2f_approx()`, the
   exponent plus an fp32 Horner polynomial of nine coefficients in the
   mantissa. The
   kernel called the device `log2f`. Up to 2.2e-7.
3. **Types.** `vif_pixel_statistic_s()` takes `vif_sigma_nsq` as a `double`,
   so `(g * g * sigma1_sq) / (sv_sq + vif_sigma_nsq)` and the `1.0f +` around
   it are fp64 and are rounded to fp32 once, at the call. The kernel took a
   `float`. Up to 1.3e-7.
4. **Order.** `vif_statistic_s()` adds the terms of a row into one `float`
   and the row sums into another. The kernel reduced each warp in a tree, each
   16x16 block in fp32, and the blocks in `double` on the host. Up to 2.7e-6
   at 3840x2160.

The CPU extractor is the reference and its golden assertions are not modified
([ADR-0024](0024-netflix-golden-preserved.md)).
[ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md) decided for `psnr_hvs` that a
twin reproduces the CPU's accumulation instead of approximating it, and the
maintainer's direction for the remaining CUDA twins is the same: results
first, bit for bit, speed afterwards.

## Decision

We will make `float_vif_cuda` return the CPU extractor's values bit for bit:
the CPU's taps, the CPU's per-pixel arithmetic in the CPU's types, and the
CPU's two running sums.

**Taps from the CPU's routine.** `float_vif_cuda.c::float_vif_init_taps()`
calls `vif_get_filter_size()` and `vif_get_filter()` for
`(float)vif_kernelscale`, as `float_vif.c::init()` does, and every launch
hands the scale's taps to the kernel by value. No kernel file holds a tap.

**The reference's statistic, written once.**
`core/src/feature/cuda/float_vif/float_vif_device.h` holds
`vif_pixel_statistic_s()` and `log2f_approx()` operation for operation
(`fvif_pixel_statistic()`, `fvif_log2()`): `vif_sigma_nsq` is a `double`
kernel argument, the two log arguments are fp64, the comparison against it is
fp64, the `MAX` / `MIN` macros are the reference's ternaries. On the device
every rounding is an explicit `__fmul_rn` / `__fadd_rn` / `__fdiv_rn` /
`__dadd_rn` / `__ddiv_rn`; the host compiles the same header for a test.

**Terms, then row sums, then the rows.** `float_vif_compute` stores the
numerator and denominator term of every pixel. `float_vif_row_sums` runs one
thread per row and adds that row's terms left to right into one fp32
accumulator per output (`fvif_row_sum()`). The readback is two floats per row
per scale, and `fvif_sum_rows()` adds them top to bottom on the host, again in
fp32. Nothing reduces per warp or per block. The terms are stored column by
column so that the row threads read neighbouring addresses.

**The CPU's option table.** The twin gains `vif_scale1_min_val`,
`vif_scale2_min_val` and `vif_scale3_min_val` (default 0, as on the CPU) and
applies them through the shared emitter. Without them a per-scale ratio below
the floor was published unclamped.

**Gate.** `float_vif` gets a `cuda` entry in `EXACT_TWINS`
(`scripts/ci/cross_backend_calibration.py`): the CPU ↔ CUDA cell is compared
with tolerance 0 at `--precision max`. The other `float_vif` twins keep
places=4.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Update the kernel's tap table to today's `vif_get_filter()` output | Smallest change; removes 3.8e-5 | A second copy of numbers the CPU derives at run time, through libm's `exp`; it is how the twins drifted when the CPU changed in #758 | HISS-19: one implementation of the filter |
| Fix the taps only and tighten the tolerance | No new kernel, no fp64 | Leaves three differences of up to 2.7e-6 | The direction is bit for bit |
| Read every term back and add on the host | No row-sum kernel | 66 MB of readback per 3840x2160 frame | One 128-thread-per-block kernel does it on the device |
| An order-preserving reduction inside the compute kernel | One launch fewer | A row spans every block of its row; the carry would have to pass from block to block in order | Not expressible in independent blocks |
| fp32 pairs instead of fp64 for the two quotients | No fp64 on the device | Needs a proof for the rounded quotient; CUDA has fp64 and the reference's own operations are available | Kept for the fp64-less SYCL twin; a tuning candidate here (`T-CUDA-FLOAT-VIF-EXACT-THROUGHPUT-2026-10-01`) |
| Make the CPU order-independent (`double` sums, libm `log2f`) | Twins could reduce freely | Moves the Netflix golden values | [ADR-0024](0024-netflix-golden-preserved.md) |

## Consequences

- **Positive**: measured on an RTX 4090 at `--precision max`, every output of
  every frame equals `--backend cpu`: Netflix 576x324 at 8 bits (48 frames)
  and at 10, 12 and 16 bits (3 frames each), both 1920x1080 checkerboard pairs
  (3 frames each) and BBB 3840x2160 (50 frames), 452 of 452 scale outputs;
  with `debug=true`, 1 605 of 1 605 outputs (the frame ratio and the eight
  per-scale sums included); the gate over all 200 BBB frames reports 0. Before,
  without the 16-bit fixture: 10 of 440, largest difference 3.81e-5. The same
  holds when clang's CUDA
  driver builds the kernels. Non-default `vif_enhn_gain_limit`,
  `vif_sigma_nsq`, `vif_skip_scale0` and the new floors are identical too
  (`test_cuda_float_vif_parity`).
- **Positive**: the tile loads clamp the indices no output consumes
  (`cuda_tile_index.h`). Read from the index arithmetic, no fault observed: on
  a frame narrower or shorter than 72 pixels the scale-3 tile of the old
  kernel reflected a padding index to below zero.
- **Negative**: the kernels of one instance cost 0.72 ms per 3840x2160 frame
  before and 1.00 ms after (median of seven alternating runs of nine instances
  against one, 60 frames, host load average 20 to 24; 0.57 and 0.89 ms at
  load 12 to 14). A build with an fp32 statistic measured 0.70 ms next to the
  0.89, so about 0.19 ms of the increase is the fp64 arithmetic (two
  quotients and three sums per pixel) and the rest the term store and the row
  sums. A single `float_vif_cuda` run does not show it, because at 4K it
  waits for the frame: 1.97 and 1.96 ms per frame (nine alternating pairs of
  198 frames, paired difference +0.02, quartiles -0.37 to +0.11).
  `T-CUDA-FLOAT-VIF-EXACT-THROUGHPUT-2026-10-01`.
- **Negative**: 66 MB more device memory at 3840x2160 (two floats per pixel of
  scale 0), on top of 50 MB of raw planes and scale buffers.
- **Negative**: stored `float_vif_cuda` outputs change by up to 3.8e-5.
- **Neutral / follow-ups**:
  - The contract mirrors three CPU properties. If any changes, the twin
    changes in the same PR: `vif_get_filter()` (the taps),
    `VIF_OPT_FAST_LOG2` and `log2f_approx()` (the polynomial), and
    `vif_pixel_statistic_s()` / `vif_statistic_s()` (types and order).
    `test_float_vif_device_math` compares the header with those routines on
    the host and fails when they diverge;
    `test_cuda_float_vif_exact_contract.py` pins the design at source level.
  - The reference is the contraction-free CPU build. x86-64 has no FMA in its
    baseline, so `vif_tools.c` cannot contract there; the device-free test is
    registered for x86-64.
  - `float_vif_sycl`, `float_vif_hip` and `float_vif_metal` carry the same
    tap table and call their device `log2`:
    `T-GPU-FLOAT-VIF-CPU-ARITHMETIC-2026-10-01`.

## References

- `req` (maintainer brief, 2026-10-01): "results before speed; a twin
  reproduces the CPU bit for bit, and tuning comes afterwards."
- [ADR-1403](1403-cuda-strict-fp-every-kernel.md),
  [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md),
  [ADR-0416](0416-vif-upstream-onthefly-filter-sync.md),
  [ADR-1217](1217-gpu-float-vif-options-reach-kernel.md),
  [ADR-1373](1373-cuda-twin-cpu-option-parity.md),
  [ADR-1374](1374-cuda-integer-tiny-frame-guards.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-0024](0024-netflix-golden-preserved.md).
- [Research-1412](../research/1412-cuda-float-vif-cpu-arithmetic.md).
