<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1030: HIP adm_decouple dangling body + VIF wavefront 32-bit carry + Metal motion vertical halo

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `hip`, `metal`, `correctness`, `gpu`

## Context

Three HIGH-severity correctness defects were identified in the HIP and Metal
GPU backends during the r6-hip-kernel / r6-metal-kernel audit round:

**1. HIP adm_decouple dangling function body**
`core/src/feature/hip/integer_adm/adm_decouple.hip` line 27 contained a bare
`{ ... return temp; }` block — the body of `get_best15_from32` — without a
function declaration. The declaration was stripped during an earlier edit but
the body was left in place, making the translation unit invalid C++. The
function is called at lines 167-169 of the same file for the scale-1..3
decouple path. The definition lives in `adm_decouple_inline.hip` (which is
correctly `#include`'d by `adm_csf.hip` and `adm_cm.hip`) but was never
included in `adm_decouple.hip` itself.

**2. HIP wavefront_reduce_i64 32-bit overflow drops carry**
`core/src/feature/hip/integer_vif/vif_statistics.hip` lines 215-222 split
`int64_t` into `uint32_t lo` + `uint32_t hi`, reduced each half independently
across 64 lanes, then reassembled with bitwise OR:
`(int64_t)lo | ((int64_t)hi << 32)`. When the lane-sum of `lo` overflows
`uint32_t` (possible for `num_non_log` with `sigma2_sq` values up to ~2^31
per lane, 64 lanes giving ~2^37 total), carry into the upper 32 bits is
silently dropped. The result is systematically wrong VIF accumulator values
whenever `num_non_log` or similarly large accumulators exceed 2^32 total.

**3. Metal float_motion missing vertical halo (8bpc and 16bpc)**
`core/src/feature/metal/float_motion.metal` used `TILE_H=16` and
`wg_oy = bid.y * 16` (no halo) for the shared-memory tile load. The
5-tap vertical filter reads taps at `row + k - 2` (k=0..4), requiring
rows `wg_oy - 2` and `wg_oy - 1` to be present in the tile. For any
workgroup with `bid.y > 0`, those rows were absent, so `tile_row =
src_row - wg_oy` computed a negative value that clamped to 0, reading
the wrong row. Every workgroup except the first had 2 corrupted output
rows per frame, propagating into `cur_blurred` and the SAD accumulation.

## Decision

Apply three surgical fixes:

1. **adm_decouple.hip**: replace the bare body block at lines 27-33 (and the
   duplicate `COS_1DEG_SQ` constexpr on line 36) with a single
   `#include "adm_decouple_inline.hip"` after `common.h`. The inline header is
   include-guarded; its `get_best15_from32` and `COS_1DEG_SQ` become available
   to both kernel functions.

2. **vif_statistics.hip**: change the `wavefront_reduce_i64` reassembly from
   `(int64_t)lo | ((int64_t)hi << 32)` to
   `(int64_t)((uint64_t)lo + ((uint64_t)hi << 32))`. The unsigned-addition
   form preserves carry from `lo` into the upper word.

3. **float_motion.metal**: change `TILE_H` from 16 to 20, change both
   `wg_oy = bid.y * 16` expressions (tile-load and vertical-filter phases) to
   `wg_oy = bid.y * 16 - HALF_FW`, and change `ty = lid2.y` in the
   horizontal-filter phase to `ty = lid2.y + HALF_FW`. Applied identically
   to both the 8bpc and 16bpc kernels, mirroring the already-correct pattern
   in `integer_motion.metal` (lines 44-45, 82).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Forward-declare `get_best15_from32` inline in adm_decouple.hip | Avoids include dependency | Duplicates a non-trivial inline; divergence risk on future changes | Single source of truth via include is cleaner |
| 64-bit shuffle for wavefront_reduce_i64 (two 32-bit shuffles + carry) | Avoids the split entirely | AMD `__shfl_xor` takes 32-bit operands; a full 64-bit shuffle requires the same split/reassemble pattern anyway | No benefit over fixing the reassembly |
| Read outside-tile pixels directly from device memory in vertical pass | Removes TILE_H constraint | Adds conditional global-load paths, increasing divergence and register pressure | Shared-memory tiling with correct halo is the established pattern |

## Consequences

- **Positive**: HIP adm_decouple scale-1..3 path compiles and links for the
  first time; `get_best15_from32` is defined in exactly one place.
- **Positive**: HIP VIF `num_non_log` accumulations no longer silently lose
  carry, restoring numerical correctness for high-contrast content where
  `sigma2_sq` per lane exceeds ~2^26.
- **Positive**: Metal float_motion produces correct blurred frames for all
  workgroups, not just the first row of workgroups; SAD scores are now correct
  for all frames on content taller than 16 pixels.
- **Negative**: Metal `s_tile` and `s_vert` shared-memory usage increases from
  `16×20×4 = 1280` bytes to `20×20×4 = 1600` bytes per threadgroup (each),
  a 25% increase. This remains well within Apple GPU shared-memory limits
  (32 KiB typical).
- **Neutral**: No change to the public C API, CLI flags, or output schema.

## References

- r6-hip-kernel + r6-metal-kernel audit round findings (HIGH × 4).
- `adm_decouple_inline.hip` include-guard `ADM_DECOUPLE_INLINE_HIP_`.
- `integer_motion.metal` precedent for `TILE_H=20` / `wg_oy - HALF_FW`.
- ADR-0552 (deterministic wavefront reduction, original HIP VIF fix).
- req: user-provided findings brief specifying all three fixes and their
  root causes verbatim.
