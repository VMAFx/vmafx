<!-- markdownlint-disable MD013 MD060 -->
# `core/src/feature/metal/` — Metal feature-kernel directory

Parent: [../AGENTS.md](../AGENTS.md). Metal backend runtime lives at
[`../../metal/AGENTS.md`](../../metal/AGENTS.md); ADRs governing this
directory listed in "Governing ADRs" section at bottom of this file.

## Purpose

Contains one `.mm` (Objective-C++ host dispatch) + one `.metal` (Metal
Shading Language device kernel) pair per feature extractor in Metal
GPU backend, plus per-T8-1 scaffold `.c` stubs, superseded.

Only `.mm` + `.metal` pairs functional. `.c` stubs (e.g.
`float_psnr_metal.c`) replaced by `.mm` counterparts once real kernel
lands; removed from `metal_sources` in `core/src/metal/meson.build`
when conversion happens.

## Rebase-sensitive invariants

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
  partials indexed by `bid.y * grid_groups.x + bid.x`, reduced on the
  host. Never introduce `atomic_ulong` or
  `atomic_fetch_add_explicit` for 64-bit types. Exact 64-bit reductions
  use threadgroup `long` / `ulong` scratch followed by a serial lane-0
  sum, as in `integer_vif.metal` and `float_moment.metal`.

- **`simd_sum` reduction is 32-bit only**: MSL `simd_sum()` is the
  standard two-level reduction primitive for float / uint values. Those
  kernels use:
  1. `simd_sum(per_thread_val)` → lane 0 of each SIMD group writes to
     `threadgroup float simd_partials[8]` array.
  2. Thread 0 (`lid == 0`) sums `simd_count` SIMD-group partials into
     global `partials[bid.y * grid_groups.x + bid.x]` slot.
  Do not split a 64-bit addend into independent lo/hi `simd_sum(uint)`
  calls: a carry out of the low half is then lost. `float_moment` and
  `integer_vif` therefore use the exact serial lane-0 pattern above.

- **8×16 threadgroup / 20×20 shared tile (radius-2 kernels)**:
  `integer_motion_v2`, `float_motion`, `integer_motion`, and
  `float_ssim` all use 16×16 threadgroup with 20×20 shared tile
  (4-element halo radius-2). (`float_ansnr` used same tile layout but
  was removed in commit 70ed8b3ce3 / PR #38.) Tile pitch is 21 (not
  20) to avoid bank conflicts on Apple GPU 32-bank threadgroup memory.

- **Per-WG partials buffer**: each `.mm` allocates Shared-storage
  `MTLBuffer` sized `ceil(W/16) * ceil(H/16)` float (or uint)
  elements, one per threadgroup. `float_moment` is the exact-integer
  exception: it has eight uint32 planes holding lo/hi pairs for the four
  uint64 sums (ref1st/dis1st/ref2nd/dis2nd). The host reconstructs each
  pair before applying the bit-depth scaler. Preserve this shape across
  rebases; restoring four interleaved floats reopens BUG-048 A6.

- **Bridge-retained PSO slots**: each `.mm` stores
  `MTLComputePipelineState` handles as `void *` under
  `__bridge_retained` cast (one per bpc variant). `close_fex_metal`
  must release them via `__bridge_transfer` to avoid leaks.

- **`float_moment` feature name correction**: T8-1 scaffold
  `float_moment_metal.c` erroneously listed `{"float_moment1",
  "float_moment2", "float_std", NULL}` as `provided_features`. The
  correct names (matching CPU, CUDA, HIP, SYCL, Vulkan) are
  `{"float_moment_ref1st", "float_moment_dis1st",
  "float_moment_ref2nd", "float_moment_dis2nd", NULL}`. The `.mm`
  conversion uses correct names; `.c` file removed from
  `metal_sources` on merge.

- **`integer_psnr_metal` option parity (ADR-1322 / BUG-048)**:
  `integer_psnr_metal.mm` must declare `enable_chroma` (default `true`)
  and `uncapped` (default `false`) in its `options[]` table. `init_fex_metal`
  must clamp `n_planes` to 1 when `!enable_chroma` or `pix_fmt == VMAF_PIX_FMT_YUV400P`.
  `submit_fex_metal` and `collect_fex_metal` must loop over `s->n_planes`.
  Guarded by device-free contract test `test_gpu_psnr_option_parity_contract.py`.

## Kernel files

