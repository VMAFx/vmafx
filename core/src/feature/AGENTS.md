<!-- markdownlint-disable MD060 -->
# AGENTS.md — core/src/feature

Orientation for agents working on feature extractors (the VMAF metric
components: VIF, ADM, motion, integer-valued VIF/ADM/motion, CIEDE, CAMBI,
PSNR, SSIM, MS-SSIM, LPIPS, …). Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

Every VMAF "feature" is a small C module with a `VmafFeatureExtractor`
registration:

```text
feature/
  feature_extractor.cpp/.h   # the registry + lifecycle contract (init/extract/flush/close)
  feature_collector.c/.h     # per-frame score aggregator
  vif.c / adm.c / …          # scalar CPU reference implementations
  integer_*.c                # integer-math reference implementations
  feature_lpips.c            # DNN-backed extractor (opens vmaf_dnn_session_*)
  feature_dists.c            # DISTS-Sq DNN-backed extractor (LPIPS-shaped ABI)
  x86/                       # AVX2 / AVX-512 SIMD paths — must match scalar bit-for-bit
  arm64/                     # NEON SIMD paths — must match scalar bit-for-bit
  cuda/                      # CUDA kernels + launchers
  sycl/                      # SYCL kernels (DPC++)
  common/                    # cross-arch helpers
```

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **Bit-exactness with the scalar reference** is non-negotiable for SIMD
  paths. Reductions, FMA-ordering, and rounding must match the scalar path
  exactly — no "close enough". See
  [add-simd-path](../../../.claude/skills/add-simd-path/SKILL.md) for the
  dispatch pattern (`cpu.c` + feature_name_avx2.c + feature_name_avx512.c).
- **CUDA / SYCL kernels** should match the CPU reference within the
  documented tolerance. If a kernel cannot match exactly, file a snapshot
  justification in the commit message and regenerate
  `testdata/scores_cpu_*.json` via
  [`/regen-snapshots`](../../../.claude/skills/regen-snapshots/SKILL.md).
- **Registration is discoverable by both name and provided-feature-name**:
  `vmaf_get_feature_extractor_by_name()` and
  `vmaf_get_feature_extractor_by_feature_name()`. Both must resolve.
  - **GPU/Metal twins live in `feature_extractor.cpp`'s `#if HAVE_*`
    blocks, NOT in a parallel file.** The registry was `feature_extractor.c`
    until PR #875 introduced the compiled `.cpp` twin; for a window both
    files existed and diverged, and the `speed_{chroma,temporal}_{cuda,
    sycl,hip}` registrations were left behind in the dead `.c` — so the
    kernels compiled but `by_name("speed_chroma_cuda")` returned NULL and
    SpEED silently fell back to CPU. The `.c` is now deleted; when you add
    a GPU twin, add its `extern` + array entry to the matching `#if HAVE_*`
    block in the `.cpp` AND assert resolution in
    `test/test_feature_extractor.c`. A registered-but-unresolvable twin is
    a silent correctness bug, not a build error.
- **Options tables** must have non-NULL `help` for every entry; see
  [../../test/test_lpips.c](../../test/test_lpips.c) for the unit-test
  pattern that enforces this.
- **DNN-backed extractors** open sessions through
  [src/dnn/](../dnn/AGENTS.md) — never call ONNX Runtime directly from a
  `feature_*.c` file.

## Workflows

| Task | Skill |
| --- | --- |
| Add a feature extractor | [add-feature-extractor](../../../.claude/skills/add-feature-extractor/SKILL.md) |
| Add a SIMD path | [add-simd-path](../../../.claude/skills/add-simd-path/SKILL.md) |
| Cross-backend diff | [cross-backend-diff](../../../.claude/skills/cross-backend-diff/SKILL.md) |
| Profile a hot path | [profile-hotpath](../../../.claude/skills/profile-hotpath/SKILL.md) |

## Rebase-sensitive invariants

- `ssimulacra2.c` is fork-local (not upstream). It embeds several
  constant tables that must stay in lock-step with libjxl even across
  a rebase:
  - **Opsin absorbance matrix** (`kM00`…`kM22`) and bias `kB` — see
    libjxl `lib/jxl/opsin_params.h`.
  - **`MakePositiveXYB` offsets** — `B=(B-Y)+0.55`, `X*=14`, `X+=0.42`,
    `Y+=0.01`.
  - **108 pooling weights (`kWeights[]`)** and the final polynomial
    transform (`0.9562382…`, `2.326765…`, `-0.0208845…`,
    `6.2484966e-05`, `0.6276336…`) — from `tools/ssimulacra2.cc`.
  - **FastGaussian coefficient derivation** — `3.2795·σ + 0.2546`
    radius, k∈{1,3,5}, Cramer's-rule 3×3 solve for β. Any drift from
    libjxl's `lib/jxl/gauss_blur.cc` formulas breaks bit-exactness of
    the scalar blur.
  If libjxl changes any of these upstream, update the scalar extractor
  in the same PR (same for the SIMD follow-ups, which will mirror the
  same coefficient path).
- **CIEDE chroma-upsample subsample flags (FORK DIVERGES FROM
  UPSTREAM, 2026-06-27)**: `ciede.c` `scale_chroma_planes` /
  `scale_chroma_planes_hbd` must key the *horizontal* sample-index
  divisor off `ss_hor` and the *vertical* row advance off `ss_ver`.
  **Upstream Netflix carries these two flags transposed** (horizontal
  off `ss_ver`, vertical off `ss_hor`) — that bug heap-OOB-reads and
  mis-scores YUV422P (half-width / full-height chroma). YUV420P is a
  no-op (both flags set) and YUV444P never calls the function, so the
  Netflix golden CIEDE2000 pair (420P) cannot detect a regression
  here. On rebase, do NOT let an upstream sync revert the flags back to
  the transposed form. Guarded by `test_ciede_scale_chroma_422_8b` and
  `test_ciede_scale_chroma_422_16b` in `core/test/test_ciede.c` (both
  fail against the upstream form).
- **integer SSIM `samplemax²` must be widened to `double` (16-bpc
  parity, 2026-06-27)**: `integer_ssim.c` `ssim_reduce_row_range`
  computes `c1/c2 = sm * sm * K * w_d²` via a hoisted
  `const double sm = (double)samplemax;`. It must NOT regress to the
  `int samplemax * samplemax` form: for 16-bpc `samplemax = 65535` and
  `65535² > INT_MAX` overflows `int` (UB, wraps negative), corrupting
  the stability constants and diverging from the CUDA/HIP/SYCL twins
  (which already use `int64_t`/`double`). 8/10/12-bpc are bit-unchanged
  by the cast. Guarded by `test_ssim_16bit_distorted_in_range` in
  `core/test/test_ssim_coverage.c` (fails against the `int` form).
- **MS-SSIM decimate LPF coefficients**: the 9-tap 9/7 biorthogonal
  filter table (`ms_ssim_lpf_h` / `ms_ssim_lpf_v`) appears verbatim in
  four TUs that must stay byte-identical for the bit-exactness
  contract — `ms_ssim_decimate.c`, `x86/ms_ssim_decimate_avx2.c`,
  `x86/ms_ssim_decimate_avx512.c`, and
  `arm64/ms_ssim_decimate_neon.c`. The source of truth upstream is
  `g_lpf_h` / `g_lpf_v` in `ms_ssim.c`. If a rebase touches any of
  those five files, diff all five against each other before pushing.
  See [ADR-0125](../../../docs/adr/0125-ms-ssim-decimate-simd.md).
- **KBND_SYMMETRIC mirror**: `ms_ssim_decimate_mirror` is duplicated
  across the same four TUs and must match the upstream
  `KBND_SYMMETRIC` branch in `iqa/convolve.c`. Changing the boundary
  semantics in any one of them breaks bit-identity.
- **MS-SSIM decimate `-ENOMEM` contract** (ADR-0877, 2026-05-30): the
  `malloc`-failure branch in all four MS-SSIM decimate TUs returns
  `-ENOMEM` (negative POSIX errno), matching the libvmaf internal
  convention documented in `ms_ssim_decimate.h`. On rebase, if the
  upstream port re-introduces a `return -1` here, restore the
  `-ENOMEM` form (and the `#include <errno.h>`) — caller
  `ms_ssim.c:207-208` uses a truthy check so either compiles, but the
  errno form is required by ADR-0877 for higher-level error reporters
  (Go controller, MCP server, ffmpeg filter) that surface the cause.
