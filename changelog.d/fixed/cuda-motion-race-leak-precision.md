- **CUDA `motion` correctness — four real bugs + two perf advisories.**
  Cuda-reviewer pass on `core/src/feature/cuda/integer_motion_cuda.c`
  (2026-05-09) surfaced and this PR fixes:
  - **Cross-stream race on the SAD accumulator** —
    `cuMemsetD8Async(s->sad->data, ..., s->str)` on the drain stream
    raced the kernel's `atomicAdd` running on `pic_stream`; both
    streams were `CU_STREAM_NON_BLOCKING` and no event pair linked
    them. The memset now runs on `pic_stream`, mirroring the existing
    pattern at `integer_motion_v2_cuda.c:188`.
  - **Pinned-memory leak of `s->sad_host`** —
    `vmaf_cuda_buffer_host_alloc` was paired with no
    `vmaf_cuda_buffer_host_free`, leaking one page-locked `uint64_t`
    per init/close cycle. `compute-sanitizer --tool memcheck` on
    `master` reports `LEAK SUMMARY: 8 bytes leaked in 1 allocations`,
    `0 bytes leaked` after the fix.
  - **`motion2_score` skipped the CPU reference's `MIN(score *
    motion_fps_weight, motion_max_val)` post-process** in both the
    collect (line 468 pre-fix) and flush (line 359 pre-fix) paths,
    diverging from `integer_motion.c:563` whenever
    `motion_fps_weight ≠ 1.0` or the clip triggered.
  - **Off-by-one in the `motion3_postprocess_cuda` moving-average
    guard.** `s->frame_index` is pre-incremented in `collect()` so
    the helper saw `frame_index == 2` at framework-collect index 1
    where the CPU reference (`integer_motion.c:523`) evaluates
    `1 > 1 = false`. Guard now reads `frame_index > 2` to compensate;
    only matters under non-default `motion_moving_average = true`.

  Also lands two performance advisories on `motion_score.cu` +
  `motion_v2_score.cu`:
  - Pad the shared-memory tile inner stride from `TILE_W = 20` to 21
    (`GCD(20, 32) = 4` → 2-way bank conflict; `GCD(21, 32) = 1`
    eliminates it; +64 B/block, far under the 48 KB SM cap).
  - Add `__launch_bounds__(BLOCK_X * BLOCK_Y, 8)` to all four motion
    kernels so nvcc trims register usage to keep occupancy stable
    across the supported gencode set.

  Default-settings cross-backend diff (Netflix
  `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv`, CUDA vs
  CPU, `places = 4`): **0 / 144 mismatches, max_abs = 0.00e+00** —
  bit-exact. `meson test -C build-cuda` passes 55/55 including the
  cuda preallocation-leak test. See
  [ADR-0358](../../docs/adr/0358-cuda-motion-race-and-precision-fixes.md).