| File                               | Status      | Feature(s)                                                              |
|------------------------------------|-------------|-------------------------------------------------------------------------|
| `integer_motion_v2.metal`          | Done (T8-1c) | `VMAF_integer_feature_motion_v2_sad_score`, `motion2_v2_score`         |
| `integer_motion_v2_metal.mm`       | Done (T8-1c) | host dispatch                                                           |
| `float_psnr.metal`                 | Done (T8-1d) | `float_psnr`                                                            |
| `float_psnr_metal.mm`              | Done (T8-1d) | host dispatch                                                           |
| `float_moment.metal`               | Done (T8-1e) | `float_moment_ref1st`, `float_moment_dis1st`, `float_moment_ref2nd`, `float_moment_dis2nd` |
| `float_moment_metal.mm`            | Done (T8-1e) | host dispatch (fixes provided_features)                                 |
| `integer_psnr.metal`               | Done (T8-1g) | `psnr_y`, `psnr_cb`, `psnr_cr`                                          |
| `integer_psnr_metal.mm`            | Done (T8-1g) | host dispatch                                                           |
| `float_motion.metal`               | Done (T8-1h) | `float_motion`                                                          |
| `float_motion_metal.mm`            | Done (T8-1h) | host dispatch                                                           |
| `integer_motion.metal`             | Done (T8-1i) | `VMAF_integer_feature_motion_sad_score`, `_motion_score`, `_motion2_score`, `_motion3_score` (ADR-1498) |
| `integer_motion_metal.mm`          | Done (T8-1i) | host dispatch                                                           |
| `float_ssim.metal`                 | Done (T8-1j) | `float_ssim`, `float_ssim_l`, `float_ssim_c`, `float_ssim_s`           |
| `float_ssim_metal.mm`              | Done (T8-1j) | host dispatch                                                           |
| `float_ms_ssim.metal`              | Done (T8-2b) | `float_ms_ssim`, `float_ms_ssim_cb`, `float_ms_ssim_cr` — 5-scale pyramid, Wang weights |
| `float_ms_ssim_metal.mm`           | Done (T8-2b) | host dispatch (ADR-0490, ADR-1334; enable_db, clip_db, enable_chroma)   |
| `integer_ssim.metal`               | Done         | `ssim`                                                                  |
| `integer_ssim_metal.mm`            | Done         | host dispatch                                                           |
| `float_vif.metal`                  | Done         | `VMAF_feature_vif_scale0..3_score`, `vif`, `vif_num/den` (+ per-scale) |
| `float_vif_metal.mm`               | Done         | host dispatch                                                           |
| `integer_vif.metal`                | Done         | `VMAF_integer_feature_vif_scale0..3_score`, `integer_vif` (+ per-scale)|
| `integer_vif_metal.mm`             | Done         | host dispatch                                                           |
| `float_adm.metal`                  | Done         | `VMAF_feature_adm2/aim/adm3/adm_scale0..3_score`, `adm_num/den` (+scale)|
| `float_adm_metal.mm`               | Done         | host dispatch                                                           |
| `integer_adm.metal`                | Done         | `VMAF_integer_feature_adm2/aim/adm3_score`, `integer_adm` (+ per-scale)|
| `integer_adm_metal.mm`             | Done         | host dispatch                                                           |
| `integer_ciede.metal`              | Done         | `ciede2000`                                                             |
| `integer_ciede_metal.mm`           | Done         | host dispatch                                                           |
| `integer_psnr_hvs.metal`           | Done         | `psnr_hvs_y`, `psnr_hvs_cb`, `psnr_hvs_cr`, `psnr_hvs`                  |
| `integer_psnr_hvs_metal.mm`        | Done         | host dispatch                                                           |
| `integer_cambi.metal`              | Done         | `Cambi_feature_cambi_score`                                            |
| `integer_cambi_metal.mm`           | Done         | host dispatch                                                           |
| `ssimulacra2.metal`                | Done         | `ssimulacra2`                                                           |
| `ssimulacra2_metal.mm`             | Done         | host dispatch                                                           |

## Rebase-sensitive invariants (end-of-stream drain + frame-0 motion2)

- **Every Metal extractor MUST set `VMAF_FEATURE_EXTRACTOR_METAL`**
  (`feature_extractor.h`, bit 7). `flush_context_serial`
  (`core/src/libvmaf.c`) drains backend's pending final-frame
  `collect()` *only* when extractor carries its backend flag — the
  `#ifdef HAVE_METAL` branch keys off `VMAF_FEATURE_EXTRACTOR_METAL &&
  gpu_pending`, mirroring CUDA / HIP / SYCL drain blocks. Extractor
  that forgets flag silently drops its last submitted frame's score
  (generic submit/collect double-buffer leaves `collect(N)` pending).
  When adding new `<feature>_metal.mm`, OR the flag into `.flags`
  alongside any feature-class flag (e.g.
  `VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_METAL`).
  All 17 registered Metal extractors set this flag
  (GAP-METAL-DISPATCH-FLAGS-ZERO-MODEL-FALLBACK resolved 2026-09-02;
  all 9 round-3/4 extractors promoted).