- **SSIM / MS-SSIM SIMD bit-exactness invariants** (fork-local,
  ADR-0138 + ADR-0139 + ADR-0140): the AVX2 / AVX-512 / NEON paths
  in `x86/ssim_avx2.c` / `x86/ssim_avx512.c` /
  `arm64/ssim_neon.c` / `x86/convolve_avx2.c` /
  `x86/convolve_avx512.c` / `arm64/convolve_neon.c` are
  bit-identical to the scalar reference under FLT_EVAL_METHOD == 0.
  Two rules are load-bearing and must be preserved on rebase:
  1. **Convolve taps**: each tap is *single-rounded `float * float`
     → widen to `double` → `double` add*. No FMA. Mirrors scalar
     `sum += img[i] * k[j]` in
     [`iqa/convolve.c`](iqa/convolve.c). Changing scalar to `fmaf`
     or to a double-mul pattern requires matching all three SIMD
     variants.
  2. **SSIM accumulate**: the `2.0 *` literal in
     [`ssim_accumulate_default_scalar`](iqa/ssim_tools.c)
     (`2.0 * ref_mu[i] * cmp_mu[i] + C1` and
     `2.0 * srsc + C2`) is a C `double` literal, which promotes
     the float operands to double before the multiply. All three
     SIMD accumulators do the `2.0 *` numerator + division + final
     `l*c*s` product per-lane in scalar double to match. If
     upstream ever changes the `2.0` literal to `2.0f` (or
     restructures the l/c numerators), all three SIMD variants
     need a matching rewrite.
  3. **AVX-512 vector-double per-lane reduction**: the AVX-512
     accumulator (`x86/ssim_avx512.c`) computes `lv`, `cv`, `sv`,
     and `lv*cv*sv` lane-wise in two 8-wide `__m512d` passes via
     `_mm512_cvtps_pd` widening + plain `_mm512_mul_pd` /
     `_mm512_add_pd` / `_mm512_div_pd` (no `_mm512_fmadd_pd`),
     then spills to `_Alignas(64) double[16]×4` and accumulates
     left-to-right scalar into `local_*`. The vector-double form
     is bit-identical to scalar lane-wise by IEEE-754, but only
     because: (a) op order matches scalar's parse — `((2*rm)*cm
     /l_den`, etc.; (b) no FMA contraction; (c) the running
     sum stays scalar left-to-right, lane 0 → lane 15. Tree
     reductions over the 16-lane block break ADR-0139's
     running-sum invariant against scalar and are forbidden
     unless scalar itself is rewritten in lockstep. AVX2 and NEON
     stay on the per-lane scalar path (`ssim_accumulate_lane`)
     for now — vectorising them with the same `__m256d` /
     `float64x2_t` widening would follow the same three rules.
- **`simd_dx.h` DX macros** (fork-local, ADR-0140): the header
  [`simd_dx.h`](simd_dx.h) is fork-internal and has no upstream
  equivalent. On rebase, keep the fork's version. The macros
  (`SIMD_WIDEN_ADD_F32_F64_*`, `SIMD_ALIGNED_F32_BUF_*`,
  `SIMD_LANES_*`) encode ADR-0138 / ADR-0139 bit-exactness patterns
  by construction — changing their expansion without auditing the
  three SSIM / convolve consumers (`ssim_accumulate_*`,
  `iqa_convolve_*`) is a bit-exactness break waiting to happen.
  Macro names are ISA-suffixed on purpose; do not collapse them
  into cross-ISA aliases (the fork's SIMD policy rules out
  Highway / simde / xsimd — see user memory
  `feedback_simd_dx_scope.md`).
- **`feature_collector.c` mount/unmount traversal**: the fork rewrites
  `vmaf_feature_collector_mount_model` and `unmount_model` to walk a
  local cursor instead of advancing the pointer-to-head — upstream
  [Netflix#1406](https://github.com/Netflix/vmaf/pull/1406) is still
  OPEN as of 2026-04-20 and its body corrupts the list on ≥3 mounted
  models. `unmount_model` additionally returns `-ENOENT` (not
  `-EINVAL`) for "model not mounted". If upstream ever merges #1406,
  **keep the fork's version on conflict** — the traversal is correct
  and the errno split lets callers distinguish misuse from not-found.
  Test coverage in [`../../test/test_feature_collector.c`](../../test/test_feature_collector.c)
  uses the shared `load_three_test_models` / `destroy_three_test_models`
  helpers; upstream's PR inlines 60 LoC of per-model scaffolding that
  would trip clang-tidy `readability-function-size` (JPL-P10 rule 4).
  See [ADR-0132](../../../docs/adr/0132-port-netflix-1406-feature-collector-model-list.md)
  and [rebase-notes 0031](../../../docs/rebase-notes.md).
- **Generalised AVX convolve scanline helpers** (fork-local,
  ADR-0143): the four `convolution_f32_avx_s_1d_*_scanline`
  helpers in [`common/convolution_avx.c`](common/convolution_avx.c)
  are `static` in the fork (upstream leaves them extern out of
  habit). Strides are `ptrdiff_t` inside helpers, `int` at the
  public `convolution_f32_avx_*_s` wrappers, with `(ptrdiff_t)`
  casts at pointer-offset multiplication sites. On rebase: keep
  the fork's `static` and `ptrdiff_t` unless upstream adopts them.
  See [ADR-0143](../../../docs/adr/0143-port-netflix-f3a628b4-generalized-avx-convolve.md)
  and [rebase-notes 0036](../../../docs/rebase-notes.md).
- **`motion_v2` public option-surface duplicates motion v1**
  (fork-local, ADR-0337):
  [`integer_motion_v2.c`](integer_motion_v2.c) registers its own
  `VmafOption[]` table for the seven motion knobs
  (`motion_force_zero`, `motion_blend_factor`, `motion_blend_offset`,
  `motion_fps_weight`, `motion_max_val`, `motion_five_frame_window`,
  `motion_moving_average`) — duplicating motion v1's
  [`integer_motion.c`](integer_motion.c) surface byte-for-byte
  against upstream Netflix `4e469601`. The duplication is
  deliberate; v1 and v2 are independent extractors with independent
  output namespaces (`VMAF_integer_feature_motion*_score` vs
  `…_v2_score`). On rebase: when touching one extractor's option
  help string, touch the other; when upstream touches the option
  table, port the change to **both** extractors. ADR-0141 catches
  drift on the next edit.
- **`motion_v2` rejects `motion_five_frame_window=true`**
  (fork-local, ADR-0337): `init()` returns `-ENOTSUP` and logs a
  pointer at the ADR. The 5-frame mode requires a `prev_prev_ref`
  field on `VmafFeatureExtractor` plus `n_threads * 2 + 2`
  picture-pool sizing in `vmaf_read_pictures` (upstream `a2b59b77`)
  that conflicts with the fork's [ADR-0152](../../../docs/adr/0152-vmaf-read-pictures-monotonic-index.md)
  `read_pictures*` decomposition. The picture-pool refactor is
  deferred to its own PR. Mirrors [ADR-0219](../../../docs/adr/0219-motion3-gpu-coverage.md)
  §Decision's GPU motion3 `-ENOTSUP` precedent. On rebase: when
  the picture-pool refactor PR lands, flip the `-ENOTSUP` guard to
  a `prev_prev_ref` lookup and reinstate the `min_idx = 5? 2 : 1`
  branching in `flush()` (currently collapsed to `min_idx = 1`
  per ADR-0337's deferral). See
  [rebase-notes ADR-0337](../../../docs/rebase-notes.md) for the
  deferred-hunks ledger.
- **`motion_v2` NEON shift semantics** (fork-local, ADR-0145):
  [`arm64/motion_v2_neon.c`](arm64/motion_v2_neon.c) uses
  **arithmetic** right-shift throughout (`vshrq_n_s64(v, 16)` for
  the Phase-2 known shift, `vshlq_s64(v, -(int64_t)bpc)` for the
  Phase-1 runtime shift). The fork's AVX2 variant
  [`x86/motion_v2_avx2.c`](x86/motion_v2_avx2.c) uses
  `_mm256_srlv_epi64` (*logical*) which can diverge from scalar on
  negative-diff pixels. NEON matches scalar, AVX2 does not — this
  is intentional until the AVX2 audit lands. On rebase: keep the
  arithmetic-shift form in NEON; do NOT port AVX2's logical pattern
  even if it looks simpler. 4-lane stride + scalar tails on both
  sides of the row are load-bearing for the x_conv edge-mirror
  contract. See
  [ADR-0145](../../../docs/adr/0145-motion-v2-neon-bitexact.md)
  and [rebase-notes 0038](../../../docs/rebase-notes.md).
- **IQA / VIF SIMD helper decomposition** (fork-local, ADR-0146):
  `iqa_convolve` in
  [`iqa/convolve.c`](iqa/convolve.c) is split into
  `iqa_convolve_horizontal_pass` + `iqa_convolve_vertical_pass`
  composed by `iqa_convolve_1d_separable` (for `IQA_CONVOLVE_1D`)
  and `iqa_convolve_2d`; `iqa_ssim` in
  [`iqa/ssim_tools.c`](iqa/ssim_tools.c) is split into
  `ssim_workspace_alloc` / `_free` + `ssim_compute_stats` +
  `ssim_init_args` around an explicit `struct ssim_workspace`;
  `vif_statistic_s_avx2` in
  [`x86/vif_statistic_avx2.c`](x86/vif_statistic_avx2.c) is split
  into `vif_stat_simd8_compute` + `vif_stat_simd8_reduce` around
  an explicit `struct vif_simd8_lane` that carries `__m256` lane
  state between the two halves. **Load-bearing**: the per-lane
  scalar-float reduction via 32-byte aligned `tmp_n[8]` / `tmp_d[8]`
  in `vif_stat_simd8_reduce` preserves ADR-0139 exactly; the
  convolve pass ordering in `iqa_convolve_1d_separable` preserves
  ADR-0138 exactly. On rebase: if upstream rewrites any of these
  three functions, prefer upstream's shape **only** if it maintains
  both invariants; otherwise keep the fork's split and re-document
  divergence in
  [rebase-notes 0039](../../../docs/rebase-notes.md). Also: the
  TU-static rename `_calc_scale` → `iqa_calc_scale` in
  `iqa/convolve.c` is fork-local — keep on rebase. See
  [ADR-0146](../../../docs/adr/0146-nolint-sweep-function-size.md).
- **IQA reserved-identifier rename** (fork-local, ADR-0148):
  every `_iqa_*` / `struct _kernel` / `_ssim_int` /
  `_map_reduce` / `_map` / `_reduce` / `_context` /
  `_ms_ssim_*` / `_ssim_*` / `_alloc_buffers` /
  `_free_buffers` symbol and the four underscore-prefixed
  header guards (`_CONVOLVE_H_`, `_DECIMATE_H_`,
  `_SSIM_TOOLS_H_`, `__VMAF_MS_SSIM_DECIMATE_H__`) was renamed
  to its non-reserved spelling. The IQA tree is now baseline
  lint-clean. **Load-bearing NOLINTs** (do not collapse on
  rebase): scoped
  `NOLINTBEGIN/END(clang-analyzer-security.ArrayBound)` around
  the inner kernel loops in `ssim_accumulate_row` and
  `ssim_reduce_row_range` of
  [`integer_ssim.c`](integer_ssim.c) — the
  `k_min`/`k_max` clamping is provably correct but the
  analyzer can't follow it across the helper boundary; scoped
  `NOLINTBEGIN/END(clang-analyzer-unix.Malloc)` around
  `check_simd_variant` and `check_case` in
  [`../../test/test_iqa_convolve.c`](../../test/test_iqa_convolve.c)
  — test exits process on failure path; small allocations
  leak by design at test end; cross-TU
  `NOLINTNEXTLINE(misc-use-internal-linkage)` on `compute_ssim`
  in [`ssim.c`](ssim.c) and `compute_ms_ssim` in
  [`ms_ssim.c`](ms_ssim.c) — declared in `ssim.h` /
  `ms_ssim.h`, called from `float_ssim.c` /
  `float_ms_ssim.c`; clang-tidy runs per-TU and can't see the
  bridge. On rebase, keep all these brackets verbatim. See
  [ADR-0148](../../../docs/adr/0148-iqa-rename-and-cleanup.md)
  and [rebase-notes 0041](../../../docs/rebase-notes.md).

- **`integer_adm.c` i4_adm_cm int32 rounding overflow**
  (fork-inherited, ADR-0155): both `add_bef_shift_flt[]`
  initialiser loops in
  [`integer_adm.c`](integer_adm.c) (scales 1–3) assign
  `1u << 31 = 0x80000000` into `int32_t`, which wraps to
  `-2147483648`. The rounding term is sign-negated; every
  downstream `(prod + add_bef_shift) >> 32` subtracts 2^31
  instead of adding it. **Deliberately preserved** — the buggy
  arithmetic is encoded in the Netflix golden
  `assertAlmostEqual` values (project hard rule #1 /
  [ADR-0024](../../../docs/adr/0024-netflix-golden-preserved.md)).
  Do NOT widen `add_bef_shift_flt[]` to `uint32_t` or `int64_t`
  without a coordinated Netflix-authored golden-number update
  (the [ADR-0142](../../../docs/adr/0142-port-netflix-18e8f1c5-vif-sigma-nsq.md)
  carve-out). Netflix upstream #955 is OPEN since 2020 with no
  maintainer response — until it closes with a fix, the
  overflow stays.  See
  [ADR-0155](../../../docs/adr/0155-adm-i4-rounding-deferred-netflix-955.md)
  and [rebase-notes 0048](../../../docs/rebase-notes.md).

- **`integer_adm` GPU row-level rounding invariant** (fork-local, ADR-1167):
  In integer ADM contrast masking kernels (`cuda/integer_adm/adm_cm.cu` and
  `hip/integer_adm/adm_cm.hip`), the inner accumulation rounding shift
  `(row_total + add_shift_inner_accum) >> shift_inner_accum` must NEVER be
  distributed across warp reduction or per-thread reduction. Bitwise right-shift
  with rounding bias is non-linear and non-distributive over addition. The
  kernel must accumulate all columns of a row in 64-bit precision across the
  entire width `[start_col, end_col)` before applying the shift once per row.
  Kernel launch grids must use `gridDim.x = 1` to ensure single-block/warp
  row traversal. Furthermore, border row selection at `i == 0 && top <= 0`
  must use explicit absolute indices `{row_top, row_bot, col_l, col_r}` and
  evaluate `csf_a` at row 0 center (`i * src_stride + j`), never walking running
  pointer offsets. See [ADR-1167](../../../docs/adr/1167-adm-cm-row-level-rounding.md).

- **`integer_adm.c` / `adm_tools.c` are restructured upstream-mirror
  files** (ADR-1141, 2026-09-02): every kernel expression is verbatim
  but the code no longer lines up textually with Netflix/vmaf — re-port
  upstream hunks by hand into the owning helper (function map in
  [rebase-notes](../../../docs/rebase-notes.md)
  "refactor/c-rework-adm"). Invariants a rebase or a follow-up must
  keep: (1) `adm_cm_thresh()` / `i4_adm_cm_thresh()` /
  `adm_cm_thresh3x3_s()` are the closed form of the nine upstream
  `ADM_CM_THRESH_S_*` corner / edge / interior macros — mirror-to-1
  before the first edge, clamp-to-last past the last edge, nine terms in
  the macro order (the float twin's summation order is golden-gated);
  the `(int16_t)` cast on the integer centre term truncates on purpose.
  (2) The border branch is the predicate pair `left_edge = left <= 0` /
  `right_edge = right > w - 1` — do not restore upstream's four-way
  branch (its unreachable third arm read `rfactor[]` out of bounds).
  (3) The ADR-0155 rounding terms live in `i4_adm_round_terms()`
  (`int32_t`, sign-negated for scales 1..3); the `i4_shift_dst[]` /
  `i4_shift_flt[]` tables are file-scope. (4) `adm_decouple()` /
  `adm_decouple_s123()` keep the mutable `int32_t *lut` parameter of
  the `AdmState` / SIMD-twin prototype behind a cited
  `readability-non-const-parameter` + `cppcheck constParameterCallback`
  pair; `extract()` keeps its frozen `VmafFeatureExtractor::extract`
  prototype the same way. (5) `adm_dwt2_s()` in `adm_tools.c` stays one
  function under the ADR-1057 `optimize("-ffp-contract=off")` /
  `#pragma clang fp contract(off)` bracket; never share DWT helpers
  between it and `adm_dwt2_lo_s()`. (6) Float accumulators in
  `adm_csf_den_scale_s()` / `adm_cm_s()` stay `float` (`adm_fold3_s()`);
  only `adm_sum_cube_s()` is `double` (ADR-0418). (7) Both C TUs keep
  `NULL` under the file-scoped `NOLINTBEGIN/END(modernize-use-nullptr)`
  bracket (ADR-1138); keep the `NOLINTEND` line at end of file.
  Bit-exactness proof for any further change: rerun the 62-case
  `--precision max` CLI matrix from the ADR-1141 research digest
  against a baseline binary — the goldens alone do not reach the
  small-scale border branches.

