---
paths:
  - core/src/feature/motion.c
  - core/src/feature/ms_ssim.c
  - core/src/feature/speed.c
invariant: MS-SSIM honors enable_lcs; float_motion surfaces extra options; SpEED singular is non-fatal.
area: backends-metrics-and-tests
---
<!-- markdownlint-disable MD013 MD060 -->
# Motion, MS-SSIM, and SpEED-chroma GPU metric contracts

- **MS-SSIM `enable_lcs` GPU contract** (fork-local,
  [ADR-0243](../../docs/adr/0243-enable-lcs-gpu.md)).
  [`src/feature/cuda/integer_ms_ssim_cuda.c`](../src/feature/cuda/integer_ms_ssim_cuda.c)
  and
  [`src/feature/vulkan/ms_ssim_vulkan.c`](../src/feature/vulkan/ms_ssim_vulkan.c)
  emit 15 extra metrics — `float_ms_ssim_{l,c,s}_scale{0..4}` —
  when `enable_lcs` option is true, mirroring CPU
  `float_ms_ssim` extractor in
  [`src/feature/float_ms_ssim.c`](../src/feature/float_ms_ssim.c#L189-L221).
  Metric names, ordering (metric-wise — all `l_scale*` first,
  then `c_*`, then `s_*`), and `places=4` cross-backend contract
  are part of public API surface; never rename, reorder, or
  introduce per-backend variations. Kernels themselves
  (`ms_ssim_vert_lcs` CUDA / vert pass in `ms_ssim.comp` Vulkan)
  already compute per-scale `l_means[i]` / `c_means[i]` /
  `s_means[i]` doubles — gating only host-side
  `vmaf_feature_collector_append` calls keeps default-path
  (`enable_lcs=false`) output bit-identical to pre-T7-35
  binary. Cross-backend gate's `float_ms_ssim_lcs`
  pseudo-feature in
  [`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
  and
  [`scripts/ci/cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py)
  enforces contract; never drop `FEATURE_ALIASES` entry
  or matching `FEATURE_TOLERANCE` row on rebase.
- **`float_motion` extra-options surface (upstream port from Netflix
  b949cebf, 2026-04-29).** [`src/feature/float_motion.c`](../src/feature/float_motion.c)
  exposes four extra options (`motion_add_scale1`, `motion_add_uv`,
  `motion_filter_size`, `motion_max_val`), emits `motion3_score` on
  second frame. Default Y-plane / scale-0 path stays bit-identical
  to pre-port baseline by routing through `compute_motion_simd()`
  (AVX2 / AVX-512 / NEON `float_sad_line` dispatch); non-default paths
  (`scale1`, UV) fall through to scalar `compute_motion()` in
  [`src/feature/motion.c`](../src/feature/motion.c).
  `picture_copy()` / `picture_copy_hbd()` signature in
  [`src/feature/picture_copy.{c,h}`](../src/feature/picture_copy.h) gained
  trailing `int channel` parameter (upstream d3647c73 prerequisite); every
  fork-local caller (`float_adm.c`, `float_moment.c`,
  `float_ms_ssim.c`, `float_psnr.c`, `float_ssim.c`, `float_vif.c`,
  `cuda/integer_ms_ssim_cuda.c`, `sycl/integer_ms_ssim_sycl.cpp`,
  `vulkan/ms_ssim_vulkan.c`, `vulkan/ssim_vulkan.c`) passes `0` for
  Y-plane. (`sycl/integer_ssim_sycl.cpp` applies same scaling on
  device since ADR-1370 and no longer calls it.) On future upstream
  syncs, never drop SIMD fast-path wrapper: NASA/JPL Power-of-10
  inner-loop budget still demands it, and Netflix golden-data gate
  ([ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md)) is regression-
  flagging if default path stops dispatching to `vmaf_image_sad_avx2`
  / `_avx512` / `_neon`. See
  [`docs/rebase-notes.md` §0049](../../docs/rebase-notes.md).
- **`motion3_score` GPU contract (T3-15(c) / ADR-0219).** Three GPU
  motion twins (`src/feature/vulkan/motion_vulkan.c`,
  `src/feature/cuda/integer_motion_cuda.c`,
  `src/feature/sycl/integer_motion_sycl.cpp`) emit
  `VMAF_integer_feature_motion3_score` in 3-frame window mode by
  applying CPU's host-side post-process to motion2: `clip(motion_blend(
  motion2 * motion_fps_weight, motion_blend_factor,
  motion_blend_offset), motion_max_val)` with optional moving-average.
  No device-side state is added — motion3 is deterministic scalar
  function of motion2. Two invariants rebase story depends on:
  (1) `motion_five_frame_window=true` on CUDA / SYCL / HIP twins = frame
  n-2 kept on device + CPU's `vmaf_motion_window_flush()` at flush,
  bit-identical (ADR-1491); a twin without the window (Metal) must not
  declare the option or must mark it `VMAF_OPT_FLAG_DEFAULT_ONLY`, so the
  CPU extractor computes it — never silently fall back to 3-frame path;
  (2) any Netflix
  upstream sync touching `motion_blend()` in
  [`motion_blend_tools.h`](../src/feature/motion_blend_tools.h),
  `motion_max_val` clip, or moving-average rule MUST mirror
  change into `motion3_postprocess_*` across all three GPU files
  in same PR. Cross-backend parity gate at `places=4`
  (`scripts/ci/cross_backend_parity_gate.py` +
  `scripts/ci/cross_backend_vif_diff.py` `FEATURE_METRICS["motion"]`
  → `integer_motion3`) catches drift, but only after full GPU
  run. See [`docs/rebase-notes.md` §0219](../../docs/rebase-notes.md).

## GPU SpEED-chroma: singular is not an error (ADR-1202)

`core/src/feature/speed.c` overloads single `int` to mean *singular
covariance matrix*: `solve_covariance_system()` returns `cannot_invert`,
`speed_extract_score()` forwards it, and `extract_fex()` reads it to impute
`uv` score from whichever chroma channel inverted. That is CPU contract and
it is correct there.

**GPU twins do not work that way.** In
`core/src/feature/cuda/speed_chroma_cuda.c`,
`core/src/feature/sycl/speed_chroma_sycl.cpp` and
`core/src/feature/hip/speed_chroma_hip.c`, linear-algebra helper handles
singularity itself (warn, zero solution, return 0), reserves its return
value for hard API failures. Singularity travels out through explicit
`bool *singular_out`.

Never "simplify" that out-parameter away by keying imputation off
return value again. That is what code did before ADR-1202, meant
real device error was routed into singular path: one channel failing
imputed from other, and both channels failing fell through to
`(0 + 0) * 0.5`, appended three `0.0` scores, returned success.
`CUDA_ERROR_INVALID_VALUE` on every 4K frame therefore surfaced as pooled
VMAF 3.4 points off CPU score on exit-0 run, not as error.

Two related invariants in same files:

- **`SC_SOLVE_WARPS_PER_BLOCK` bounds block size; block count scales.**
  Backward-substitution launch maps one warp per linear system. Deriving
  block size from system count instead of block count exceeds
  CUDA's 1024-thread block limit above 256 systems — every 4K frame. SYCL
  and HIP twins already compute this correctly; keep three consistent.
- **channel with exactly one singular side scores 0**, matching
  `speed_extract_score()`. Averaging in zeroed solution instead produces
  inflated score.

GPU parity tests in repository runner's `--suite=fast` selection all run below 256-system
threshold, cannot catch either invariant. Check 4K agreement against CPU
backend by hand. See `docs/rebase-notes.md` entry
`fix/cuda-speed-chroma-4k-launch`.