- **Frame-0 `motion2` emission contract.** Motion-family Metal
  extractors append `motion2 = 0.0` at index 0, no-op at index 1, and
  `min(prev, cur)` at index − 1 for index ≥ 2 — byte-identical to
  `integer_motion_metal` and HIP / CUDA twins. `float_motion_metal`
  was previously missing index-0 append and double-wrote at index 1
  (fixed fix/metal-drain-motion2, 2026-06-20). Never "simplify" the
  index-0 / index-1 split away.

## Rebase-sensitive invariants (motion_fps_weight)

- **`motion_fps_weight` = CPU's, per frame (ADR-1498)** — see canonical
  invariant note in [`../cuda/AGENTS.md`](../cuda/AGENTS.md).
  `integer_motion_metal`, `motion_v2_metal`: `collect()` stores
  `MIN(sad / 256. / (w * h) * motion_fps_weight, motion_max_val)` (CPU
  `extract()`), `.advance` = `vmaf_motion_window_advance()` per frame and
  `flush()` = `vmaf_motion_window_flush()` for the rest (ADR-2090), no own
  window. `float_motion_metal`: motion / motion2 / debug score =
  `MIN(score * motion_fps_weight, motion_max_val)`. Parity at `==`
  (`test_metal_*_parity`), not places=4.

## Registration coverage invariant

Every new Metal `VmafFeatureExtractor` added to
`core/src/feature/feature_extractor.cpp`'s `feature_extractor_list[]` must add
its basename to `core/test/test_metal_kernel_coverage_audit.c`'s
`g_metal_kernel_basenames[]` and update `EXPECTED_KERNEL_COUNT` in the same
PR. Motion-class extractors must also appear in
`core/test/test_metal_kernel_registration.c`'s `kTemporal[]` table so the
`VMAF_FEATURE_EXTRACTOR_TEMPORAL` scheduling flag is pinned. The runtime-focused
`test_metal_smoke.c` is not the authoritative registration inventory.

The dedicated registration and 17-kernel audit tests supersede the older
per-extractor smoke functions removed by a stale squash; do not duplicate those
lookups back into the runtime test. See Research-2091.

## Per-feature option-table sync invariant

- **`float_ms_ssim_metal` exposes full option and score parity.** ADR-1334
  extends ADR-1221 to resolve `T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07`:
  `float_ms_ssim_metal` exposes `enable_lcs`, `enable_db`, `clip_db`, and `enable_chroma`.
  Scores are emitted via the shared `vmaf_ms_ssim_emit_scores()` (for luma) and
  `vmaf_ssim_emit_score_named()` (for chroma planes `float_ms_ssim_cb` and
  `float_ms_ssim_cr`), passing `s->enable_db, s->max_db`. When `clip_db` is enabled,
  `s->max_db` is derived from the frame geometry via
  `ceil(10. * log10(peak * peak / mse))` with `mse = 0.5 / (w * h)`. The
  framework-free `float_ms_ssim_option_semantics.h` owns active-plane count,
  ceil-subsampled plane geometry, and the dB ceiling so the exact production
  semantics execute on hosts without Metal.
  Subsampled chroma requires at least 176x176 dimensions (351x351 luma for
  YUV420P because allocation uses ceil subsampling), enforced at init. YUV400P
  resolves to one active plane before that chroma check. Every L/C/S atom on
  every active plane is validated before the weighted product;
  `pow(NaN, 0)` must never erase a failed reduction.
  `test_metal_ms_ssim_option_semantics`, `test_metal_ms_ssim_options_contract.py`,
  and `test_nonfinite_collector_wiring.py` lock this contract down device-free.
  The CUDA and HIP twins include the same header for their `enable_chroma`
  plane count and plane sizes (`T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06`):
  a rename or signature change of its helpers changes
  `../cuda/integer_ms_ssim_cuda.c` and `../hip/integer_ms_ssim_hip.c` in the
  same PR.