- **`psnr_hvs` AVX2 DCT bit-exactness** (fork-local, ADR-0159):
  [`x86/psnr_hvs_avx2.c`](x86/psnr_hvs_avx2.c) vectorizes the
  Xiph/Daala 8×8 integer DCT across 8 rows in parallel
  (`__m256i`, 8× int32) via **butterfly → transpose → butterfly
  → transpose**. Byte-identical `od_coeff` output to scalar
  under `FLT_EVAL_METHOD == 0`; float accumulators (means /
  variances / mask / error) kept scalar by construction per
  ADR-0139 precedent. **On rebase**: never introduce a
  horizontal-reduce vectorization of the float accumulators
  without replicating the per-lane scalar-float reduction
  pattern. Keep `#pragma STDC FP_CONTRACT OFF` at the TU
  header — removing it allows `fmaf` and breaks the 1-ulp
  guarantee. The scalar TU
  [`third_party/xiph/psnr_hvs.c`](third_party/xiph/psnr_hvs.c)
  is the bit-exact reference; don't touch its butterfly block
  without matching changes in the AVX2 TU. See
  [ADR-0159](../../../docs/adr/0159-psnr-hvs-avx2-bitexact.md)
  and [rebase-notes 0052](../../../docs/rebase-notes.md).

- **SSIMULACRA 2 end-to-end regression gate** (fork-local, ADR-0164):
  [`python/test/ssimulacra2_test.py`](../../../python/test/ssimulacra2_test.py)
  pins pooled + per-frame `--feature ssimulacra2` output on two
  checked-in YUV fixtures. **On rebase**: if the scalar or any SIMD
  path changes semantically (should never happen per ADR-0161's
  bit-exact contract), the test will fail with values that differ
  by more than 1e-4. Don't update the pinned floats unilaterally —
  figure out which kernel drifted and fix it. The Netflix golden
  assertions in `quality_runner_test.py` et al. remain untouched.

- **SPEED and CAMBI feature decomposition and NULL preservation** (fork-local,
  ADR-1146): [`cambi.c`](cambi.c), [`cambi.h`](cambi.h), [`speed.c`](speed.c),
  [`speed_qa.c`](speed_qa.c), and SIMD twins in [`x86/`](x86/)
  (`cambi_avx2.c`, `cambi_avx512.c`, `speed_avx2.c`, `speed_avx512.c`) are
  decomposed into static single-purpose helpers to satisfy
  `readability-function-size` (≤60 lines, max nesting 4). C TUs preserve `NULL`
  through file-scoped `/* NOLINTBEGIN(modernize-use-nullptr) */` /
  `/* NOLINTEND(modernize-use-nullptr) */` brackets (ADR-1138). **On rebase**:
  when merging upstream changes to `calculate_c_values`, `init`, or
  `est_params`, map changes into the respective decomposed helpers
  (`c_values_*`, `validate_and_setup_dimensions`, `alloc_cambi_buffers`,
  `solve_covariance_system`) rather than re-inlining. Shared prototypes in
  `cambi_internal.h` and `speed_internal.h` must not change without mirroring
  to all GPU/CPU twins (verified by `scripts/ci/twin-drift-check.sh`). Numerical
  bit-exactness is governed by ADR-1146.

- **SSIMULACRA 2 `picture_to_linear_rgb` SIMD** (fork-local, ADR-0163):
  `ssimulacra2_picture_to_linear_rgb_{avx2,avx512,neon}` vectorises
  the last scalar hot path (2×/frame). Strategy: per-lane scalar
  reads (all chroma ratios + 8/16-bit), SIMD matmul + normalise +
  clamp, per-lane scalar `powf` for sRGB EOTF. New decoupling
  header `ssimulacra2_simd_common.h` defines `simd_plane_t`; the
  dispatch wrapper in `ssimulacra2.c` unpacks `VmafPicture` into it.
  **On rebase**: (1) keep scalar-order matmul chain
  `G = Yn + cb_g*Un; G += cr_g*Vn;` — regrouping drifts ~1 ulp;
  (2) per-lane scalar `powf` is load-bearing — no vector
  polynomial; (3) `simd_plane_t` layout `{data, stride, w, h}`
  is assumed by all three SIMD TUs; (4) arbitrary chroma ratios
  (non-420/422/444) must still work — don't delete the `int64_t`
  fallback branch. SSIMULACRA 2 now has **zero scalar hot paths**.
  See
  [ADR-0163](../../../docs/adr/0163-ssimulacra2-ptlr-simd.md) and
  [rebase-notes 0055](../../../docs/rebase-notes.md).

- **SSIMULACRA 2 FastGaussian IIR blur SIMD** (fork-local, ADR-0162):
  `ssimulacra2_blur_plane_{avx2,avx512,neon}` vectorises the 30×/frame
  2-pass separable IIR blur. Horizontal pass batches rows (AVX2: 8,
  AVX-512: 16, NEON: 4) and uses gather/lane-set loads to pull
  column-n values from N rows into a SIMD vector; vertical pass
  SIMD-iterates columns over the per-column `prev1_*`/`prev2_*`
  state arrays. **On rebase**: (1) preserve left-to-right summation
  `(o0 + o1) + o2` and `n2*sum - d1*prev1 - prev2` chaining — any
  re-grouping drifts by ~1 ulp; (2) `col_state` layout is
  `[prev1_0|prev1_1|prev1_2|prev2_0|prev2_1|prev2_2]` in 6×w
  contiguous floats; SIMD loads assume this; (3) NEON lane-set
  pattern (4 `vsetq_lane_f32` per input) replaces the
  non-existent aarch64 gather intrinsic; (4) row-batching lane
  layout: lane i holds row (y_base + i). Regression test
  `test_blur` in `test_ssimulacra2_simd.c` catches all four. See
  [ADR-0162](../../../docs/adr/0162-ssimulacra2-iir-blur-simd.md)
  and [rebase-notes 0054](../../../docs/rebase-notes.md).

