---
paths:
  - core/src/feature/float_adm.c
  - core/src/feature/adm_tools.h
invariant: Float ADM GPU exports, strict division (no reciprocal estimate), min dim 17x17, and signature stability.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Float ADM GPU Exports, Strict Divides, and Bounds

- **Float ADM reference exports for GPU twins** (ADR-1420,
  [`adm_float_reference.h`](../adm_float_reference.h)). `adm_tools.c`
  exports `adm_border_s()`, `adm_csf_rfactor_s()`,
  `adm_pool_bands_s()`, `adm_decouple_cos_1deg_sq_s()`.
  `float_adm_cuda.c` calls them instead of copying. Keep: four
  reductions (`adm_csf_den_scale_s[_p3]`, `adm_cm_s[_p3]`) end in
  `adm_pool_bands_s()`. Twin copies drift: old CUDA copy of
  `dwt_quant_step()` was 1-3 ulp off. Twin types
  that matter: gain limit `double`, `FLOAT_ONE_BY_30` / `_15` double
  literals, threshold centre tap fifth, angle threshold
  `(cos^2 * |o|^2) * |t|^2`, fp32 row + frame accumulators. Change any
  -> change `float_adm_gpu_common.h` (CUDA + HIP twins, ADR-1420 /
  ADR-1458) and
  `sycl/sycl_float_adm_math.h` (ADR-1434, same functions without fp64
  type) same PR (`test_float_adm_device_math` /
  `test_sycl_float_adm_math` fail until they follow). HIP / Metal twins
  still old arithmetic: `T-GPU-FLOAT-ADM-CPU-ARITHMETIC-2026-10-01`.
- **Float ADM SIMD = wavelet + CSF only, same bits as scalar** (ADR-1473).
  `adm.c`: `adm_dwt2_dispatch()` (NEON, AVX2, AVX-512) and
  `adm_csf_plane_select()` -> `adm_csf_planes_s(..., plane)`. `adm_csf_s()`
  = `adm_csf_planes_s(..., adm_csf_plane_s)`; element loop lives in
  `adm_csf_plane_s()`, upstream's three statements verbatim. Decouple,
  denominator reduction, contrast masking: scalar on every processor (fp32
  accumulators in column order are golden-gated reference). New kernel
  -> byte-compare test against `_s` function first, see
  [`../x86/AGENTS.d/float-adm.md`](../x86/AGENTS.d/float-adm.md). Upstream
  sync touching `adm_csf_s()`: port hunk into `adm_csf_plane_s()` /
  `adm_csf_planes_s()`.
- **Float ADM DIVIDES; no reciprocal estimate, ever** (ADR-1442,
  fork-local, diverges from upstream). `adm_options.h`: NO
  `#define ADM_OPT_RECIP_DIVISION`. `adm_tools.c`: one
  `#define DIVS(n, d) ((n) / (d))` + `#error` when macro defined; no
  `rcp_s()`, no `_mm_rcp_ss`, no `<emmintrin.h>`. Reason: upstream
  `t * rcp_s(o)` builds on `RCPSS`, specified by error bound only ->
  scores = property of processor (Zen 5: 2.6M of 8.4M mantissas off
  quotient), different again on MSVC / ARM (never used instruction).
  Quotient = IEEE fp32 everywhere. Upstream sync touching either file:
  keep fork side of both hunks. Twins: correctly rounded fp32 division
  (`__fdiv_rn()` CUDA), no host probe, no table;
  `adm_reciprocal_model.{c,h}` deleted, stay deleted. Golden gate held
  (271 passed). Guards: `test_float_adm_divides_contract.py` (reference,
  every `*float_adm*` file of every backend, `core/src/meson.build`
  flags), `test_float_adm_device_math` (`test_decouple_divides`: four
  inputs where estimate != quotient on Zen 5).
- **`float_adm` refuses frames below 17x17** (fork-local; upstream has no
  check). `float_adm.c::init()` -> `adm_frame_size_check("float_adm", w, h)`
  (`adm_csf_fixed_point.h`, same floor as fixed-point `adm`), before any
  allocation. Reason: scale-3 band = 1 sample below 17 px;
  `adm_cm_thresh3x3_s()` mirror wants index 1, `dwt2_src_indices_1d_s()`
  fourth tap = index -1 = heap read before band buffer (ASan, 8x8).
  Upstream sync touching `float_adm.c::init()`: keep check.
  `float_adm_cuda` has it too; SYCL / HIP / Metal float twins not yet
  (`T-GPU-FLOAT-ADM-TINY-FRAME-FLOOR-2026-10-01`). Guard:
  `test_float_adm_coverage`. Debug key `adm` stays UNSUFFIXED
  (`provided_features` lists `adm_scale0`, upstream parity): Netflix golden
  tests read `VMAF_feature_adm_score` under non-default options; do not
  "fix" list (`T-FLOAT-ADM-DEBUG-KEY-UNSUFFIXED-2026-10-01`).
- **`compute_adm` signature stays on fork's parameter
  list — Strategy E in Research-0024.** Netflix upstream
  `4dcc2f7c` adds 12 new parameters (`luminance_level`,
  `adm_csf_scale`, `adm_csf_diag_scale`, `adm_noise_weight`,
  `adm_bypass_cm`, `adm_p_norm`, `adm_f1s0..3`, `adm_f2s0..3`,
  `adm_skip_aim_scale`, `adm_skip_scale0`) plus new
  `score_aim` output. Threading those through SIMD paths
  (`adm_avx2.c` / `adm_avx512.c` / `adm_neon.c`) **and**
  GPU twins (`adm_vulkan.c` / `adm_cuda.c` / `adm_sycl.cpp`)
  is multi-day work, and new `aim` feature has no fork-
  side golden values yet. **Do not port `4dcc2f7c` until
  there is concrete user demand for `aim` and coordinated
  cross-backend port plan.** See
  [Research-0024 §"Same divergence test for motion + float_adm"](../../../../docs/research/0024-vif-upstream-divergence.md).