- **GPU twins must mirror CPU option table for model-configured
  features.** Model (such as default model `vmaf_v1.0.16_3d0h`) may
  provide feature options. `vmaf_use_features_from_model` then checks
  that selected backend's feature extractor supports every option
  present in model's options dictionary. If any option missing from
  GPU twin's `options[]`
  table, dispatch rejects twin and falls back to CPU. For CAMBI
  (`integer_cambi_metal`), `cambi_high_res_speedup` (alias `hrs`,
  default 0, min 0, max 2160) must be present in option table and
  honored during processing (adjusting window size and subsampling
  post-spatial-mask for resolutions >= 1080p, matching `cambi.c`).
- **Never re-declare shared CAMBI constants.** Resolution thresholds
  (`CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1080p` / `_1440p` / `_2160p`) and
  `CAMBI_WINDOW_DIVISOR` live in `core/src/feature/cambi_internal.h`,
  which `integer_cambi_metal.mm` already includes. Metal-local copy
  of shared constant is how twin silently drifts from `cambi.c` on
  next upstream sync.
- **`submit_fex_metal` must leave `s->d_image` / `s->d_mask` /
  `s->d_tmp` pointing at allocations `init_fex_metal` made.** Per-scale
  pipeline rotates three scratch buffers via pointer swaps; every exit
  path restores originals so next frame starts from known state and
  `close_fex_metal` releases handles it owns.
- **`vmaf_use_feature()` consumes option dictionary.** It takes
  ownership of `VmafFeatureDictionary` on every path except
  argument-validation guards, so CPU-vs-Metal parity test must build
  fresh dictionary per call. Sharing one across two runners = a
  use-after-free; freeing it afterwards = double free.

## Governing ADRs

- [ADR-1176](../../../../docs/adr/1176-metal-motion-v2-mirror-closeout.md) — Metal motion_v2 mirror closeout and reflect-101 parity
- [ADR-0490](../../../../docs/adr/0490-float-ms-ssim-metal-port.md) — T8-2b: float_ms_ssim_metal port
- [ADR-0421](../../../../docs/adr/0421-metal-first-kernel-motion-v2.md) — T8-1c through T8-1k batch specification
- [ADR-0420](../../../../docs/adr/0420-metal-backend-runtime-t8-1b.md) — runtime (T8-1b), prerequisite
- [ADR-0361](../../../../docs/adr/0361-metal-compute-backend.md) — scaffold (T8-1), origin
- [ADR-0214](../../../../docs/adr/0214-gpu-parity-ci-gate.md) — `places=4` cross-backend parity gate

## Exact designs of the Metal twins (ADR-1498)

Twins = CPU bits by construction, on the CUDA / HIP / SYCL exact designs. No
Apple device on lanes: arithmetic checked on host only; device = tester report
(ADR-1496). Every `.metal` compiles `-fno-fast-math -ffp-contract=off`
(`metal_shader_strict_fp_args`, one list, `test_metal_shader_build_contract`).

- **Per-sample arithmetic** = header on `metal_portable.h` (MSL + host
  C/C++ subset: values in/out, no ptr/ref param, no double / long long /
  `ULL` / static local / `std::`). Kernel includes it; host test compiles it
  against CPU. Change to the CPU routine -> header, same PR.
- **fp64 in integers** = `metal_soft_double.h` / `metal_soft_signed.h`:
  SYCL `sycl_soft_double.h` / `sycl_soft_signed.h` statement for statement;
  mapping at header top (`vmaf_sycl_soft::f` -> `vmaf_mtl_f`, `S` ->
  `VmafMtlS`, `kX` -> `VMAF_MTL_SOFT_X`). Change one copy -> other, same PR
  (RC5 row `T-METAL-SYCL-FP64-FREE-ARITHMETIC-COPIES-2026-10-03`). Metal
  masks shift counts to 6 bits -> keep every `shift >= 64` guard. Guards:
  `test_metal_soft_double`, `test_metal_soft_double_contract.py`.
- **float_psnr**: term = `vmaf_mtl_fpsnr_term()` (fp32 product of raw diff,
  uint32); group sum ulong in threadgroup memory (no `simd_sum`: excludes
  64-bit); threadgroup = 256 pixels of one row; host
  `vmaf_float_psnr_row_noise()` (`float_psnr_rows.h`, ADR-1499): segments
  exact, rows into double in order. Never one frame total rounded once.