- **SSIMULACRA 2 SIMD bit-exactness** (fork-local, ADR-0161):
  [`x86/ssimulacra2_avx2.c`](x86/ssimulacra2_avx2.c),
  [`x86/ssimulacra2_avx512.c`](x86/ssimulacra2_avx512.c),
  [`arm64/ssimulacra2_neon.c`](arm64/ssimulacra2_neon.c) and
  [`arm64/ssimulacra2_sve2.c`](arm64/ssimulacra2_sve2.c) (T7-38,
  ADR-0213) all produce byte-identical output to scalar on the 5
  vectorised kernels (`multiply_3plane`, `linear_rgb_to_xyb`,
  `downsample_2x2`, `ssim_map`, `edge_diff_map`) under
  `FLT_EVAL_METHOD == 0`, plus the IIR blur and PTLR ports
  (ADR-0162 / ADR-0163). **On rebase**: (1) preserve left-to-right
  scalar summation order in every matmul + downsample chain —
  a `(a+b)+(c+d)` pairing drifts by 1 ULP and the regression test
  `test_ssimulacra2_simd` catches it; (2) `cbrtf` stays per-lane
  scalar libm — no vector polynomial; (3) reductions in
  `ssim_map`/`edge_diff_map` use the ADR-0139 per-lane `double`
  scalar tail; (4) the SVE2 sister TU is locked to a fixed 4-lane
  predicate (`svwhilelt_b32(0, 4)`) so its arithmetic order
  matches the NEON sibling regardless of runtime vector length —
  do **not** widen to `svptrue_b32()` without a separate ADR
  plus snapshot regen, even if it looks like a free perf win. See
  [ADR-0161](../../../docs/adr/0161-ssimulacra2-simd-bitexact.md),
  [ADR-0213](../../../docs/adr/0213-ssimulacra2-sve2.md), and
  [rebase-notes 0053](../../../docs/rebase-notes.md) /
  [rebase-notes 0074](../../../docs/rebase-notes.md).

- **SSIMULACRA 2 Vulkan host-path SIMD** (fork-local, ADR-0252):
  [`x86/ssimulacra2_host_avx2.c`](x86/ssimulacra2_host_avx2.c) and
  [`arm64/ssimulacra2_host_neon.c`](arm64/ssimulacra2_host_neon.c)
  are `plane_stride`-parameterised variants of `linear_rgb_to_xyb`
  and `downsample_2x2` for the Vulkan pyramid layout (channel slot
  size = full-resolution frame, fixed across downsampled scales).
  These two TUs carry the **same ADR-0161 bit-exactness contract**
  as their CPU-extractor siblings: per-lane scalar `vmaf_ss2_cbrtf`,
  `#pragma STDC FP_CONTRACT OFF`, `-ffp-contract=off`, left-to-right
  addition order. **On rebase**: if upstream or a follow-up PR
  changes the scalar `ss2v_host_linear_rgb_to_xyb` or
  `ss2v_downsample_2x2` arithmetic order in `ssimulacra2_vulkan.c`,
  the SIMD TUs and their `test_host_xyb` / `test_host_downsample`
  scalar references must be updated in lockstep — the byte-exact
  contract breaks silently if the scalar changes without the SIMD.
  See [ADR-0252](../../../docs/adr/0252-ssimulacra2-host-xyb-simd.md)
  and [rebase-notes 0106](../../../docs/rebase-notes.md).

- **`psnr_hvs` NEON DCT bit-exactness** (fork-local, ADR-0160):
  [`arm64/psnr_hvs_neon.c`](arm64/psnr_hvs_neon.c) is the aarch64
  sister port to the AVX2 TU. NEON's 4-wide `int32x4_t` splits
  each 8-column row into `r_k_lo` (cols 0-3) + `r_k_hi` (cols
  4-7); the 30-butterfly runs twice per DCT pass, and 8×8
  transpose = four `transpose4x4_s32` (via `vtrn1q_s32` /
  `vtrn2q_s32` / `vtrn1q_s64` / `vtrn2q_s64`) + a top-right
  ↔ bottom-left block swap. **On rebase**: the two SIMD TUs
  (AVX2 + NEON) must move in lockstep with the scalar Xiph
  reference — any change to the butterfly in `psnr_hvs.c`
  requires matched edits to both SIMD TUs and a re-run of
  `test_psnr_hvs_{avx2,neon}`. `accumulate_error()` must keep
  threading the outer `ret` by pointer (ADR-0159 summation-order
  lesson; a local float accumulator would drift the Netflix
  golden by ~5.5e-5). `#pragma STDC FP_CONTRACT OFF` is ignored
  by aarch64 GCC (non-fatal `-Wunknown-pragmas`) but kept for
  portability; aarch64 GCC does not contract `a + b * c` across
  statements at default optimization anyway.
  **IMPORTANT — Intel icx (`intel-llvm`)**: `#pragma STDC FP_CONTRACT
  OFF` is also silently ignored by icx unless `-fp-model=precise` is
  also on the command line. The SIMD carve-out static libs in
  `core/src/meson.build` detect `cc.get_id() == 'intel-llvm'` and
  append `-fp-model=precise`; the SIMD test executables in
  `core/test/meson.build` do the same via `_simd_strict_fp_args`. Do
  not remove these flags without re-running `--suite=fast --suite=simd`
  under icx. Traced via the 2026-05-30 all-backends CI failure. See
  [ADR-0160](../../../docs/adr/0160-psnr-hvs-neon-bitexact.md)
  and [rebase-notes 0052](../../../docs/rebase-notes.md).
- **`fastdvdnet_pre.c` 5-frame-window contract** (fork-local,
  ADR-0215): the FastDVDnet temporal pre-filter extractor is wired
  to the I/O contract `frames: float32 NCHW [1, 5, H, W]` (channel
  axis stacks `[t-2, t-1, t, t+1, t+2]`) → `denoised: float32 NCHW
  [1, 1, H, W]`. Three pieces are load-bearing on rebase: (1) the
  centre index is 2 (`FASTDVDNET_PRE_CENTRE`) — `gather_window`
  computes channel-k offsets relative to it; (2) the ring buffer
  holds 5 slots and replicates the closest available end frame for
  channel positions outside the available window (clip start +
  end); (3) the registered feature name is
  `fastdvdnet_pre_l1_residual` — downstream consumers (the future
  FFmpeg `vmaf_pre_temporal` filter, training harnesses) bind to
  that exact string. **T6-7b update (ADR-0255)**: the registry now
  ships real upstream FastDVDnet weights (`smoke: false`) wrapped by
  a luma adapter in `ai/scripts/export_fastdvdnet_pre.py`; the
  previous smoke-only placeholder is history. The C-side contract is
  unchanged; the wrapper keeps the I/O names (`frames` / `denoised`)
  byte-identical, handles `Y → [Y, Y, Y]` tiling, supplies a
  constant `sigma = 25/255` noise map, and performs BT.601 RGB→Y
  collapse internally. Two rebase-sensitive invariants flow from the
  wrapper: (4) upstream's `nn.PixelShuffle` is swapped for an
  allowlist-safe `Reshape`/`Transpose`/`Reshape` decomposition at
  export time (`DepthToSpace` is not on the ONNX op allowlist —
  ADR-0255 §Decision); (5) the upstream commit is pinned at
  `c8fdf6182a0340e89dd18f5df25b47337cbede6f` and the exporter
  enforces the upstream weights sha256
  `9d9d8413c33e3d9d961d07c530237befa1197610b9d60602ff42fd77975d2a17`
  to keep the weights drop reproducible. See
  [ADR-0215](../../../docs/adr/0215-fastdvdnet-pre-filter.md) and
  [ADR-0255](../../../docs/adr/0255-fastdvdnet-pre-real-weights.md).
- **`transnet_v2.c` 100-frame-window contract** (fork-local,
  ADR-0223 + ADR-0257) — TransNet V2 shot-boundary detector is
  wired to the I/O contract `frames: float32 [1, 100, 3, 27, 48]`
  (100-frame window of 27x48 RGB thumbnails) → `boundary_logits:
  float32 [1, 100]`. Four pieces are load-bearing on rebase:
  (1) the ring buffer holds 100 slots and replicates the *oldest*
  available frame across pre-clip slots (head-clamp at clip
  start) — the corresponding output logit is read from
  `output_logits[WINDOW-1]` because `gather_window` lays the
  most-recent push at the LAST channel; (2) the dual feature-name
  surface — the extractor emits both
  `shot_boundary_probability` (sigmoid of the centre-slot
  logit) **and** `shot_boundary` (binary 0/1 thresholded at 0.5);
  downstream consumers (the per-shot CRF predictor T6-3b, the
  FFmpeg shot-cut filter shipping with T6-3b) bind to *both*
  exact strings; (3) the shipped ONNX under
  `model/tiny/transnet_v2.onnx` is real upstream weights as of
  ADR-0257 (`smoke: false`, MIT, upstream commit pin
  `77498b8e`); the wrapper layer that adapts NTCHW→NTHWC and
  selects only `output_1` lives in
  `ai/scripts/export_transnet_v2.py` and must be re-run if the
  upstream commit pin moves; (4) the export pipeline replaces a
  rank-2 `UnsortedSegmentSum` in upstream's `ColorHistograms`
  branch with an equivalent `ScatterND` reduction='add'
  subgraph — semantics-preserving but a load-bearing rewrite that
  any future upstream-graph re-conversion has to repeat. See
  [ADR-0223](../../../docs/adr/0223-transnet-v2-shot-detector.md)
  [ADR-0257](../../../docs/adr/0257-transnet-v2-real-weights.md).

