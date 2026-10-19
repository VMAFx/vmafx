---
paths:
  - core/src/feature/metal/*.mm
  - core/src/feature/metal/*.metal
  - core/src/feature/metal/metal_portable.h
  - core/src/feature/metal/metal_plane_index.h
invariant: One .mm + .metal pair per extractor; partials per workgroup, no atomics; simd_sum is 32-bit; PSO slots bridge-retained.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Rebase-sensitive invariants

- **Five-plane twins refuse plane past uint index**
  (T-METAL-UINT-PLANE-INDEX-2026-10-05). `float_vif`, integer SSIM,
  `float_ssim` and `float_ms_ssim` index five moment planes as `k * N + at`
  in `uint`. Their hosts call `vmaf_mtl_plane_index_check()`
  (`metal_plane_index.h`) with five planes in `init_geometry()` /
  `configure()` / `validate_dimensions()`, before `vmaf_metal_context_new()`.
  A new twin that keeps several planes in one `uint`-indexed buffer calls it
  too. `core/test/test_metal_plane_index_contract.py` reads kernels and
  hosts; `test_metal_plane_index` holds limit (858,993,459 samples).

- **Only wired `.mm` + `.metal` pairs exist** (ADR-0545). Metal
  feature directory carries exactly one wired `.mm` + `.metal` pair
  per registered extractor, each with meson entry and registry slot
  in `feature_extractor.c`. Wired set = full 17-kernel cross-backend
  metric set: `integer_motion_v2` (registers as `motion_v2_metal`),
  `float_psnr`, `float_moment`, `integer_psnr`, `float_motion`,
  `integer_motion`, `float_ssim`, `float_ms_ssim`, `integer_ssim`,
  `float_vif`, `integer_vif`, `float_adm`, `integer_adm`,
  `integer_ciede`, `integer_psnr_hvs`, `integer_cambi`, and
  `ssimulacra2`.
  (`float_ansnr` removed from wired set in commit 70ed8b3ce3 / PR #38.)
  All 17 live: each has paired `.metal` kernel, meson entry, registry
  slot, and per-kernel CPU-vs-Metal parity test guarded by
  `core/test/test_metal_kernel_coverage_audit.c`. Only remaining
  Metal-twin gap = SpEED family (`speed_chroma` / `speed_temporal`) —
  has CUDA/SYCL/HIP twins but no Metal kernel yet. Never add new
  `<feature>_metal.mm` without all five: (a) matching `.metal` kernel.
  (b) meson entry in `metal_objcpp_lib`, `<feature>_air`
  `custom_target`, and entry in `metal_air_files`. (c)
  `extern VmafFeatureExtractor` declaration in `feature_extractor.c`
  plus slot in `feature_extractor_list[]` under `HAVE_METAL` block.
  (d) row in
  `g_metal_features[]` in `core/src/metal/dispatch_strategy.c` for
  both registry name and every provided-features key. (e) basename
  entry (bumped `EXPECTED_KERNEL_COUNT`) in
  `test_metal_kernel_coverage_audit.c`.

- **Per-WG partials — no atomics**: Apple MSL does not
  expose `atomic_ulong` (`atomic_fetch_add_explicit` for `ulong`
  silently compiles but fails on device — confirmed CI run
  25685703780 / job 75408804495). Metal kernels use per-threadgroup
  partials indexed by `bid.y * grid_groups.x + bid.x`, reduced on
  host. Never introduce `atomic_ulong` or
  `atomic_fetch_add_explicit` for 64-bit types. Exact 64-bit reductions
  use threadgroup `long` / `ulong` scratch followed by serial lane-0
  sum, as in `integer_vif.metal` and `float_moment.metal`.

- **`simd_sum` reduction is 32-bit only**: MSL `simd_sum()` is
  standard two-level reduction primitive for float / uint values. Those
  kernels use:
  1. `simd_sum(per_thread_val)` → lane 0 of each SIMD group writes to
     `threadgroup float simd_partials[8]` array.
  2. Thread 0 (`lid == 0`) sums `simd_count` SIMD-group partials into
     global `partials[bid.y * grid_groups.x + bid.x]` slot.
  Do not split 64-bit addend into independent lo/hi `simd_sum(uint)`
  calls: carry out of low half is then lost. `float_moment` and
  `integer_vif` therefore use exact serial lane-0 pattern above.

- **8×16 threadgroup / 20×20 shared tile (radius-2 kernels)**:
  `integer_motion_v2`, `float_motion`, `integer_motion`, and
  `float_ssim` all use 16×16 threadgroup with 20×20 shared tile
  (4-element halo radius-2). (`float_ansnr` used same tile layout but
  was removed in commit 70ed8b3ce3 / PR #38.) Tile pitch is 21 (not
  20) to avoid bank conflicts on Apple GPU 32-bank threadgroup memory.

- **Per-WG partials buffer**: each `.mm` allocates Shared-storage
  `MTLBuffer` sized `ceil(W/16) * ceil(H/16)` float (or uint)
  elements, one per threadgroup. `float_moment` is exact-integer
  exception: it has eight uint32 planes holding lo/hi pairs for four
  uint64 sums (ref1st/dis1st/ref2nd/dis2nd). host reconstructs each
  pair before applying bit-depth scaler. Preserve this shape across
  rebases; restoring four interleaved floats reopens BUG-048 A6.

- **Bridge-retained PSO slots**: each `.mm` stores
  `MTLComputePipelineState` handles as `void *` under
  `__bridge_retained` cast (one per bpc variant). `close_fex_metal`
  must release them via `__bridge_transfer` to avoid leaks.

- **`float_moment` feature name correction**: T8-1 scaffold
  `float_moment_metal.c` erroneously listed `{"float_moment1",
  "float_moment2", "float_std", NULL}` as `provided_features`.
  correct names (matching CPU, CUDA, HIP, SYCL, Vulkan) are
  `{"float_moment_ref1st", "float_moment_dis1st",
  "float_moment_ref2nd", "float_moment_dis2nd", NULL}`. `.mm`
  conversion uses correct names; `.c` file removed from
  `metal_sources` on merge.

- **`integer_psnr_metal` option parity (ADR-1322 / BUG-048)**:
  `integer_psnr_metal.mm` must declare `enable_chroma` (default `true`)
  and `uncapped` (default `false`) in its `options[]` table. `init_fex_metal`
  must clamp `n_planes` to 1 when `!enable_chroma` or `pix_fmt == VMAF_PIX_FMT_YUV400P`.
  `submit_fex_metal` and `collect_fex_metal` must loop over `s->n_planes`.
  Guarded by device-free contract test `test_gpu_psnr_option_parity_contract.py`.