- **float_moment**: 10/12/16-bit kernel adds `vmaf_mtl_moment_float_square()`,
  never `rv * rv`; host uint64 sum. Past 2^53 units (ADR-1497):
  `metal_float_moment_sum.h` = statement-for-statement copy of
  `float_moment_sum.h` + four `ordered_sum.h` functions, address spaces via
  `VMAF_MTL_DEV` / `VMAF_MTL_TG` / `VMAF_MTL_THR`. Change to either shared
  header -> copy, same PR (`test_metal_float_moment_exact_contract.py`,
  `test_float_moment_sum_contract.py`). Five kernels, one wait; never a device
  reduction for the walk; never trust the plan. `.mm` takes `sums[2..3]` from
  the walk, fails closed. Guard: `test_metal_float_moment_sum`.
- **integer ADM**: decouple = `metal_integer_adm_math.h`: reciprocal = CPU
  integer `2^30 / o` (never fp32 quotient), gain limit =
  `adm_gain_limit_product()` of shared `adm_gain_limit.h` (Metal guard keeps
  other `-E` byte-identical; keep line count). Uniforms, slots, host logic:
  see `integer_adm_metal`: host logic section below.
- **integer motion**: diff first (`metal_integer_motion_math.h`), raw ring 2
  (3 with five-frame window) slots, `collect()` = CPU SAD score first in
  `provided_features`, table = `integer_motion.c`'s. No `motion_add_uv`, no
  `motion_y_score`.
- **psnr**: exact uint64 SSE per threadgroup (lid 0 serial sum); host CPU MSE
  expr + `psnr_score.h`; `flush()` = apsnr; TEMPORAL | METAL; chroma by pixel
  format.
- **integer vif**: min dim 16 from `vif_filter1d_width`; `check_context` ->
  CPU `vif`; init -EINVAL before device work; border =
  `vmaf_mtl_vif_mirror()` (fold 2*(sup-1)), never single bounce.
- **float_motion**: blur stores |cur-prev| transposed (64-row groups),
  `float_motion_row_sum` one thread/row left-to-right fp32, host
  `vmaf_float_motion_score_from_row_sads()`; every per-sample op in
  `metal_float_motion_math.h`; motion3 = `motion_blend()` of motion2,
  `mbf` / `mbo` in CPU order; table = `float_motion.c`'s (nine options, order
  spells feature names).
- **ciede**: `ciede_ff_math.h` `pixel()` on `metal_ciede_math.h` primitives;
  one float/pixel, host `ciede_frame_sum()`; constants `make_constants(bpc)`
  on host. Shared `ff_pair.h` / `ff_math.h` / `ciede_ff_math.h`
  `VMAF_FF_MSL_SUBSET` branches: edit only with byte-identical `-E -P` proof on
  SYCL + HIP TUs; keep `#endif` directly above `#include "ff_math.h"`.