- **`cambi.c` GPU port is hybrid host/GPU per
  [ADR-0205](../../../docs/adr/0205-cambi-gpu-feasibility.md) +
  [ADR-0210](../../../docs/adr/0210-cambi-vulkan-integration.md)
  (T7-36 integration).** The Vulkan kernel offloads only the
  embarrassingly-parallel phases (preprocessing scaffold +
  derivative + 7×7 SAT spatial mask + 2× decimate + 3-tap mode
  filter) to the GPU; the precision-sensitive
  `calculate_c_values` sliding-histogram pass + top-K spatial
  pooling stay on the host. Any CPU-side change to the c-value
  formula or the histogram update protocol must keep the host
  residual call site
  (`cambi_vulkan.c::cambi_vk_extract` → `vmaf_cambi_calculate_c_values`)
  lock-step with the CPU `calculate_c_values` — they are
  intentionally the same code, called against the GPU-produced
  image + mask buffers.
  - **`cambi_internal.h` invariant**: this internal-only header
    exposes cambi.c's file-static helpers (`get_spatial_mask`,
    `decimate`, `filter_mode`, `calculate_c_values`,
    `spatial_pooling`, `weight_scores_per_scale`,
    `get_pixels_in_window`, `cambi_preprocessing`,
    `increment_range` / `decrement_range` /
    `get_derivative_data_for_row` callbacks) to the GPU twin via
    a thin trampoline block at the bottom of `cambi.c`. **Do not
    rename or change the signatures of those helpers without
    updating the trampoline block + the header in the same PR
    or the GPU build breaks.** The trampoline body is the *only*
    fork-added code inside `cambi.c`; the upstream-mirror body
    above stays byte-identical to keep Netflix sync clean.
  - Strategy III (fully-on-GPU c-values via direct per-pixel
    histogram) is documented in
    [research digest 0020](../../../docs/research/0020-cambi-gpu-strategies.md)
    but deferred to a future batch — *do not* attempt to
    optimise it inside the v1 hybrid integration.

- **VIF kernelscale stays on the precomputed
  `vif_filter1d_table_s` flow — Strategy E in Research-0024.**
  The fork carries an 11-entry `enum vif_kernelscale_enum`
  plus `vif_filter1d_table_s[11][4][65]` of frozen `const float`
  Gaussian taps in [`vif_tools.h`](vif_tools.h). The Netflix
  upstream chain (`4ad6e0ea` runtime helpers, `8c645ce3`
  prescale options, `41d42c9e` edge-mirror bugfix) computes
  Gaussians at runtime — that loses the SIMD bit-exact
  contract that ADR-0138 / 0139 / 0142 / 0143 froze. **Do not
  port `4ad6e0ea` / `8c645ce3` verbatim.** A future port that
  adds runtime helpers as an *opt-in second path* (Strategy C)
  is allowed; it must not touch the default
  `vif_kernelscale=1.0` + `vif_prescale=1.0` code path.
  Mirror bugfix `41d42c9e` is a separate decision — must come
  with paired `places=4 → places=3` golden loosening per the
  ADR-0142 Netflix-authority precedent. See
  [Research-0024](../../../docs/research/0024-vif-upstream-divergence.md)
  for the full divergence analysis + decision matrix.

- **`compute_adm` signature stays on the fork's parameter
  list — Strategy E in Research-0024.** Netflix upstream
  `4dcc2f7c` adds 12 new parameters (`luminance_level`,
  `adm_csf_scale`, `adm_csf_diag_scale`, `adm_noise_weight`,
  `adm_bypass_cm`, `adm_p_norm`, `adm_f1s0..3`, `adm_f2s0..3`,
  `adm_skip_aim_scale`, `adm_skip_scale0`) plus a new
  `score_aim` output. Threading those through the SIMD paths
  (`adm_avx2.c` / `adm_avx512.c` / `adm_neon.c`) **and** the
  GPU twins (`adm_vulkan.c` / `adm_cuda.c` / `adm_sycl.cpp`)
  is multi-day work, and the new `aim` feature has no fork-
  side golden values yet. **Do not port `4dcc2f7c` until
  there is concrete user demand for `aim` and a coordinated
  cross-backend port plan.** See
  [Research-0024 §"Same divergence test for motion + float_adm"](../../../docs/research/0024-vif-upstream-divergence.md).

### `picture_copy()` carries a `channel` parameter

Upstream commit `d3647c73` (T-NEW-1, ported via this fork's
`upstream/port-d3647c73-feature-speed`) widened the
`picture_copy()` / `picture_copy_hbd()` signatures with a new
`int channel` argument so the new `speed_chroma` and
`speed_temporal` extractors can lift U / V planes from
`VmafPicture`. Every fork-local extractor that calls
`picture_copy()` (`cuda/integer_ms_ssim_cuda.c`,
`vulkan/ssim_vulkan.c`, `vulkan/ms_ssim_vulkan.c`) passes
`channel=0`; the upstream-mirror `float_*` callers already do.
**If a future upstream commit evolves the signature further
(extra parameter, type change), update those four fork-local
call sites in lockstep with the upstream-mirror ones — silently
trailing the upstream signature change will fail compilation
on any GPU backend.** See
[`docs/rebase-notes.md` §0075](../../../docs/rebase-notes.md).

### `vmaf_fex_ssim` is registered fork-side, not upstream

Upstream Netflix's `feature_extractor.c` does **not** list
`&vmaf_fex_ssim` in its `feature_extractor_list[]`, and upstream
does not compile `integer_ssim.c` either — both are dormant on the
upstream master branch. The fork wires both paths up so that
`vmaf --feature ssim` resolves at the CLI; the
fix-up touches three upstream-mirror surfaces (the registry-array
row in `feature_extractor.c`, the matching `extern` declaration,
and the `#include "config.h"` in `integer_ssim.c`) plus one
fork-local meson-build line. **On every upstream sync, re-check
that the fork's three additions remain in place.** If upstream
ever lands its own integer-SSIM registration, drop the fork's row
in favour of upstream's; the file structure is identical so the
diff should resolve cleanly in `git rebase`. The `config.h` include
in `integer_ssim.c` is load-bearing on Vulkan-enabled LTO builds —
without it the `VmafFeatureExtractor` struct layout disagrees
between TUs (different `HAVE_CUDA` / `HAVE_SYCL` / `HAVE_VULKAN`
visibility) and GCC fires `-Wlto-type-mismatch` at link time. The
generated `config.h` include in `feature_extractor.h` is the
project-wide guard for this layout; keep it there so every extractor
definition and every registry consumer sees the same backend fields.

### SSIM SIMD dispatch globals are pthread_once-installed (ADR-0871)

`float_ssim.c` and `float_ms_ssim.c` install the SSIM/iqa_convolve
SIMD dispatch tables (`g_ssim_precompute`, `g_ssim_variance`,
`g_ssim_accumulate`, `g_iqa_convolve` in `iqa/ssim_tools.c`) by
calling `iqa_ssim_install_dispatch_once(&s_dispatch_guard,
installer_cb)` from their per-extractor `init()`. The guard
parameter is retained for API symmetry but the actual mutual
exclusion is provided by a file-static `pthread_once_t` inside
`iqa/ssim_tools.c` shared across both TUs — without that sharing
the two per-TU guards would each be allowed to fire once, racing
on the same dispatch globals (TSan-confirmed 2026-05-30).

**Invariants** when modifying SSIM-related dispatch code:

- Never call `iqa_ssim_set_dispatch` / `iqa_convolve_set_dispatch`
  directly from a per-extractor `init()` body. The setters
  themselves are unsynchronised by design; the only correct
  serialisation point is `iqa_ssim_install_dispatch_once`.
- The installer callbacks across TUs must remain idempotent (install
  the same ISA-best function pointers). If a future SIMD path
  (e.g. SVE, AVX10) is added, extend the existing installer rather
  than creating a parallel one — the once-guard fires exactly once
  process-wide.
- If a future upstream commit refactors `float_ssim::init` or
  `float_ms_ssim::init`, preserve the
  `iqa_ssim_install_dispatch_once(...)` call. Replacing it with a
  bare dispatch install resurrects the data race.

See [ADR-0871](../../../docs/adr/0871-ssim-dispatch-pthread-once.md)
and the underlying research digest at
[`docs/research/tsan-race-audit-2026-05-30.md`](../../../docs/research/tsan-race-audit-2026-05-30.md).

### `speed_chroma` / `speed_temporal` are float-build-only

The two upstream Speed extractors register inside the
`#if VMAF_FLOAT_FEATURES` block in `feature_extractor.c`. They
are absent from a default `meson setup` build; users who want
them must pass `-Denable_float=true`. Do **not** lift them out
of the `#if` block — they call into the Speed-specific helpers
in `vif_tools.c` that are themselves only compiled in the
float-features path.

[ADR-0253](../../../docs/adr/0253-speed-qa-extractor.md)
(Proposed) records the deferral on extending this surface with a
SpEED-QA full-frame reduction or a SpEED-driven model. Status quo
is the binding contract until one of the three named triggers in
that ADR fires.

### `speed_internal.c` is the shared CPU helper TU for the SpEED GPU twins (ADR-0964)

`core/src/feature/speed_internal.{h,c}` is the contract between the
CPU SpEED extractor (`speed.c`) and the GPU twins
(`feature/{cuda,hip,sycl}/speed_{chroma,temporal}_*.{c,cpp}`).
The header declares 9 functions (dimensions, float-stride,
filter+downscale, covariance, eigendecomp, QR factorise, Q^T
multiply, backward-substitution, regularity check); the GPU TUs
`#include` it and call 7 of them.

**Wiring invariant — any new feature extractor that ships GPU
twins must have FIVE companion changes in the same PR**:

1. **CPU implementation TU** — the scalar reference under
   `core/src/feature/<name>.c` (or, for shared math, an
   `<name>_internal.c` under the same directory).
2. **Header declaration** in `core/src/feature/<name>.h` or
   `<name>_internal.h` — the GPU-callable surface.
3. **Meson source list** —
   - CPU TU added to the appropriate block in `core/src/meson.build`
     (`libvmaf_feature_sources` or under `if float_enabled`).
   - HIP TU added to `core/src/hip/meson.build` `hip_sources +=
     files(...)`.
   - SYCL TU added to `core/src/meson.build` `sycl_feature_sources`.
   - CUDA TU added to `core/src/meson.build`
     `libvmaf_feature_sources` under `if is_cuda_enabled`.
4. **Registry entry** in
   `core/src/feature/feature_extractor.c` — `extern` declaration
   under the right `#if HAVE_<BACKEND>` block plus a row in
   `feature_extractor_list[]`.
