---
paths:
  - core/src/feature/metal/*.mm
  - core/src/feature/metal/metal_*.h
invariant: exact designs of Metal twins (ADR-1498): fp32 pairs, integer fp64, CPU summation order.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Exact designs of the Metal twins (ADR-1498)

Twins = CPU bits by construction, on CUDA / HIP / SYCL exact designs. No
Apple device on lanes: arithmetic checked on host only; device = tester report
(ADR-1496). Every `.metal` compiles `-fno-fast-math -ffp-contract=off`
(`metal_shader_strict_fp_args`, one list, `test_metal_shader_build_contract`).

- **Per-sample arithmetic** = header on `metal_portable.h` (MSL + host
  C/C++ subset: values in/out, no ptr/ref param, no double / long long /
  `ULL` / static local / `std::`). Kernel includes it; host test compiles it
  against CPU. Change to CPU routine -> header, same PR.
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
  `test_float_moment_sum_contract.py`). Five kernels, one wait; never device
  reduction for walk; never trust plan. `.mm` takes `sums[2..3]` from
  walk, fails closed. Guard: `test_metal_float_moment_sum`.
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
  `feature_name_dict` before it or any function it calls first writes option
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
  `write_scores()`: `.single_precision_ratio = true`, frame sums add
  rounded values. Double quotient = every score off by up to half fp32
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
  `vmaf_psnr_hvs_mask_value()` (double product -> float), buffer 6; never
  fp32 `csf * 0.3885746225901003f` square. CSF tables = `vmaf_mtl_hvs_csf`.
  Change to `calc_psnrhvs()` -> header, same PR. Guards:
  `test_metal_psnr_hvs_math`, `test_psnr_hvs_twin_exact_sum_contract.py`.
- **MSL names**: no identifier `half`, `device`, `thread`, `constant`,
  `kernel`, ... in any Metal source or included header (C accepts, MSL does
  not; `test_metal_shader_build_contract`).

- **Host-only equality and by-pointer structs in shared headers (CodeQL sweep, 2026-10-06).**
  `vmaf_mtl_f64_equal()` (`metal_portable.h`, host branch) is `==` for doubles spelled with
  `isless` / `isgreater` / `isunordered`: same answer for every input, +0 equals -0, NaN equals nothing.
  Spelled through `VMAF_MTL_ISLESS` / `_ISGREATER` / `_ISUNORDERED` = `__builtin_*` where compiler has
  them: icx's C `<math.h>` maps `<math.h>` macros to libimf calls, which strict-FP link drops
  (`-no-intel-lib=libimf`); three C tests did not link in oneAPI build. MSVC keeps `<math.h>`.
  host-only gain-limit builders (`metal_float_adm_math.h`, `metal_integer_vif_gain.h`) use it instead of `==`
  (`cpp/equality-on-floats`); do not replace it by bit compare (it must keep reference's `==` semantics)
  or tolerance. `vmaf_mtl_fm_blur()` takes its 100-byte window by `const VMAF_MTL_FM_THR` pointer
  (`thread` under Metal, empty on host; `cpp/large-parameter`); `float_motion.metal` passes `&win`.