- **cambi table**: = CPU's, `heatmaps_path` included. Heatmaps only through
  `vmaf_cambi_open_heatmaps()` / `_dump_c_values()` / `_close_heatmaps()`
  (`cambi.c`'s writers): dump in `cambi_metal_scale_host()` before
  `vmaf_cambi_spatial_pooling()` (quick-select reorders c-values), distorted
  pass only, adjusted encode window, frame = `index`. Helpers from
  `cambi_internal.h` only, no copies.
- **Names before option slots (every twin)**: `init()` builds
  `feature_name_dict` before it or any function it calls first writes an option
  slot (`cambi.c::init` order). cambi: `cambi_metal_resolve_dimensions()` writes
  `enc_*` / `src_*` (FEATURE_PARAM, default 0); dict after it = every name
  suffixed `_encbd_8_ench_..._srcw_...`, gate / model / parity test find no
  score (T-METAL-CAMBI-SCORE-NAME-SUFFIXED-2026-10-05, M4 Pro #2118). Guard:
  `test_metal_twin_option_tables_contract.py` (`_name_order_failures`).
- **float_adm**: `adm_frame_size_check()` first in `init()`. Per-sample math
  only in `metal_float_adm_math.h` (= `sycl_float_adm_math.h` method,
  ADR-1434): decouple quotient plain fp32 `/`, never reciprocal; three fp64
  expressions = exact fp32 pairs + integer replay. One fp32 row sum per thread,
  host folds rows in fp32; no simd / threadgroup / atomic sum of terms. Host
  takes rfactor (every `adm_fNsM`), border, pool, cos from
  `adm_float_reference.h`; no copies. Frame-sum floor = `adm.c` expression.
  `adm_csf_mode` default-only (ADR-1316). Change to `adm_decouple_s()`,
  `adm_csf_s()`, `adm_cm_thresh3x3_s()`, `adm_csf_den_scale_s()`, `adm_cm_s()`
  -> this header + SYCL + CUDA headers, same PR. Guards:
  `test_metal_float_adm_math` (x86_64), `test_metal_float_adm_exact_contract.py`.
- **float_vif**: `metal_float_vif_math.h` = `sycl_float_vif_math.h` statement
  for statement (ADR-1422). Taps = kernel argument from `vif_get_filter()`
  (every kernelscale, full range), log2 polynomial as bit patterns,
  `vif_sigma_nsq` as pair + replay, one thread per row, host fp32 row fold,
  prescale on host via CPU scaler. Banned: tap literal, device log2, fp32
  sigma_nsq, simd / threadgroup / atomic term sum, any `DEFAULT_ONLY`. Guards:
  `test_metal_float_vif_math`, `test_metal_float_vif_exact_contract.py`.
- **integer vif gain**: `metal_integer_vif_gain.h` = `sycl_integer_vif_math.h`
  (ADR-1432): integer division + soft-double replay; no fp32 gain or clamp, no
  64-bit mulhi. Limit reaches kernels as `VmafMtlGainLimit` (32 bytes,
  `vmaf_mtl_ivif_make_gain_limit()`). `sv_sq` = `vif_sv_sq()`'s value
  (ADR-1561: x86's 0 below 0, defined everywhere). Change to
  `vif_accumulate_pixel()` lines of `integer_vif.c` / `x86/vif_avx2.c` /
  `x86/vif_avx512.c` -> header, same PR. Guards: `test_metal_integer_vif_gain`, `..._gain_contract.py`.
- **integer vif host tail**: `scale_num_den()` rounds each scale's num / den
  to float (`vif_store_residuals()`); `collect_fex_metal()` score set =
  `write_scores()`: `.single_precision_ratio = true`, frame sums add the
  rounded values. Double quotient = every score off by up to half an fp32
  step (M4 Pro, #2118). Guard: `test_sycl_vif_float_sums_contract.py` (every
  integer VIF twin).
- **integer ssim**: `metal_integer_ssim_math.h` = `sycl_integer_ssim_math.h`
  (ADR-1443); terms stored unreduced, host adds in `calc_ssim()` raster order.
- **float_ssim / float_ms_ssim**: window terms = `metal_ssim_terms.h` (CPU fp32
  window values, fp64 quotients on soft-signed integers, no forced 1); every
  term stored at raster position, host adds per plane / scale in
  `iqa_ssim()` order, rounds mean to fp32. ms_ssim decimation =
  `metal_ms_ssim_math.h`, one explicit fma per tap in CPU order. Guards:
  `test_metal_{integer,float,float_ms}_ssim_math` + exact contracts.
- **psnr_hvs** (ADR-1397 / ADR-1401 design): per-block math only in
  `metal_psnr_hvs_math.h` (variance ratio, `od_bin_fdct8()`, energy, threshold
  = fp32 product + Metal `sqrt`, term from integer diff). Kernel stores all 64
  terms per block, no device sum; host `vmaf_psnr_hvs_plane_score()` /
  `_combined_score()` / `_score_db()`. Mask table = host
  `vmaf_psnr_hvs_mask_value()` (double product -> float), buffer 6; never an
  fp32 `csf * 0.3885746225901003f` square. CSF tables = `vmaf_mtl_hvs_csf`.
  Change to `calc_psnrhvs()` -> header, same PR. Guards:
  `test_metal_psnr_hvs_math`, `test_psnr_hvs_twin_exact_sum_contract.py`.
- **MSL names**: no identifier `half`, `device`, `thread`, `constant`,
  `kernel`, ... in any Metal source or included header (C accepts, MSL does
  not; `test_metal_shader_build_contract`).

## motion3_v2 cross-twin invariant (ADR-1108)

- `integer_motion_v2_metal` and `integer_motion_metal` derive `motion2` /
  `motion3` with the CPU's own `vmaf_motion_window_advance()` (each frame
  once its window is complete, registered `.advance`, state
  `s->window_state`) and `vmaf_motion_window_flush()` (the rest) (blend,
  clip, seed, moving average, five-frame window; ADR-2090), never a copy.
  One-frame input -> 0 / 0 as CPU. No device here: compile via the macOS CI
  job. Guard: `test_metal_motion_v2_exact_contract.py`,
  `test_metal_integer_motion_exact_contract.py`,
  `test_motion_window_advance_contract.py`.

## mv2_mirror cross-twin invariant (ADR-1176)

- **mv2_mirror is reflect-101, identical across backends**:
  `integer_motion_v2.metal::mv2_mirror` uses iterated reflect-101
  `idx = (idx < 0) ? -idx : 2 * (sup - 1) - idx`, bit-identical to CPU
  `integer_motion_v2.c::mirror`, CUDA `cuda_tile_index.h::vmaf_cuda_reflect_101`
  (then `vmaf_cuda_tile_index()` clamp, identity for consumed samples, ADR-1372),
  SYCL `integer_motion_pipeline_sycl.cpp::reflect_101`, and HIP
  `motion_v2_score.hip::mv2_mirror`. Never revert to single-bounce or
  `- 1` edge-replicating form.

## CAMBI: use the shared TVI helper and the CPU's border rules (ADR-1219)

Three exact-logic traps, all of which HIP twin fell into and which
together collapsed its CAMBI score to **exactly 0.0** on banding
content CPU scores at 5.85.

1. **Call `vmaf_cambi_init_tvi_and_vlt()`; never re-derive TVI
   table.** It runs CPU's own bisection of
   `tvi_hard_threshold_condition` between `luma_range.foot` and
   `luma_range.head - diff - 1`, plus `vlt_luma` and derived-band
   validation. Two independent hand-ports (HIP and Metal) both
   searched *negated* predicate seeded from luma 0, giving
   `tvi_for_diff = [1026, 1025, 1024, 4]` against CPU's
   `[182, 309, 436, 563]`. Host-side scalar work done once in
   `init()`, so per-backend copy buys nothing.

2. **`cambi.c::filter_mode` leaves output rows 0 and `height-1`
   UNFILTERED.** Its vertical writeback is under `if (i > 1)` and
   covers rows `1 .. height-2`; horizontal results for border rows
   live only in 3-row ring and are never written back. Kernel guard
   is `if (axis == 1 && (y == 0 || y >= height - 1)) return;` — V
   pass writes into buffer that still holds pre-filter image, so
   returning early preserves original pixels exactly.

3. **`get_spatial_mask_for_index()` ZERO-PADS its 7x7 box sum.**
   Summed-area table is `memset` to zero and gated by
   `deriv_valid = (i < height)`, so out-of-frame tap adds nothing.
   Clamping taps to border pixel counts its zero-derivative flag up
   to three extra times per axis and flips `box_sum > mask_index` on
   band of border pixels.

CAMBI parity fixture must band for real: CAMBI counts neighbour
differences of `1 .. num_diffs` (4 at default), so 8-bit gradient
stepping 32 levels every 32 columns scores 0.0 on CPU too and makes
assertion `0 == 0`. Use 10-bit gradient of one level every two
columns inside TVI band (200..900) and assert CPU score is
non-degenerate first.

## float_adm options must reach the kernels (ADR-1220)

`adm_p_norm` (`apn`), `adm_bypass_cm` (`bcm`) and `adm_skip_scale0`
(`ssz`) are `VMAF_OPT_FLAG_FEATURE_PARAM` options `float_adm_metal.mm`
declares. Until ADR-1220, kernels hardcoded cube sum. Host pooling
hardcoded `1.0f / 3.0f` root. `bcm` was read by nothing. `ssz` zeroed
only reported `adm_scale0` sub-score while still folding scale 0 into
pooled `adm2` / `aim`.

Invariants:

- `FadmCsf` in `float_adm.metal` and `FadmCsfHost` in
  `float_adm_metal.mm` must stay byte-identical. Two former padding
  slots now carry `p_norm` (float) and `bypass_cm` (uint), so size
  and alignment unchanged; new option goes into **both** structs in
  same commit.
- `adm_p_norm` has four application points (DLM sum, CSF sum, pooling
  root, `get_noise_constant`); keep CPU's `p == 3` literal-cube fast
  path so default path cannot move.
- `adm_bypass_cm` gates BOTH DLM and AIM `adm_cm()` call — `adm.c`
  passes it to each.
- `adm_skip_scale0` is POOLING rule: `num_scale = 0`,
  `den_scale = 1e-10` for scale 0. No kernel change needed, because
  `adm_dwt2_lo_s` writes only `band_a`, which `adm_dwt2` computes
  identically.

No Apple hardware in dev fleet, so these asserted by construction
against CPU reference and CUDA twin rather than measured. Treat any
Metal float-ADM change as unverified until someone runs
`test_metal_float_adm_parity` on real silicon.

## Integer ADM 16-bit vertical DWT sums in 64 bits (T-GPU-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18)

- `integer_adm.metal`, raw vertical DWT kernel: `accum_lo` / `accum_hi` are
  `long`. A 16-bit low-pass sum passes `INT_MAX` once three samples reach
  42456; the CPU forms it in int64 (`adm_dwt2_vpass16_tap4()`).
- Unverified on Apple silicon (no hardware in the fleet); the change mirrors
  the CUDA and HIP twins, which are measured byte-identical.

## float_motion force-zero ownership and flush idempotency (BUG048 A5)

- `init_fex_metal()` releases the device lifecycle before returning from the
  `motion_force_zero` path, but the cloned extractor still owns its
  `feature_name_dict`. Keep `close_fex_metal` (or an equivalent dictionary-owning
  callback) installed; restoring `fex->close = NULL` leaks the dictionary.
- Before appending the tail `VMAF_feature_motion2_score`, `flush_fex_metal()`
  resolves the actual score name through `feature_name_dict` and probes that
  name at `s->frame_index`. A literal-name probe misses option-derived names such as
  `motion_fps_weight=1.5` and makes a repeated flush fail.
- `collect_fex_metal()` gates emission of `VMAF_feature_motion_score` behind
  `if (s->debug)`.
- `test_metal_float_motion_parity` exercises both lifecycle and flush idempotency
  invariants on Apple Silicon, and skips cleanly on Linux/Windows.
- `test_metal_float_motion_contract.py` enforces struct fields, option
  registrations, close callback retention, debug gating, and dictionary-resolved
  flush idempotency at AST level. See [Research-2113](../../../../docs/research/2113-metal-float-motion-lifecycle-flush.md).

## `integer_adm_metal`: host logic, slots, scale-1 parent (ADR-1806)

T-METAL-INTEGER-ADM-TWIN-DEFECTS-2026-10-05: M4 Pro report (#2118) = every
exact case off. Six defects, each now one definition:

- **Uniforms + slots = `metal_integer_adm_uniforms.h`** (MSL + C). `IadmDims`,
  `IadmCsf` defined once; kernel and host include it. Reduction slot address
  only via `vmaf_mtl_iadm_accum_word()` (kernel wrote stride 36, host read 18:
  band d lost, out-of-bounds writes). No second struct copy in `.mm`/`.metal`.
- **Host logic = `integer_adm_metal_host.c`** (plain C, no Metal API): geometry,
  buffer sizes, stage plan (`iadm_metal_stages()`), uniforms, scores. `.mm` =
  alloc, bind, encode, emit only. Uniform shifts/rounding copied from CPU
  contexts (`adm_cm_ctx_init()`, `i4_adm_cm_ctx_init()`,
  `adm_csf_den_ctx_init()`, `i4_adm_csf_den_ctx_init()`, `i4_dwt2_round()`);
  scores = `adm_cm_result()` / `adm_csf_den_result()` + `i4_` forms, double
  noise weight. No local table, no local quant step, no local `powf`.
- **Scale 1 reads int16** (`integer_adm_dwt_vert_s1`, CPU `i16_to_i32()`);
  scales 2-3 int32 (`integer_adm_dwt_vert_s123`). Never bind the int16 band to
  an int kernel.
- **Scales 1-3 masking terms** = `vmaf_mtl_iadm_i4_masking_term()` with
  `I4AdmCmCtx::add_bef_shift_flt` = INT32_MIN (Netflix#955, ADR-0155), never
  +2^31. Denominator square add = `I4AdmDenCtx::add_shift_sq` = 2^shift_sq.
- **`adm_skip_scale0`**: scale 0 = DWT only, num 0, den 1e-10f, AIM 0.
- **`kernel` is a macro in the host shim**: no host-header identifier named
  `kernel` (stage field = `entry`); no MSL type names (`half`) in headers the
  kernels include (`test_metal_shader_build_contract`).
- Guards: `test_metal_integer_adm_host_replay` (unmodified `.metal` through
  `core/test/metal_msl_host_shim.h`, `==` vs CPU, guard bands),
  `test_metal_integer_adm_math`, `test_metal_integer_adm_exact_contract.py`.
  Device: `test_metal_integer_adm_parity`. Replay limits: one thread per
  threadgroup (no barrier/race coverage), not the Metal compiler.

## `float_adm_metal.mm`: CSF weights from the CPU (ADR-1489, ADR-1498)

- No copy of `dwt_quant_step()` since ADR-1498: weights =
  `adm_csf_rfactor_s()` (`adm_float_reference.h`) with every CPU option,
  `adm_f1sN` / `adm_f2sN` included. Never bring a local step back. Guard:
  `core/test/test_float_adm_csf_upstream_contract.py`.