5. **CPU-vs-GPU parity test** under `core/test/`, mirroring
   `test_sycl_motion3_parity.c` (or
   `test_sycl_speed_{chroma,temporal}_parity.c` for the SpEED
   example).  Must skip cleanly when the relevant GPU device is
   not visible.

If any of the five is missing, the symptom is silent: the
extractor name does not resolve in `vmaf_get_feature_extractor_by_name()`
and the GPU pipeline never runs, while CI stays green because no
parity gate fires.  The `feature_extractor_list_audit()` function
in `feature_extractor.c` catches duplicate registrations, not
*missing* ones.

**Source-of-truth note**: `speed_internal.c` duplicates ~600 LOC
of pure math (eigendecomp, QR, matrix helpers) from `speed.c`.
This is deliberate (see ADR-0964 Alternatives) — keeping
`speed.c` clean of `extern` exposures preserves its
Netflix-mirrored status for the `/sync-upstream` cadence.  If
either copy gets a bug-fix, mirror it to the other; the
CPU-vs-SYCL and CPU-vs-CUDA parity tests will surface drift at CI time.

**CUDA TU dependency on `CudaFunctions` schema (ADR-0965)**: the two
CUDA SpEED TUs (`cuda/speed_chroma_cuda.c` and
`cuda/speed_temporal_cuda.c`) call into the `CudaFunctions` table
using **two specific members**:

- `cuMemHostAlloc((void **)&ptr, size, flags)` — pinned host
  allocation (NOT `cuMemAllocHost`; that variant is not in the table).
- `cuMemFreeHost(ptr)` — pinned host free.

The two CUDA TUs also use `CHECK_CUDA_GOTO(cu_f, CALL, label)` for all
fallible CUDA calls (NOT the legacy `CHECK_CUDA` macro which was removed).
If the `CudaFunctions` table ever gains or renames these members, update
all four `ALLOC_HOST` / `FREE_HOST` macro call sites in both TUs in the
same PR. See `core/src/cuda/cuda_helper.cuh` for the macro contract and
`core/src/cuda/picture_cuda.c` / `core/src/cuda/common.c` for the
canonical usage of these members across the codebase.

### CodeQL `cpp/declaration-hides-variable` rename invariants (2026-05-09)

The 64-alert sweep of 2026-05-09 (see
[`docs/rebase-notes.md`](../../../docs/rebase-notes.md) entry of
the same date) renamed inner-scope shadows in
`x86/adm_avx2.c`, `x86/adm_avx512.c`, `x86/vif_avx2.c`,
`x86/vif_avx512.c`, plus `cambi.c`. **Do not let an upstream port
re-introduce the unprefixed names** — CodeQL re-flags them and the
strict `cpp/declaration-hides-variable` gate trips. The rebase-safe
identifier dictionary is:

| Surface | Old (origin/Netflix) | New (fork) |
|---------|----------------------|------------|
| ADM AVX2/AVX-512 horizontal pass | `j == 0` block at top of i-loop using `j0`/`j1`/`j2`/`j3`/`s0`/`s1`/`s2`/`s3` | the same names but wrapped in a tight `{ ... }` block; the per-`j` tail loop owns the names afterwards |
| ADM AVX2/AVX-512 horizontal pass | inner `__m256i add_shift_HP_vex = _mm256_set1_epi32(32768)` | removed (function-scope outer is bit-identical) |
| `i4_adm_cm_avx2` / `_avx512` rfactor splat | `__m256i rfactor0/1/2` (or `__m512i`) shadowing `float rfactor1[3]` | `rfactor_v0/_v1/_v2` |
| ADM AVX-512 8-bit angle path | `__m512i o_mag_sq` / `ot_dp` / `t_mag_sq` shadowing function-scope `int64_t` scalars | `o_mag_sq_v` / `ot_dp_v` / `t_mag_sq_v` |
| VIF AVX2 vertical-tap loop (`for tap`) | `__m256i f0` / `r0` / `r1` / `d0` / `d1` shadowing the centre-tap broadcasts | `f_tap`, `r_top` / `r_bot`, `d_top` / `d_bot` |
| VIF AVX-512 vertical-tap loop (paired-tap) | `__m512i f0` / `f1` / `r0` / `r16` / `d0` / `d16` / `r1` / `r15` / `d1` / `d15` | `f_tap0` / `f_tap1`, `r_back0` / `r_fwd0`, `d_back0` / `d_fwd0`, `r_back1` / `r_fwd1`, `d_back1` / `d_fwd1` |
| VIF AVX2/AVX-512 horizontal-tap loops (`for fj`) | inner `__m256i fq` / `__m512i fq` re-broadcasting `vif_filt_*[fj]` | `f_tap` |
| VIF AVX2 ref/dis/refdis horizontal stage | inner `__m256i m0` / `m1` reading sliding window | `m_top` / `m_bot` |
| VIF AVX-512 16-bit / subsample tail loops | inner `int ii` / `const ptrdiff_t stride` / `uint16_t *ref` / `uint8_t *ref` / `uint16_t *dis` / `uint8_t *dis` | removed (function-scope outer is identical) |
| VIF AVX-512 tail residual reductions | inner `VifResiduals residuals` shadowing `Residuals512 residuals` | `tail_residuals` |
| VIF AVX-512 subsample horizontal tail | inner `const uint16_t fcoeff` shadowing `__m512i fcoeff` | `fcoeff_scalar` |
| `cambi.c` heatmap init | inner `int err` shadowing init-loop accumulator | `mkdir_err` |
| `cambi.c` full-ref extract path | inner `int err` shadowing dist-side accumulator | `src_err` |

Bit-exactness: the sweep is provably no-op (renames + scope-tighten

- identical-typed deletes only). Netflix CPU golden 3 must remain
green across rebases — re-run the
`PYTHONPATH=$PWD/python python3 -m pytest python/test/quality_runner_test.py
-k test_run_vmaf python/test/vmafexec_test.py
python/test/vmafexec_feature_extractor_test.py -m "not slow"` block
after a port-upstream of any of these files.
- **NIQE fork-pkl parity invariants** (`niqe.c` / `niqe_math.h` /
  `niqe_model.h`, fork-local, ADR-1112): NIQE has **no upstream twin** — it
  replicates the fork Python harness
  (`compat/python-vmaf/core/noref_feature_extractor.py::NiqeNorefFeatureExtractor`),
  not LIVE MATLAB or scikit-video. Two divergences from upstream NIQE are
  **load-bearing** and must survive any refactor:
  1. The AGGD mean parameter `N` carries a **trailing `*aggdratio`** factor
     (`niqe_math.h::niqe_extract_aggd`). Upstream omits it; the pkl was
     trained with it. Dropping it shifts `N` ~0.428 → ~0.245.
  2. The MSCN maps (`niqe_compute_mscn` casts to `float`) **and** the PIL
     bicubic half-resolution output (`niqe_bicubic_resize` final
     `(double)(float)acc`) are **rounded through float32**. PIL returns a
     float32 ('F'-mode) image and the harness quantizes the MSCN maps; without
     these rounds the scale-2 features drift ~4e-7 and, through the
     ill-conditioned averaged covariance, the score by ~1e-4.
  The pristine model `niqe_model.h` is generated from
  `model/other_models/niqe_v0.1.pkl` in the per-block **interleaved** feature
  order (permutation in the header comment); the build-time checksums
  (`mu.sum`, `trace(cov)`, sha256 prefixes) pin it. The end-to-end gate is
  `core/test/test_niqe.c` against `testdata/scores_cpu_niqe.json` at places=4.
  niqe is registered in the **C++23 `feature_extractor.cpp`** registry, not the
  dead `feature_extractor.c` twin.
- **BRISQUE MATLAB-pipeline parity invariants** (`brisque.c` /
  `brisque_math.h` / `brisque_model.h`, fork-local, ADR-1115): BRISQUE has **no
  upstream twin** — it replicates the **gregfreeman MATLAB pipeline that trained
  the bundled model** (`model/other_models/brisque_live.model`), NOT the
  widely-copied krshrimali C++ port. Four choices are **load-bearing** and must
  survive any refactor (porting the C++ behaviour instead would mis-predict
  against the trained model):
  1. The MSCN field (features f1/f2) is fit with **GGD**, not AGGD
     (`brisque_fit_ggd`). krshrimali uses AGGD — a bug vs the paper Table I and
     the trained model.
  2. The Gaussian window uses **sigma = 7/6** (`brisque_build_window`), not the
     truncated `1.166` from the C++ port.
  3. The half-resolution downscale is **MATLAB antialiased bicubic**
     (`brisque_resize_coeffs`, scale 0.5, kernel width 8 → fixed 10 taps,
     symmetric-reflect index map), not OpenCV INTER_CUBIC.
  4. Range-scaling uses the **inline `computescore.cpp` `min_[36]`/`max_[36]`
     arrays** baked into `brisque.c` — NOT the conflicting `allrange` file in the
     same upstream repo (which the reference code never reads; substituting it
     corrupts every score). No output clamp. Prediction is plain `svm_predict`
     (== `svm_predict_probability` for EPSILON_SVR).
  The AGGD fit excludes exact zeros from both sign buckets (strict `x<0` /
  `x>0`, MATLAB semantics — unlike NIQE, which buckets zeros right). The model
  is **embedded at build time** by an `xxd -i` Meson `custom_target` over
  `model/other_models/brisque_live.model` (same path as libvmaf's JSON models;
  symbols `src_brisque_live_model[]` / `_len`, declared in `brisque_model.h`) —
  the big C array is NOT committed (it exceeds the 1 MB large-file gate; only
  the binary model + the tiny declaration header live in-tree). `init()` parses
  that buffer via `svm_parse_model_from_buffer`, or, if the `model_path` option
  is set, loads an on-disk model via `svm_load_model`. The end-to-end gate is
  `core/test/test_brisque.c` against
  `testdata/scores_cpu_brisque.json` (snapshotted because the AGGD strict-sign
  fit is FP-summation-order-sensitive on near-flat content). First feature
  extractor to consume the vendored libsvm. Registered in the **C++23
  `feature_extractor.cpp`** registry.

