<!-- markdownlint-disable MD013 MD029 MD060 -->
# ADR-0358: CUDA `motion` correctness — SAD race, pinned-mem leak, and motion2/motion3 precision parity with CPU

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `cuda`, `motion`, `correctness`, `precision`

## Context

A targeted cuda-reviewer pass on `core/src/feature/cuda/integer_motion_cuda.c`
on 2026-05-09 surfaced four real defects that were latent only because the
default golden gate exercises a single configuration (`motion_fps_weight = 1.0`,
`motion_moving_average = false`):

1. **Cross-stream race on the SAD accumulator.** `submit_fex_cuda()` issued
   `cuMemsetD8Async` of the single-int64 SAD buffer on `s->str`, but launched
   the `calculate_motion_score_kernel_*` kernel on the picture's stream
   (`pic_stream`). Both streams are `CU_STREAM_NON_BLOCKING` and no event
   pair links them; the kernel's `atomicAdd` to the same buffer is therefore
   ordered relative to the memset only by happenstance of single-frame
   cadence. The matching cousin `integer_motion_v2_cuda.c:188` already runs
   the memset on `pic_stream`.

2. **Pinned-memory leak of `s->sad_host`.** `init_fex_cuda()` allocates a
   single page-locked `uint64_t` via `vmaf_cuda_buffer_host_alloc` for the
   D2H copy of the SAD score. `close_fex_cuda()` did not free it. Each
   init/close cycle leaked one pinned page. `compute-sanitizer --tool
   memcheck --leak-check full` on `master` reports `LEAK SUMMARY: 8 bytes
   leaked in 1 allocations` traced to `init_fex_cuda → cuMemHostAlloc`.

3. **`motion2_score` skipped `motion_fps_weight × clip`.** The CPU reference
   (`integer_motion.c:563`) emits
   `MIN(score2 * motion_fps_weight, motion_max_val)` for the
   `VMAF_integer_feature_motion2_score` row. Both the CUDA collect path
   (line 468 pre-fix) and the flush path (line 359 pre-fix) emitted the raw
   `min(prev, cur)` (and raw `s->score` respectively) — bit-exact only
   while `motion_fps_weight == 1.0` and the `motion_max_val` clip never
   triggers. `motion3_score` was already weighted-and-clipped because the
   `motion3_postprocess_cuda` helper does it inline.

4. **Off-by-one in the moving-average guard.** `s->frame_index` is
   pre-incremented in `collect_fex_cuda()` *before* `motion3_postprocess_cuda`
   runs. The CPU reference (`integer_motion.c:523`) guards with
   `index > minimum_past_frames_needed` where `minimum_past_frames_needed
   == 1` for 3-frame mode — i.e. at framework-collect index 1 the guard
   evaluates `1 > 1 = false` and the moving average is *not* applied. With
   the pre-increment the GPU helper saw `frame_index == 2` at that call
   and applied the average (`2 > 1 = true`), diverging from CPU at the
   first non-zero frame whenever `motion_moving_average=true`.

In addition the kernels carry two performance advisories:

5. **Bank conflict on `__shared__ float tile[20*20]`.** With 32-bank shared
   memory and `TILE_W = 20`, `GCD(20, 32) = 4` aliases consecutive rows
   onto the same 4-bank cycle, producing a 2-way conflict between `(y=1,
   x=12..15)` and `(y=0, x=0..3)`. Padding the inner dimension to 21
   (`GCD(21, 32) = 1`) eliminates the conflict at +64 bytes per block
   (1764 vs 1600, far under the 48 KB SM limit).

6. **No `__launch_bounds__` directive.** The motion blur+SAD kernels use
   16×16 blocks (256 threads) and modest register pressure, so an explicit
   `__launch_bounds__(256, 8)` is well within reach for small-/mid-tier
   GPUs and lets nvcc trim register usage to keep occupancy stable across
   the supported gencode set.

## Decision

We will:

- Move the SAD `cuMemsetD8Async` from `s->str` onto `pic_stream` so it shares
  ordering with the kernel that consumes it; mirrors the v2 pattern (BLOCKER 1).
- Free `s->sad_host` in `close_fex_cuda()` and the `init_fex_cuda()` error
  unwind via `vmaf_cuda_buffer_host_free` (BLOCKER 2).
- Emit `MIN(score * motion_fps_weight, motion_max_val)` for
  `VMAF_integer_feature_motion2_score` in both the collect and flush paths,
  matching `integer_motion.c:563` line-for-line (NEEDS-CHANGES 3).
- Adjust the moving-average guard in `motion3_postprocess_cuda()` to
  `s->frame_index > 2` (compensating for the pre-increment) so framework-
  collect-index 1 skips averaging exactly as the CPU reference does
  (NEEDS-CHANGES 4).
- Pad the `motion_score.cu` and `motion_v2_score.cu` shared-memory tile
  inner dimension from `TILE_W` to `TILE_W + 1` and add
  `__launch_bounds__(BLOCK_X * BLOCK_Y, 8)` to all four kernels
  (ADVISORY 5 + 6).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Move increment of `frame_index` after `motion3_postprocess_cuda` instead of changing the guard | More obviously parallel to the CPU code's `index > 1` | Touches both the index==0 and index>0 branches, and the increment is also relied on for the index==0 zero-emit logic; harder to keep the existing semantics intact | The narrow-scope fix is to compensate at the consumer (the helper), keeping the increment site behavioural with the rest of the file |
| Insert `cuStreamWaitEvent` linking `s->str` to `pic_stream` for the memset | Preserves the existing two-stream split | Extra event hop per frame for no engineering benefit; the v2 path already established that running the memset on `pic_stream` is correct | The two-stream split exists so the D2H copy can run in parallel with subsequent work; the memset itself is fast and trivially co-locates with the kernel |
| Leave the bank-conflict padding to a follow-up PR | Smaller PR | The padding cost (+64 B/block) is negligible and the fix is one line per kernel; bundling avoids a second build/CI cycle | Bundle |

## Consequences

- **Positive**: motion / motion2 / motion3 are now bit-exact (places=4) with
  the CPU reference under all option combinations of `motion_fps_weight`,
  `motion_max_val`, and `motion_moving_average` — not just defaults; one
  page-locked allocation no longer leaks per init/close cycle; the SAD
  accumulator is no longer racing the kernel; the motion kernels no longer
  pay a 2-way bank-conflict tax on every shared-memory access.
- **Negative**: `__shared__` footprint of the motion blur kernels grows by
  64 bytes per block — irrelevant at the 48 KB SM cap.
- **Neutral / follow-ups**: the cuda-reviewer pass also flagged
  `core/src/cuda/common.c:388,416` for an inverted stream-select
  condition (no live callers); deferred to a separate small PR per the
  agent brief. Motion3 GPU coverage (T3-15(c)) remains a separate
  feature track and is *not* expanded here.

## References

- `req`: cuda-reviewer 2026-05-09 brief identifying BLOCKERS 1+2,
  NEEDS-CHANGES 3+4, and ADVISORIES 5+6.
- CPU reference: `core/src/feature/integer_motion.c:523, 563`.
- Existing correct pattern: `core/src/feature/cuda/integer_motion_v2_cuda.c:188`.
- Related: ADR-0219 (motion3 CUDA scaffold), ADR-0242 (engine-scope
  fence batching).