- **Perceptual side-data weighting golden-isolation invariant**
  (`perceptual_weight.{c,h}` + the weighting branch in
  `core/src/libvmaf.c::vmaf_feature_score_pooled`, fork-local, ADR-1118): the
  Pelorus-driven pooling weights MUST be **inert** unless BOTH (a) weighting is
  enabled (`vmaf_set_perceptual_weight_enabled`, default OFF) AND (b) a valid
  Pelorus blob was registered for the frame (`vmaf_set_perceptual_sidedata`).
  This is **load-bearing for the Netflix golden gate** — the golden pairs carry
  no side-data, so they must score **bit-exact**. Two rules survive any refactor:
  1. `perceptual_weight.c::vmaf_perceptual_weight_at_index` returns **exactly
     `1.0`** for any disabled / empty / absent / non-finite case. Never let it
     return a "close to 1.0" value on the no-side-data path.
  2. `vmaf_feature_score_pooled` must branch on
     `vmaf_perceptual_weight_active()`: when inactive it runs the **literal
     upstream** MEAN / HARMONIC_MEAN expressions (`sum/pic_cnt`,
     `pic_cnt/i_sum − 1`) in the original order — NOT a weighted formula that
     merely evaluates to the same number. Byte-identical, not numerically close.
     The weighted accumulators (`w_sum` / `w_score_sum` / `w_i_sum`) are only
     summed when active. MIN/MAX are intentionally never weighted.
  The end-to-end guard is `core/test/test_perceptual_weight.c` (bit-exact
  pooling without side-data, with enabled-but-absent, and with present-but-
  disabled). The reader is CPU-only and consumes the vendored Pelorus parser
  (ADR-1113) — do not edit those vendored files. R1–R6 graceful-degrade
  (`grid==0` → frame-level scalar; bad ABI → unweighted + log) must also hold.
  3. **Complexity modulation (ADR-1120, Pelorus ABI ≥ 1.3):**
     `derive_salience` scales the salience by
     `complexity_modulation(blob)` = `(1 − 0.5·complexity)` floored at `0.25`
     when `PEL_SEC_COMPLEXITY` is present. This MUST collapse to factor `1.0`
     (no modulation) when the section is **absent** or `complexity` is
     non-finite — that is what keeps the no-side-data golden path bit-exact.
     Never change `complexity_modulation` to return anything but `1.0` on the
     absent/NaN path. The load-bearing guard is `test_complexity_modulates_weight`
     / `test_complexity_modulates_grid_zero` (toggle-proven: stubbing the helper
     to a no-op fails the test). The section is grid-independent, so it also
     modulates the `grid==0` scalar path.

### Scalar VIF statistic and float-motion plane helpers (2026-09-02, c-rework-vif-motion)

- **`integer_vif.c` is the single scalar reference the SIMD tails run.**
  `vif_statistic_8`, `vif_statistic_16` and `vif_compute_line_residuals`
  (called by `x86/vif_avx2.c`, `x86/vif_avx512.c` and `arm64/vif_neon.c` for
  the columns their 16-wide blocks do not cover) all go through
  `vif_horizontal_pixel` → `vif_accumulate_pixel` → `vif_store_residuals`.
  Those helpers hold the upstream arithmetic verbatim (operand types and
  evaluation order included) and are `FORCE_INLINE`; change the statistic
  there once and mirror it in the three kernels — never re-inline a private
  copy into one entry point, or the SIMD block path and the scalar tail
  diverge for widths that are not multiples of 16. Bit-exactness relies on
  the fork's `-std=c23` (contraction off) and no `-march` (no FMA) flags; if
  either changes, re-run the 31-case `--precision max` matrix in
  [`docs/research/2026-09-02-c-rework-vif-motion-bit-exact.md`](../../../docs/research/2026-09-02-c-rework-vif-motion-bit-exact.md).
- **`log_generate` uses `roundf`**, proven bit-identical to upstream's
  `round` over all `VIF_LOG2_TABLE_SIZE` entries. The same LUT feeds the
  AVX-512 gather path (ADR-0500); do not switch rounding modes.
- **`write_scores` append order is an output contract**: four scale scores,
  then `integer_vif` / `_num` / `_den`, then num / den per scale 0..3. The
  `double` totals are explicit left-to-right sums — keep them out of loops.
- **`vif_tools.c` float filters**: `vif_use_avx2_convolution` is the only
  place the ADR-0504 AVX2-only decision lives (AVX-512 float convolution was
  removed for golden parity — do not re-add it here); `vif_mirror_index` is
  reflect-101 (`-idx` / `2n - idx - 2`) and is shared by all three vertical
  passes and the horizontal pass. `vif_pixel_statistic_s` keeps `vif_sigma_nsq`
  as `double` in the `log2f` arguments — narrowing it changes the promotion.
- **`float_motion.c` planes**: `MotionState.plane[0..2]` are Y, U, V; U and V
  exist only with `motion_add_uv`, so `motion_free_planes` (the only teardown)
  must keep the `motion_add_uv` guard. `motion_chroma_heights` rejects
  chroma-less formats **before** any allocation. `motion_score_pair` adds
  Y, then U, then V — the `double` add order is load-bearing for
  `motion_add_uv` parity with the CUDA / SYCL twins. `motion_clip` /
  `motion_blend_clip` are the only places the `motion_fps_weight` /
  `motion_max_val` clip is applied.
- **C translation units keep `NULL`** (ADR-1138): `integer_vif.c` and
  `float_motion.c` carry a file-scoped
  `NOLINTBEGIN/END(modernize-use-nullptr)` bracket; keep the `NOLINTEND` at
  end of file when appending. `vif_tools.c` has no null-pointer constants
  (`grep -c NULL core/src/feature/vif_tools.c` and `grep -c NOLINT` on the
  same file both print `0`) and therefore carries no bracket — do not add
  one unless an upstream hunk brings a `NULL` into that file. `flush()` in
  `float_motion.c` carries a cited `cppcheck-suppress constParameterCallback`
  because the `VmafFeatureExtractor.flush` callback type fixes its prototype.

## Governing ADRs

- [ADR-0024](../../../docs/adr/0024-netflix-golden-preserved.md) —
  the three CPU golden pairs never change.
- [ADR-0041](../../../docs/adr/0041-lpips-sq-extractor.md) — LPIPS
  extractor registration pattern.
- [ADR-0042](../../../docs/adr/0042-tinyai-docs-required-per-pr.md) —
  DNN-backed extractors ship docs under `docs/ai/`.
- [ADR-0236](../../../docs/adr/0236-dists-extractor.md) — `dists_sq`
  mirrors LPIPS' two-input tiny-AI extractor shape. Keep
  `VMAF_DISTS_SQ_MODEL_PATH`, `model_path`, registry id
  `dists_sq_placeholder_v0`, and the `score` scalar output aligned until
  the real DISTS weights replace the smoke checkpoint.
- **LPIPS / DISTS high-bit-depth input invariant** — both extractors
  accept planar 8/10/12/16-bit YUV but keep the ONNX tensor ABI as
  ImageNet-normalised RGB8. High-bit-depth samples are little-endian
  16-bit containers rounded into the 8-bit domain before the shared
  BT.709 limited-range RGB conversion.
- [ADR-0125](../../../docs/adr/0125-ms-ssim-decimate-simd.md) —
  MS-SSIM decimate separable SIMD + bit-exactness contract.
- [ADR-0126](../../../docs/adr/0126-ssimulacra2-feature-extractor.md) +
  [ADR-0130](../../../docs/adr/0130-ssimulacra2-scalar-implementation.md)
  — SSIMULACRA 2 extractor scope + scalar implementation.
- [ADR-0138](../../../docs/adr/0138-iqa-convolve-avx2-bitexact-double.md) —
  `iqa_convolve` widen-then-add bit-exactness pattern.
- [ADR-0139](../../../docs/adr/0139-ssim-simd-bitexact-double.md) —
  SSIM accumulate per-lane scalar-double reduction pattern.
- [ADR-0140](../../../docs/adr/0140-simd-dx-framework.md) — SIMD DX
  framework (`simd_dx.h` + `/add-simd-path` skill upgrade).
- [ADR-0182](../../../docs/adr/0182-gpu-long-tail-batch-1.md) +
  [ADR-0188](../../../docs/adr/0188-gpu-long-tail-batch-2.md) +
  [ADR-0192](../../../docs/adr/0192-gpu-long-tail-batch-3.md) —
  GPU long-tail batches 1–3. Every registered feature extractor
  now has at least one GPU twin (lpips remains ORT-delegated).
- [ADR-0193](../../../docs/adr/0193-motion-v2-vulkan.md) —
  `motion_v2` Vulkan kernel. ADR-0662 corrects its mirror contract:
  `integer_motion_v2.c::mirror` uses reflect-101 (`2 * size - idx - 2`)
  and the CUDA / SYCL / Vulkan twins must keep that literal aligned
  with the CPU reference.
- [ADR-0205](../../../docs/adr/0205-cambi-gpu-feasibility.md) +
  [ADR-0210](../../../docs/adr/0210-cambi-vulkan-integration.md) —
  cambi Vulkan integration (Strategy II, hybrid host/GPU).
  Precision-sensitive `calculate_c_values` + top-K stay on host;
  GPU phases are integer + bit-exact.
- [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  GPU-parity CI gate: per-feature `FEATURE_TOLERANCE` map in
  `scripts/ci/cross_backend_parity_gate.py` is single source of
  truth. Every new GPU twin needs an entry.

## Newly-arrived shipped surfaces (rebase awareness)

- **MS-SSIM `enable_lcs` GPU implementation (T7-35, PR #207 MERGED)**
  — wires the existing CPU `enable_lcs` 15-extra-metrics through
  CUDA + Vulkan + SYCL MS-SSIM kernels. On rebase: ensure the
  option metadata stays declared on the GPU paths even if the
  body is still TODO.
- **`psnr` cross-backend `enable_chroma` option parity (ADR-0453)** —
  `psnr_cuda`, `psnr_sycl`, and `psnr_vulkan` now honour
  `enable_chroma` (default `true`) consistently with the CPU reference.
  Passing `enable_chroma=false` produces luma-only output on all three
  GPU backends. The option default must remain `true`; any change to the
  default or the `n_planes` clamp logic requires a coordinated update
  across all three GPU twins. See CUDA AGENTS.md / Vulkan AGENTS.md
  invariant notes and [ADR-0453](../../../docs/adr/0453-psnr-enable-chroma-gpu-parity.md).
- **MobileSal saliency extractor (T6-2a, PR #208 open, ADR-0218
  placeholder)** — first half of T6-2 (encoder-side ROI bundle).
  DNN-backed; opens sessions through
  [`../dnn/`](../dnn/AGENTS.md).
- **TransNet V2 shot-boundary extractor (T6-3a + T6-3a-followup,
  ADR-0223 + ADR-0257)** — second half of T6-2 bundle. Now ships
  real upstream weights via NTCHW adapter (see
  `transnet_v2.c 100-frame-window contract` invariant above).
- **MobileSal saliency extractor (T6-2a, ADR-0218; smoke-only
  placeholder shipped, real-weights swap deferred per
  [ADR-0257](../../../docs/adr/0257-mobilesal-real-weights-deferred.md)
  and [ADR-0265](../../../docs/adr/0265-u2netp-saliency-replacement-blocked.md))**
  — first half of T6-2 (encoder-side ROI bundle). DNN-backed;
  opens sessions through [`../dnn/`](../dnn/AGENTS.md). Two
  real-weights swap attempts blocked: upstream MobileSal is
  CC BY-NC-SA 4.0 + Google-Drive-walled + RGB-D (ADR-0257), and
  the recommended U-2-Net `u2netp` replacement is also
  Google-Drive-walled and uses ONNX `Resize` which is not on the
  fork's `op_allowlist.c` (ADR-0265). The C-side `input` →
  `saliency_map` tensor-name contract is invariant across both
  blockers; any future drop-in replaces the `.onnx` and bumps the
  registry sha256 without touching this file.
- **TransNet V2 shot-boundary extractor (T6-3a + T6-3a-followup,
  PR #210 MERGED, ADR-0223 + ADR-0257)** — second half of T6-2
  bundle, ~1M params. DNN-backed. Ships real upstream weights via
  NTCHW adapter (see `transnet_v2.c 100-frame-window contract`
  invariant above).
- **FastDVDnet temporal pre-filter (T6-7, PR #203 MERGED, ADR-0215)**
  — 5-frame window pre-filter feeding ssim/ms_ssim. DNN-backed.
- **SVE2 SIMD ports (T7-38, PR #201 MERGED, ADR-0213)**
  — SSIMULACRA 2 PTLR + IIR-blur SVE2; same bit-exact contract
  as the existing NEON ports per
  [ADR-0161](../../../docs/adr/0161-ssimulacra2-simd-bitexact.md)
  / [ADR-0162](../../../docs/adr/0162-ssimulacra2-iir-blur-simd.md)
  / [ADR-0163](../../../docs/adr/0163-ssimulacra2-ptlr-simd.md).
- **`float_ms_ssim` `enable_chroma` (ADR-0461, PR opened 2026-05-16)**:
  `float_ms_ssim.c` has a `bool enable_chroma` field in `MsSsimState`
  and a per-plane loop in `extract()` emitting `float_ms_ssim_cb` /
  `float_ms_ssim_cr`. The default is `false` (luma-only, backward-
  compatible). GPU twins (`_cuda`, `_sycl`, `_vulkan`) do not yet carry
  this option — they are a planned follow-up. If upstream Netflix adds
  any option to `float_ms_ssim.c`, mirror it to all GPU twins in the
  same PR per the twin-parity invariant.
- **Upstream ports**: `feature/motion` options from `b949cebf`
  (T-NEW-1) MERGED via PR #197 (2026-04-29). `feature/speed`
  port from `d3647c73` (`speed_chroma` + `speed_temporal`) is
  PR #213 (open). 32-bit ADM/cpu fallbacks (`8a289703` +
  `1b6c3886`) are PR #212 (open).

- **Per-frame `malloc`/`aligned_malloc` for geometry-sized buffers is forbidden
  on hot paths** (ADR-0452): any buffer whose size is determined by the input
  geometry (`w`, `h`, `stride`) MUST be hoisted to `init_fex` and freed in
  `close_fex`. The geometry is known at init time. Per-frame heap traffic for
  geometry-sized scratch eliminates up to ~79 MB/frame of allocator pressure at
  1080p and causes arena lock contention in threaded mode. Examples: `float_vif`
  hoists `10 × plane_sz` to `VifState::vif_buf` per ADR-0452; `ssimulacra2`
  hoists its workspace similarly. If an upstream port re-introduces a per-frame
  allocation for a geometry-sized buffer, move it to init/close in the same PR.
  Small constant-size (geometry-independent) allocations inside hot paths
  are acceptable but must be justified in the PR description.

- **VIF log2 LUT invariant** (ADR-0500): `VifPublicState.log2_table` is a
  32768-entry table (64 KB, not 65537 entries / 128 KB). The normalisation in
  `log2_32` / `log2_64` always produces indices in `[32768..65535]`; the mask
  `& (VIF_LOG2_TABLE_SIZE - 1u)` strips bit 15 to get a `[0..32767]` index.
  If upstream Netflix changes the LUT size or the normalisation logic, audit
  the mask in `integer_vif.h` and the three gather sites in `vif_avx512.c`
  before merging. The new `log_generate` fills `log2_table[i] = log2f(32768+i)*2048`;
  the original filled `log2_table[i] = log2f(i)*2048` for `i` in `[32767..65535]`.
- **compute_vif filter-cache parameter** (ADR-0500): `compute_vif` in `vif.c`
  accepts two nullable trailing parameters `precomputed_filters` /
  `precomputed_filter_widths`. The `float_vif.c` caller passes pre-computed
  Gaussian coefficients from `VifState.filter_cache` / `filter_width_cache`
  (populated once in `init()`). The internal `vifdiff` path passes NULL to
  retain the original per-call `vif_get_filter()` path. If upstream changes
  `compute_vif`'s signature, both the declaration in `vif.h` and the internal
  call in `vif.c` need updating.

- **`feature_extractor_list[]` is exactly-once** (ADR-0544):
  the static `feature_extractor_list[]` in `feature_extractor.c` must
  list every `&vmaf_fex_*` symbol **at most once** under its correct
  `#ifdef HAVE_*` guard. A duplicate is silently masked by the
  first-match `vmaf_get_feature_extractor_by_name()` but breaks the
  ctx-pool's iterator dispatch: the pool's `get_fex_list_entry()` keys
  on `fex->name` so the same name registered twice still collapses to
  one pool entry there, but any caller that walks the registry
  directly (e.g. the iterator dispatch path that fans out
  `vmaf_use_features_from_model`) allocates one entry per registered
  pointer and runs `init`/`extract`/`flush` once per copy per picture.
  The audit helper `vmaf_feature_extractor_list_audit()` runs from
  `vmaf_init()` and returns `-EINVAL` on duplicates; the
  `test_feature_extractor_list_no_duplicates` C unit test exercises
  it on the live registry. When adding a new backend or extractor:
  add **one** `extern VmafFeatureExtractor vmaf_fex_*_<bk>` decl and
  **one** `&vmaf_fex_*_<bk>` entry inside the matching `#if HAVE_<BK>`
  block — do not paste the whole `&vmaf_fex_*` cluster.

- **Register extractors in `feature_extractor.cpp`, NOT
  `feature_extractor.c`** (ADR-0846 / ADR-1110): the build compiles
  the C++23 `feature_extractor.cpp` (see `core/src/meson.build`); the
  old `feature_extractor.c` is a **dead twin** left over from the
  ADR-0846 conversion and is *not* in any build target. Editing only
  the `.c` makes `vmaf_get_feature_extractor_by_name()` return NULL
  (the symptom: `problem loading feature extractor: <name>` from the
  CLI). When adding a new extractor, put the `extern` decl + the
  `feature_extractor_list[]` entry in the `.cpp`. (The stale `.c`
  should be removed in a separate cleanup.)

- **`delta_e_itp` PQ-only invariant** (ADR-1110): the ΔE-ITP
  extractor (`delta_e_itp.c`) ships the **PQ (ST-2084) transfer
  only**; `init()` rejects any `transfer` other than `pq` with
  `-EINVAL`. The PQ matrices/constants are triple-sourced against
  ITU-R BT.2124-0; the HLG (Annex 3) and BT.1886/SDR (Conversion 5)
  paths are single-sourced and intentionally deferred. Do **not**
  loosen the `transfer` guard to accept `hlg`/`bt1886` without first
  cross-validating those constants against an independent source. The
  PQ EOTF/EOTF⁻¹ live in `delta_e_itp_math.h`; out-of-gamut LMS/ICtCp
  values are deliberately **not clamped** (BT.2124 Annex 4) — the unit
  test's places=4 ITP-triple oracle depends on this. The
  `scale_chroma_planes` / `scale_chroma_planes_hbd` helpers are
  copied verbatim from `ciede.c` but are independent copies.
- **`y_funque_plus` atoms-only invariants** (ADR-1114): the Y-FUNQUE+
  extractor (`y_funque_plus.c`) has **no upstream twin** and is a
  clean-room reimplementation from the papers (MIT `funque_plus` used
  only as a cross-check). It ships the **three atoms only**
  (`y_funque_plus_ms_ssim` / `_dlm` / `_mad`); the fused ScaledSVR MOS
  score is deliberately **not** shipped (upstream commits no frozen
  regressor — see the Deferred row in `docs/state.md`). Load-bearing
  details a rebase or "cleanup" must not silently break:
  1. **Haar butterfly** uses the pywt `'haar'` convention
     `cH=(a+b-c-d)/2`, `cV=(a-b+c-d)/2` (verified directly against
     `pywt.dwt2`). The design dossier prose listed H/V **swapped** — do
     not "fix" the code to match the stale prose; the code is correct
     and DLM's psi-angle mask depends on it.
  2. **DLM num/den abs-asymmetry** (`yf_dlm_pool`): the numerator pools
     `rest^3` **without** abs while the denominator pools the ref detail
     **with** abs (mirrors upstream `pyr_features.py:54/61`). Symmetrising
     them is a real behaviour change, not a cleanup.
  3. **OpenCV `INTER_CUBIC`** (Keys cubic `a=-0.75`, src coord `2i+0.5`,
     `BORDER_REPLICATE`) is the dominant cross-host parity component;
     the TU compiles in its own static lib with `-ffp-contract=off` (the
     same rationale as `ssimulacra2`). Keep both.
  4. The Nadenau Y-channel CSF weights **only** the detail subbands;
     approx subbands are never weighted. The analytic constants
     (`a=1/256`, `b_Y=-5.4715e-3`, `c_Y=1.91`) regenerate the official
     lookup table to 8 dp. Oracle values in `core/test/test_y_funque_plus.c`
     were re-derived against a `pywt` + OpenCV reference at places=4.
