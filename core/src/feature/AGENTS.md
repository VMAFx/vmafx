<!-- markdownlint-disable MD060 -->
# AGENTS.md — core/src/feature

Orientation for agents working on feature extractors (VMAF metric
components: VIF, ADM, motion, integer-valued VIF/ADM/motion, CIEDE, CAMBI,
PSNR, SSIM, MS-SSIM, LPIPS, …). Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

Every VMAF "feature" is small C module with `VmafFeatureExtractor`
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
- **Bit-exactness with scalar reference** is non-negotiable for SIMD
  paths. Reductions, FMA-ordering, and rounding must match scalar path
  exactly — no "close enough". See
  [add-simd-path](../../../.claude/skills/add-simd-path/SKILL.md) for
  dispatch pattern (`cpu.c` + feature_name_avx2.c + feature_name_avx512.c).
- **CUDA / SYCL kernels** should match CPU reference within
  documented tolerance. If kernel cannot match exactly, file snapshot
  justification in commit message and regenerate
  `testdata/scores_cpu_*.json` via
  [`/regen-snapshots`](../../../.claude/skills/regen-snapshots/SKILL.md).
- **Registration is discoverable by both name and provided-feature-name**:
  `vmaf_get_feature_extractor_by_name()` and
  `vmaf_get_feature_extractor_by_feature_name()`. Both must resolve.
  - **GPU/Metal twins live in `feature_extractor.cpp`'s `#if HAVE_*`
    blocks, NOT in parallel file.** registry was `feature_extractor.c`
    until PR #875 introduced compiled `.cpp` twin; for window both
    files existed and diverged, and `speed_{chroma,temporal}_{cuda,
    sycl,hip}` registrations were left behind in the dead `.c` — so the
    kernels compiled but `by_name("speed_chroma_cuda")` returned NULL and
    SpEED silently fell back to CPU. `.c` is now deleted; when you add
    GPU twin, add its `extern` + array entry to matching `#if HAVE_*`
    block in `.cpp` AND assert resolution in
    `test/test_feature_extractor.c`. registered-but-unresolvable twin is
    silent correctness bug, not build error.
- **Options tables** must have non-NULL `help` for every entry; see
  [../../test/test_lpips.c](../../test/test_lpips.c) for unit-test
  pattern that enforces this.
- **DNN-backed extractors** open sessions through
  [src/dnn/](../dnn/AGENTS.md) — never call ONNX Runtime directly from
  `feature_*.c` file.

## Workflows

| Task | Skill |
| --- | --- |
| Add a feature extractor | [add-feature-extractor](../../../.claude/skills/add-feature-extractor/SKILL.md) |
| Add a SIMD path | [add-simd-path](../../../.claude/skills/add-simd-path/SKILL.md) |
| Cross-backend diff | [cross-backend-diff](../../../.claude/skills/cross-backend-diff/SKILL.md) |
| Profile a hot path | [profile-hotpath](../../../.claude/skills/profile-hotpath/SKILL.md) |

## Rebase-sensitive invariants

- **CAMBI bounded searches and live private helpers** (ADR-0205 / ADR-1146):
  `cambi.c` is strict-clean: it contains no `NOLINT` or Cppcheck suppression.
  Preserve the 16-step TVI bisection, the `UINT16_MAX`-bounded VLT scan, and
  the `n`/partition-span bounds on quick-select without changing comparison,
  pivot, swap, or accumulation order. The shared extractor callback ABI stays
  mutable; `read_only_picture_view()` is the const-view adapter Cppcheck can
  verify. All ten helpers declared in `cambi_internal.h` must remain exercised
  by real CPU/reference paths as well as available to GPU twins; do not replace
  those calls with analyzer annotations. Keep the compact `CAMBI_OPTION`
  descriptors equivalent to the public option table. See
  [measured source and binary equivalence](../../../docs/research/2043-cambi-production-lint-2026-09-08.md).

- **CAMBI heatmap paths are UTF-8 on Windows** (ADR-1182):
  `mkdirp.cpp` must create each component through `vmaf_mkdir_utf8`, and
  `cambi.c::open_heatmaps` must open every `.gray` file through
  `vmaf_open_utf8`. Keep `test_open_heatmaps_utf8_path` as a production-seam
  regression; a helper-only path test does not protect this call-site wiring.

- **Floating-point VIF lint decomposition** (ADR-0141 / ADR-1142):
  `vif.c` keeps ten-plane aligned layout and original convolution,
  decimation, statistic and scale-reduction order. Preserve float intermediate
  values before double score storage. `vif.h` declares all three legacy
  external symbols, including `vifdiff`; do not make them static to satisfy
  per-TU lint. temporal first-frame placeholders and offset/difference/
  previous-frame-copy order are unchanged. Debug dumps write one initialized
  float for each reduced numerator/denominator. See
  [focused investigation](../../../docs/research/vif-native-lint-2026-09-08.md).

- **CAMBI GPU twins mirror `cambi.c`'s host-side semantics, not "something
  reasonable" (branch `fix/gpu-cambi-parity-drift`, 2026-09-05)**. `cambi.c` is
  pinned by Netflix golden gate, so when twin and reference disagree
  twin is always side that moves. Two places where natural GPU idiom
  is wrong answer, in both `cuda/integer_cambi/cambi_score.cu` and
  `sycl/integer_cambi_sycl.cpp`:
  - 7×7 zero-derivative box sum must contribute **zero** for taps outside
    image, because `get_spatial_mask_for_index`'s summed-area table
    zero-pads (`compute_dp_row` with `actual_width = 0`). Clamping to edge
    pixel — usual GPU border idiom — inflates sum, because edge pixels
    are `zero_derivative = 1` by construction, and over-marks banding within
    three pixels of every border.
  - vertical `filter_mode` pass must skip `y == 0` and `y == height - 1`.
    `cambi.c::filter_mode` writes back only under `if (i > 1)`, so it fills
    output rows `1 .. height-2` and leaves both border rows at their
    **pre-filter** values. Because V pass writes into buffer H pass
    read from, early return preserves exactly those pixels.

  `cambi_high_res_speedup` (`hrs`) is part of same contract and has three
  separate effects that must all be present in twin: resolution against
  encode pixel count at init, halving adjusted window, and one extra
  decimation before scale 0. default model `vmaf_v1.0.16_3d0h` sets
  `hrs=1080`, so omitting any of them silently changes every `>= 1080p` score.

  **Parity fixtures must have real-content structure.** original
  `test_{cuda,sycl}_cambi_parity.c` fixture was quantised gradient: constant
  down every column and along every border. Both defects above are invisible on
  it, and gates stayed green for months while real content drifted 2.7e-3.
  added "textured" fixture (horizontal bands + vertical ramp + deterministic
  LCG dither + inverted border ring) fails at 2.11e-2 against pre-fix kernel.
  Keep both fixtures; new CAMBI twin needs to pass both.
- **Model options gate GPU twin selection (ADR-1183)**: every option
  model sets in its feature options dictionary must be present in
  chosen GPU twin's option table. If GPU twin lacks any requested option
  (e.g. `integer_adm_cuda` lacking `adm_csf_mode`),
  `vmaf_use_features_from_model` in `core/src/libvmaf.c` rejects GPU twin
  and dispatches extractor to CPU reference. Option parsing in
  `feature_extractor.cpp` rejects any unknown dictionary keys with
  `-EINVAL`. On rebase, do not bypass this validation or revert to silent
  option omission.
- **ANSNR / float_ansnr feature extractor removal (ADR-0865)**:
  `ansnr` and `float_ansnr` (CPU scalar, AVX2, AVX-512, NEON, CUDA, HIP, SYCL,
  Metal) were sunset and completely removed from library. ANSNR is legacy
  pre-VMAF metric (circa 2001) never adopted in any production VMAF model.
  On any rebase or upstream sync from Netflix/vmaf:
  - If upstream re-introduces `ansnr` or `float_ansnr` sources under `libvmaf/src/feature/`
    (`ansnr.c`, `ansnr.h`, `ansnr_options.h`, `ansnr_tools.c`, `ansnr_tools.h`,
    `float_ansnr.c`, or SIMD files `ansnr_avx2.c`, `ansnr_avx512.c`, `ansnr_neon.c`),
    re-drop them.
  - Keep `feature_extractor.cpp` free of any `ansnr` registration symbols.
  - Keep dispatch registries and feature lists free of `ansnr` / `float_ansnr`.

- `ssimulacra2.c` is fork-local (not upstream). It embeds several
  constant tables that must stay in lock-step with libjxl even across
  rebase:
  - **Opsin absorbance matrix** (`kM00`…`kM22`) and bias `kB` — see
    libjxl `lib/jxl/opsin_params.h`.
  - **`MakePositiveXYB` offsets** — `B=(B-Y)+0.55`, `X*=14`, `X+=0.42`,
    `Y+=0.01`.
  - **108 pooling weights (`kWeights[]`)** and final polynomial
    transform (`0.9562382…`, `2.326765…`, `-0.0208845…`,
    `6.2484966e-05`, `0.6276336…`) — from `tools/ssimulacra2.cc`.
  - **FastGaussian coefficient derivation** — `3.2795·σ + 0.2546`
    radius, k∈{1,3,5}, Cramer's-rule 3×3 solve for β. Any drift from
    libjxl's `lib/jxl/gauss_blur.cc` formulas breaks bit-exactness of
    scalar blur.
  If libjxl changes any of these upstream, update scalar extractor
  in same PR (same for SIMD follow-ups, which will mirror
  same coefficient path).
- **CIEDE chroma-upsample subsample flags (FORK DIVERGES FROM
  UPSTREAM, 2026-06-27)**: `ciede.c` `scale_chroma_planes` /
  `scale_chroma_planes_hbd` must key *horizontal* sample-index
  divisor off `ss_hor` and *vertical* row advance off `ss_ver`.
  **Upstream Netflix carries these two flags transposed** (horizontal
  off `ss_ver`, vertical off `ss_hor`) — that bug heap-OOB-reads and
  mis-scores YUV422P (half-width / full-height chroma). YUV420P is
  no-op (both flags set) and YUV444P never calls function, so
  Netflix golden CIEDE2000 pair (420P) cannot detect regression
  here. On rebase, do NOT let upstream sync revert flags back to
  transposed form. Guarded by `test_ciede_scale_chroma_422_8b` and
  `test_ciede_scale_chroma_422_16b` in `core/test/test_ciede.c` (both
  fail against upstream form).
- **integer SSIM `samplemax²` must be widened to `double` (16-bpc
  parity, 2026-06-27)**: `integer_ssim.c` `ssim_reduce_row_range`
  computes `c1/c2 = sm * sm * K * w_d²` via hoisted
  `const double sm = (double)samplemax;`. It must NOT regress to
  `int samplemax * samplemax` form: for 16-bpc `samplemax = 65535` and
  `65535² > INT_MAX` overflows `int` (UB, wraps negative), corrupting
  stability constants and diverging from CUDA/HIP/SYCL twins
  (which already use `int64_t`/`double`). 8/10/12-bpc are bit-unchanged
  by cast. Guarded by `test_ssim_16bit_distorted_in_range` in
  `core/test/test_ssim_coverage.c` (fails against `int` form).
- **MS-SSIM decimate LPF coefficients**: 9-tap 9/7 biorthogonal
  filter table (`ms_ssim_lpf_h` / `ms_ssim_lpf_v`) appears verbatim in
  four TUs that must stay byte-identical for bit-exactness
  contract — `ms_ssim_decimate.c`, `x86/ms_ssim_decimate_avx2.c`,
  `x86/ms_ssim_decimate_avx512.c`, and
  `arm64/ms_ssim_decimate_neon.c`. source of truth upstream is
  `g_lpf_h` / `g_lpf_v` in `ms_ssim.c`. If rebase touches any of
  those five files, diff all five against each other before pushing.
  See [ADR-0125](../../../docs/adr/0125-ms-ssim-decimate-simd.md).
- **KBND_SYMMETRIC mirror**: `ms_ssim_decimate_mirror` is duplicated
  across same four TUs and must match upstream
  `KBND_SYMMETRIC` branch in `iqa/convolve.c`. Changing boundary
  semantics in any one of them breaks bit-identity.
- **MS-SSIM decimate `-ENOMEM` contract** (ADR-0877, 2026-05-30):
  `malloc`-failure branch in all four MS-SSIM decimate TUs returns
  `-ENOMEM` (negative POSIX errno), matching libvmaf internal
  convention documented in `ms_ssim_decimate.h`. On rebase, if
  upstream port re-introduces `return -1` here, restore
  `-ENOMEM` form (and `#include <errno.h>`). Caller
  `ms_ssim.c:207-208` uses truthy check so either compiles. Errno
  form is required by ADR-0877 for higher-level error reporters
  (Go controller, MCP server, ffmpeg filter) that surface cause.
- **SSIM / MS-SSIM SIMD bit-exactness invariants** (fork-local,
  ADR-0138 + ADR-0139 + ADR-0140): AVX2 / AVX-512 / NEON paths
  in `x86/ssim_avx2.c` / `x86/ssim_avx512.c` /
  `arm64/ssim_neon.c` / `x86/convolve_avx2.c` /
  `x86/convolve_avx512.c` / `arm64/convolve_neon.c` are
  bit-identical to scalar reference under FLT_EVAL_METHOD == 0.
  Two rules are load-bearing and must be preserved on rebase:
  1. **Convolve taps**: each tap is *single-rounded `float * float`
     → widen to `double` → `double` add*. No FMA. Mirrors scalar
     `sum += img[i] * k[j]` in
     [`iqa/convolve.c`](iqa/convolve.c). Changing scalar to `fmaf`
     or to double-mul pattern requires matching all three SIMD
     variants.
  2. **SSIM accumulate**: `2.0 *` literal in
     [`ssim_accumulate_default_scalar`](iqa/ssim_tools.c)
     (`2.0 * ref_mu[i] * cmp_mu[i] + C1` and
     `2.0 * srsc + C2`) is C `double` literal, which promotes
     float operands to double before multiply. All three
     SIMD accumulators do `2.0 *` numerator + division + final
     `l*c*s` product per-lane in scalar double to match. If
     upstream ever changes `2.0` literal to `2.0f` (or
     restructures l/c numerators), all three SIMD variants
     need matching rewrite.
  3. **AVX-512 vector-double per-lane reduction**: AVX-512
     accumulator (`x86/ssim_avx512.c`) computes `lv`, `cv`, `sv`,
     and `lv*cv*sv` lane-wise in two 8-wide `__m512d` passes via
     `_mm512_cvtps_pd` widening + plain `_mm512_mul_pd` /
     `_mm512_add_pd` / `_mm512_div_pd` (no `_mm512_fmadd_pd`),
     then spills to `_Alignas(64) double[16]×4` and accumulates
     left-to-right scalar into `local_*`. vector-double form
     is bit-identical to scalar lane-wise by IEEE-754, but only
     because: () op order matches scalar's parse — `((2*rm)*cm
     /l_den`, etc.; (b) no FMA contraction; (c) running
     sum stays scalar left-to-right, lane 0 → lane 15. Tree
     reductions over 16-lane block break ADR-0139's
     running-sum invariant against scalar and are forbidden
     unless scalar itself is rewritten in lockstep. AVX2 and NEON
     stay on per-lane scalar path (`ssim_accumulate_lane`)
     for now — vectorising them with same `__m256d` /
     `float64x2_t` widening would follow same three rules.
- **`simd_dx.h` DX macros** (fork-local, ADR-0140): header
  [`simd_dx.h`](simd_dx.h) is fork-internal and has no upstream
  equivalent. On rebase, keep fork's version. macros
  (`SIMD_WIDEN_ADD_F32_F64_*`, `SIMD_ALIGNED_F32_BUF_*`,
  `SIMD_LANES_*`) encode ADR-0138 / ADR-0139 bit-exactness patterns
  by construction — changing their expansion without auditing
  three SSIM / convolve consumers (`ssim_accumulate_*`,
  `iqa_convolve_*`) is bit-exactness break waiting to happen.
  Macro names are ISA-suffixed on purpose; do not collapse them
  into cross-ISA aliases (fork's SIMD policy rules out
  Highway / simde / xsimd — see user memory
  `feedback_simd_dx_scope.md`).
- **`feature_collector.c` mount/unmount traversal**: fork rewrites
  `vmaf_feature_collector_mount_model` and `unmount_model` to walk
  local cursor instead of advancing pointer-to-head — upstream
  [Netflix#1406](https://github.com/Netflix/vmaf/pull/1406) is still
  OPEN as of 2026-04-20 and its body corrupts list on ≥3 mounted
  models. `unmount_model` additionally returns `-ENOENT` (not
  `-EINVAL`) for "model not mounted". If upstream ever merges #1406,
  **keep fork's version on conflict** — traversal is correct
  and errno split lets callers distinguish misuse from not-found.
  Test coverage in [`../../test/test_feature_collector.c`](../../test/test_feature_collector.c)
  uses shared `load_three_test_models` / `destroy_three_test_models`
  helpers; upstream's PR inlines 60 LoC of per-model scaffolding that
  would trip clang-tidy `readability-function-size` (JPL-P10 rule 4).
  See [ADR-0132](../../../docs/adr/0132-port-netflix-1406-feature-collector-model-list.md)
  and [rebase-notes 0031](../../../docs/rebase-notes.md).
- **Generalised AVX convolve scanline helpers** (fork-local,
  ADR-0143): four `convolution_f32_avx_s_1d_*_scanline`
  helpers in [`common/convolution_avx.c`](common/convolution_avx.c)
  are `static` in fork (upstream leaves them extern out of
  habit). Strides are `ptrdiff_t` inside helpers, `int` at
  public `convolution_f32_avx_*_s` wrappers, with `(ptrdiff_t)`
  casts at pointer-offset multiplication sites. On rebase: keep
  fork's `static` and `ptrdiff_t` unless upstream adopts them.
  See [ADR-0143](../../../docs/adr/0143-port-netflix-f3a628b4-generalized-avx-convolve.md)
  and [rebase-notes 0036](../../../docs/rebase-notes.md).
- **`motion_v2` public option-surface duplicates motion v1**
  (fork-local, ADR-0337):
  [`integer_motion_v2.c`](integer_motion_v2.c) registers its own
  `VmafOption[]` table for seven motion knobs
  (`motion_force_zero`, `motion_blend_factor`, `motion_blend_offset`,
  `motion_fps_weight`, `motion_max_val`, `motion_five_frame_window`,
  `motion_moving_average`) — duplicating motion v1's
  [`integer_motion.c`](integer_motion.c) surface byte-for-byte
  against upstream Netflix `4e469601`. duplication is
  deliberate; v1 and v2 are independent extractors with independent
  output namespaces (`VMAF_integer_feature_motion*_score` vs
  `…_v2_score`). On rebase: when touching one extractor's option
  help string, touch other; when upstream touches option
  table, port change to **both** extractors. ADR-0141 catches
  drift on next edit.
- **`motion_v2` rejects `motion_five_frame_window=true`**
  (fork-local, ADR-0337): `init()` returns `-ENOTSUP` and logs
  pointer at ADR. 5-frame mode requires `prev_prev_ref`
  field on `VmafFeatureExtractor` plus `n_threads * 2 + 2`
  picture-pool sizing in `vmaf_read_pictures` (upstream `a2b59b77`)
  that conflicts with fork's [ADR-0152](../../../docs/adr/0152-vmaf-read-pictures-monotonic-index.md)
  `read_pictures*` decomposition. picture-pool refactor is
  deferred to its own PR. Mirrors [ADR-0219](../../../docs/adr/0219-motion3-gpu-coverage.md)
  §Decision's GPU motion3 `-ENOTSUP` precedent. On rebase: when
  picture-pool refactor PR lands, flip `-ENOTSUP` guard to
  `prev_prev_ref` lookup and reinstate `min_idx = 5? 2 : 1`
  branching in `flush()` (currently collapsed to `min_idx = 1`
  per ADR-0337's deferral). See
  [rebase-notes ADR-0337](../../../docs/rebase-notes.md) for
  deferred-hunks ledger.
- **`motion_v2` NEON shift semantics** (fork-local, ADR-0145):
  [`arm64/motion_v2_neon.c`](arm64/motion_v2_neon.c) uses
  **arithmetic** right-shift throughout (`vshrq_n_s64(v, 16)` for
  Phase-2 known shift, `vshlq_s64(v, -(int64_t)bpc)` for
  Phase-1 runtime shift). fork's AVX2 variant
  [`x86/motion_v2_avx2.c`](x86/motion_v2_avx2.c) uses
  `_mm256_srlv_epi64` (*logical*) which can diverge from scalar on
  negative-diff pixels. NEON matches scalar, AVX2 does not — this
  is intentional until AVX2 audit lands. On rebase: keep
  arithmetic-shift form in NEON; do NOT port AVX2's logical pattern
  even if it looks simpler. 4-lane stride + scalar tails on both
  sides of row are load-bearing for x_conv edge-mirror
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
  `ssim_init_args` around explicit `struct ssim_workspace`;
  `vif_statistic_s_avx2` in
  [`x86/vif_statistic_avx2.c`](x86/vif_statistic_avx2.c) is split
  into `vif_stat_simd8_compute` + `vif_stat_simd8_reduce` around
  explicit `struct vif_simd8_lane` that carries `__m256` lane
  state between two halves. **Load-bearing**: per-lane
  scalar-float reduction via 32-byte aligned `tmp_n[8]` / `tmp_d[8]`
  in `vif_stat_simd8_reduce` preserves ADR-0139 exactly;
  convolve pass ordering in `iqa_convolve_1d_separable` preserves
  ADR-0138 exactly. On rebase: if upstream rewrites any of these
  three functions, prefer upstream's shape **only** if it maintains
  both invariants; otherwise keep fork's split and re-document
  divergence in
  [rebase-notes 0039](../../../docs/rebase-notes.md). Also:
  TU-static rename `_calc_scale` → `iqa_calc_scale` in
  `iqa/convolve.c` is fork-local — keep on rebase. See
  [ADR-0146](../../../docs/adr/0146-nolint-sweep-function-size.md).
- **IQA reserved-identifier rename** (fork-local, ADR-0148):
  every `_iqa_*` / `struct _kernel` / `_ssim_int` /
  `_map_reduce` / `_map` / `_reduce` / `_context` /
  `_ms_ssim_*` / `_ssim_*` / `_alloc_buffers` /
  `_free_buffers` symbol and four underscore-prefixed
  header guards (`_CONVOLVE_H_`, `_DECIMATE_H_`,
  `_SSIM_TOOLS_H_`, `__VMAF_MS_SSIM_DECIMATE_H__`) was renamed
  to its non-reserved spelling. IQA tree is now baseline
  lint-clean. **Load-bearing NOLINTs** (do not collapse on
  rebase): scoped
  `NOLINTBEGIN/END(clang-analyzer-security.ArrayBound)` around
  inner kernel loops in `ssim_accumulate_row` and
  `ssim_reduce_row_range` of
  [`integer_ssim.c`](integer_ssim.c) —
  `k_min`/`k_max` clamping is provably correct but
  analyzer can't follow it across helper boundary; scoped
  `NOLINTBEGIN/END(clang-analyzer-unix.Malloc)` around
  `check_simd_variant` and `check_case` in
  [`../../test/test_iqa_convolve.c`](../../test/test_iqa_convolve.c)
  — test exits process on failure path; small allocations
  leak by design at test end; cross-TU
  `NOLINTNEXTLINE(misc-use-internal-linkage)` on `compute_ssim`
  in [`ssim.c`](ssim.c) and `compute_ms_ssim` in
  [`ms_ssim.c`](ms_ssim.c) — declared in `ssim.h` /
  `ms_ssim.h`, called from `float_ssim.c` /
  `float_ms_ssim.c`; clang-tidy runs per-TU and can't see
  bridge. On rebase, keep all these brackets verbatim. See
  [ADR-0148](../../../docs/adr/0148-iqa-rename-and-cleanup.md)
  and [rebase-notes 0041](../../../docs/rebase-notes.md).

- **psnr bucket lint shape** (ADR-1142 ratchet, ADR-0278
  citations): [`psnr.c`](psnr.c) includes its own
  [`psnr.h`](psnr.h) — upstream does not, and include is
  what declares `compute_psnr()`'s external linkage to
  clang-tidy (alternative used for `compute_ssim` /
  `compute_ms_ssim` is NOLINT; psnr has real header, so it
  uses it). [`integer_psnr.c`](integer_psnr.c) and
  [`float_psnr.c`](float_psnr.c) carry
  `NOLINTNEXTLINE(misc-use-internal-linkage)` on
  `vmaf_fex_psnr` / `vmaf_fex_float_psnr` — same cross-TU
  registry pattern as `cambi.c` and `float_ssim.c` — and their
  `provided_features[]` sentinels plus `options[]`
  terminator keep upstream's `NULL` spelling: per
  [ADR-1138](../../../docs/adr/1138-c-translation-units-keep-null.md) C
  translation unit never uses C23 `nullptr` keyword,
  because required `Build — Windows MSVC + CUDA` lane
  compiles these files with cl.exe and MSVC's documented
  `/std:clatest` feature set does not include it. Both files
  therefore carry file-scoped
  `/* NOLINTBEGIN(modernize-use-nullptr) … ADR-1138. */` …
  `NOLINTEND` bracket instead — keep it spanning whole
  file, and do not "modernise" `NULL`s inside it.
  [`psnr_tools.cpp`](psnr_tools.cpp) is C++ and *does* use
  `nullptr`/designated initialisers; its `kFormatTable` peak /
  psnr_max values are byte-identical to upstream's `strcmp`
  ladder and must stay so — fork's
  `--feature psnr --precision=max` output on `src01` pair
  is asserted byte-identical across this refactor. See
  [ADR-1142](../../../docs/adr/1142-whole-codebase-standards.md),
  [ADR-1138](../../../docs/adr/1138-c-translation-units-keep-null.md),
  [ADR-0278](../../../docs/adr/0278-t7-5-nolint-sweep.md)
  and [`docs/rebase-notes.md`](../../../docs/rebase-notes.md).

- **`integer_adm.c` i4_adm_cm int32 rounding overflow**
  (fork-inherited, ADR-0155): both `add_bef_shift_flt[]`
  initialiser loops in
  [`integer_adm.c`](integer_adm.c) (scales 1–3) assign
  `1u << 31 = 0x80000000` into `int32_t`, which wraps to
  `-2147483648`. rounding term is sign-negated; every
  downstream `(prod + add_bef_shift) >> 32` subtracts 2^31
  instead of adding it. **Deliberately preserved** — buggy
  arithmetic is encoded in Netflix golden
  `assertAlmostEqual` values (project hard rule #1 /
  [ADR-0024](../../../docs/adr/0024-netflix-golden-preserved.md)).
  Do NOT widen `add_bef_shift_flt[]` to `uint32_t` or `int64_t`
  without coordinated Netflix-authored golden-number update
  ([ADR-0142](../../../docs/adr/0142-port-netflix-18e8f1c5-vif-sigma-nsq.md)
  carve-out). Netflix upstream #955 is OPEN since 2020 with no
  maintainer response — until it closes with fix,
  overflow stays. See
  [ADR-0155](../../../docs/adr/0155-adm-i4-rounding-deferred-netflix-955.md)
  and [rebase-notes 0048](../../../docs/rebase-notes.md).
  CUDA mirrors name the same negative value directly as `INT32_MIN` in
  `cuda/integer_adm/adm_csf.cu` and both fused paths in
  `cuda/integer_adm/adm_cm.cu`. Do not restore the `1u << 31`
  unsigned-to-signed conversion there: NVCC diagnoses it as `#68-D`, while
  widening it would violate this numerical invariant.

- **`integer_adm.c` DWT mirror table for tiny extents** (fork-only fix,
  [Research-2063](../../../docs/research/2063-upstream-sync-2026-09-adm-vif-simd.md)):
  `dwt2_src_indices_1d()` starts its mirrored tail at
  `(n_half > 2u) ? n_half - 2u : 1u` and bounds the first loop with
  `i + 2 < n_half`. Upstream's `n_half - 2` restarts the tail at 0 when
  `n_half == 2` (scale 3 for any frame dimension from 17 to 32), replaces the
  `{1, 0, 1, 2}` mirror with `{-1, 0, 1, 2}` and reads index -1 before the
  band and before the `tmp_ref` allocation. `init_buffers()` also zeroes
  `data_buf` (upstream `1786bd961`), but only as defence in depth: the
  zeroing does not make upstream's bound safe. Guarded by
  `test_integer_adm_tiny_frames`, which the ASan lane aborts on the old bound.

- **`integer_adm` GPU row-level rounding invariant** (fork-local, ADR-1167):
  In integer ADM contrast masking kernels (`cuda/integer_adm/adm_cm.cu` and
  `hip/integer_adm/adm_cm.hip`), inner accumulation rounding shift
  `(row_total + add_shift_inner_accum) >> shift_inner_accum` must NEVER be
  distributed across warp reduction or per-thread reduction. Bitwise right-shift
  with rounding bias is non-linear and non-distributive over addition.
  kernel must accumulate all columns of row in 64-bit precision across
  entire width `[start_col, end_col)` before applying shift once per row.
  Kernel launch grids must use `gridDim.x = 1` to ensure single-block/warp
  row traversal. Furthermore, border row selection at `i == 0 && top <= 0`
  must use explicit absolute indices `{row_top, row_bot, col_l, col_r}` and
  evaluate `csf_a` at row 0 center (`i * src_stride + j`), never walking running
  pointer offsets. See [ADR-1167](../../../docs/adr/1167-adm-cm-row-level-rounding.md).

- **`integer_adm.c` / `adm_tools.c` are restructured upstream-mirror
  files** (ADR-1141, 2026-09-02): every kernel expression is verbatim
  but code no longer lines up textually with Netflix/vmaf — re-port
  upstream hunks by hand into owning helper (function map in
  [rebase-notes](../../../docs/rebase-notes.md)
  "refactor/c-rework-adm"). Invariants rebase or follow-up must
  keep: (1) `adm_cm_thresh()` / `i4_adm_cm_thresh()` /
  `adm_cm_thresh3x3_s()` are closed form of nine upstream
  `ADM_CM_THRESH_S_*` corner / edge / interior macros — mirror-to-1
  before first edge, clamp-to-last past last edge, nine terms in
  macro order (float twin's summation order is golden-gated);
  `(int16_t)` cast on integer centre term truncates on purpose.
  (2) border branch is predicate pair `left_edge = left <= 0` /
  `right_edge = right > w - 1` — do not restore upstream's four-way
  branch (its unreachable third arm read `rfactor[]` out of bounds).
  (3) ADR-0155 rounding terms live in `i4_adm_round_terms()`
  (`int32_t`, sign-negated for scales 1..3); `i4_shift_dst[]` /
  `i4_shift_flt[]` tables are file-scope. (4) `adm_decouple()` /
  `adm_decouple_s123()` keep mutable `int32_t *lut` parameter of
  `AdmState` / SIMD-twin prototype behind cited
  `readability-non-const-parameter` + `cppcheck constParameterCallback`
  pair; `extract()` keeps its frozen `VmafFeatureExtractor::extract`
  prototype same way. (5) `adm_dwt2_s()` in `adm_tools.c` stays one
  function under ADR-1057 `optimize("-ffp-contract=off")` /
  `#pragma clang fp contract(off)` bracket; never share DWT helpers
  between it and `adm_dwt2_lo_s()`. (6) Float accumulators in
  `adm_csf_den_scale_s()` / `adm_cm_s()` stay `float` (`adm_fold3_s()`);
  only `adm_sum_cube_s()` is `double` (ADR-0418). (7) Both C TUs keep
  `NULL` under file-scoped `NOLINTBEGIN/END(modernize-use-nullptr)`
  bracket (ADR-1138); keep `NOLINTEND` line at end of file.
  Bit-exactness proof for any further change: rerun 62-case
  `--precision max` CLI matrix from ADR-1141 research digest
  against baseline binary — goldens alone do not reach
  small-scale border branches.

- **`psnr_hvs` AVX2 DCT bit-exactness** (fork-local, ADR-0159):
  [`x86/psnr_hvs_avx2.c`](x86/psnr_hvs_avx2.c) vectorizes
  Xiph/Daala 8×8 integer DCT across 8 rows in parallel
  (`__m256i`, 8× int32) via **butterfly → transpose → butterfly
  → transpose**. Byte-identical `od_coeff` output to scalar
  under `FLT_EVAL_METHOD == 0`; float accumulators (means /
  variances / mask / error) kept scalar by construction per
  ADR-0139 precedent. **On rebase**: never introduce
  horizontal-reduce vectorization of float accumulators
  without replicating per-lane scalar-float reduction
  pattern. Keep `#pragma STDC FP_CONTRACT OFF` at TU
  header — removing it allows `fmaf` and breaks 1-ulp
  guarantee. scalar TU
  [`third_party/xiph/psnr_hvs.c`](third_party/xiph/psnr_hvs.c)
  is bit-exact reference; don't touch its butterfly block
  without matching changes in AVX2 TU. See
  [ADR-0159](../../../docs/adr/0159-psnr-hvs-avx2-bitexact.md)
  and [rebase-notes 0052](../../../docs/rebase-notes.md).

- **SSIMULACRA 2 end-to-end regression gate** (fork-local, ADR-0164):
  [`python/test/ssimulacra2_test.py`](../../../python/test/ssimulacra2_test.py)
  pins pooled + per-frame `--feature ssimulacra2` output on two
  checked-in YUV fixtures. **On rebase**: if scalar or any SIMD
  path changes semantically (should never happen per ADR-0161's
  bit-exact contract), test will fail with values that differ
  by more than 1e-4. Don't update pinned floats unilaterally —
  figure out which kernel drifted and fix it. Netflix golden
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
  `est_params`, map changes into respective decomposed helpers
  (`c_values_*`, `validate_and_setup_dimensions`, `alloc_cambi_buffers`,
  `solve_covariance_system`) rather than re-inlining. Shared prototypes in
  `cambi_internal.h` and `speed_internal.h` must not change without mirroring
  to all GPU/CPU twins (verified by `scripts/ci/twin-drift-check.sh`). Numerical
  bit-exactness is governed by ADR-1146.

- **SSIMULACRA 2 `picture_to_linear_rgb` SIMD** (fork-local, ADR-0163):
  `ssimulacra2_picture_to_linear_rgb_{avx2,avx512,neon}` vectorises
  last scalar hot path (2×/frame). Strategy: per-lane scalar
  reads (all chroma ratios + 8/16-bit), SIMD matmul + normalise +
  clamp, per-lane scalar `powf` for sRGB EOTF. New decoupling
  header `ssimulacra2_simd_common.h` defines `simd_plane_t`;
  dispatch wrapper in `ssimulacra2.c` unpacks `VmafPicture` into it.
  **On rebase**: (1) keep scalar-order matmul chain
  `G = Yn + cb_g*Un; G += cr_g*Vn;` — regrouping drifts ~1 ulp;
  (2) per-lane scalar `powf` is load-bearing — no vector
  polynomial; (3) `simd_plane_t` layout `{data, stride, w, h}`
  is assumed by all three SIMD TUs; (4) arbitrary chroma ratios
  (non-420/422/444) must still work — don't delete `int64_t`
  fallback branch. SSIMULACRA 2 now has **zero scalar hot paths**.
  See
  [ADR-0163](../../../docs/adr/0163-ssimulacra2-ptlr-simd.md) and
  [rebase-notes 0055](../../../docs/rebase-notes.md).

- **SSIMULACRA 2 FastGaussian IIR blur SIMD** (fork-local, ADR-0162):
  `ssimulacra2_blur_plane_{avx2,avx512,neon}` vectorises 30×/frame
  2-pass separable IIR blur. Horizontal pass batches rows (AVX2: 8,
  AVX-512: 16, NEON: 4) and uses gather/lane-set loads to pull
  column-n values from N rows into SIMD vector; vertical pass
  SIMD-iterates columns over per-column `prev1_*`/`prev2_*`
  state arrays. **On rebase**: (1) preserve left-to-right summation
  `(o0 + o1) + o2` and `n2*sum - d1*prev1 - prev2` chaining — any
  re-grouping drifts by ~1 ulp; (2) `col_state` layout is
  `[prev1_0|prev1_1|prev1_2|prev2_0|prev2_1|prev2_2]` in 6×w
  contiguous floats; SIMD loads assume this; (3) NEON lane-set
  pattern (4 `vsetq_lane_f32` per input) replaces
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
  ADR-0213) all produce byte-identical output to scalar on 5
  vectorised kernels (`multiply_3plane`, `linear_rgb_to_xyb`,
  `downsample_2x2`, `ssim_map`, `edge_diff_map`) under
  `FLT_EVAL_METHOD == 0`, plus IIR blur and PTLR ports
  (ADR-0162 / ADR-0163). **On rebase**: (1) preserve left-to-right
  scalar summation order in every matmul + downsample chain —
  `(a+b)+(c+d)` pairing drifts by 1 ULP and regression test
  `test_ssimulacra2_simd` catches it; (2) `cbrtf` stays per-lane
  scalar libm — no vector polynomial; (3) reductions in
  `ssim_map`/`edge_diff_map` use ADR-0139 per-lane `double`
  scalar tail; (4) SVE2 sister TU is locked to fixed 4-lane
  predicate (`svwhilelt_b32(0, 4)`) so its arithmetic order
  matches NEON sibling regardless of runtime vector length. Never
  widen to `svptrue_b32()` without separate ADR plus snapshot
  regen, even if it looks like free perf win. See
  [ADR-0161](../../../docs/adr/0161-ssimulacra2-simd-bitexact.md),
  [ADR-0213](../../../docs/adr/0213-ssimulacra2-sve2.md), and
  [rebase-notes 0053](../../../docs/rebase-notes.md) /
  [rebase-notes 0074](../../../docs/rebase-notes.md).

- **SSIMULACRA 2 Vulkan host-path SIMD** (fork-local, ADR-0252):
  [`x86/ssimulacra2_host_avx2.c`](x86/ssimulacra2_host_avx2.c) and
  [`arm64/ssimulacra2_host_neon.c`](arm64/ssimulacra2_host_neon.c)
  are `plane_stride`-parameterised variants of `linear_rgb_to_xyb`
  and `downsample_2x2` for Vulkan pyramid layout (channel slot
  size = full-resolution frame, fixed across downsampled scales).
  These two TUs carry **same ADR-0161 bit-exactness contract**
  as their CPU-extractor siblings: per-lane scalar `vmaf_ss2_cbrtf`,
  `#pragma STDC FP_CONTRACT OFF`, `-ffp-contract=off`, left-to-right
  addition order. **On rebase**: if upstream or follow-up PR
  changes scalar `ss2v_host_linear_rgb_to_xyb` or
  `ss2v_downsample_2x2` arithmetic order in `ssimulacra2_vulkan.c`:
  update SIMD TUs and their `test_host_xyb` / `test_host_downsample`
  scalar references in lockstep. Byte-exact contract breaks
  silently if scalar changes without SIMD.
  See [ADR-0252](../../../docs/adr/0252-ssimulacra2-host-xyb-simd.md)
  and [rebase-notes 0106](../../../docs/rebase-notes.md).

- **`psnr_hvs` NEON DCT bit-exactness** (fork-local, ADR-0160):
  [`arm64/psnr_hvs_neon.c`](arm64/psnr_hvs_neon.c) is aarch64
  sister port to AVX2 TU. NEON's 4-wide `int32x4_t` splits
  each 8-column row into `r_k_lo` (cols 0-3) + `r_k_hi` (cols
  4-7); 30-butterfly runs twice per DCT pass, and 8×8
  transpose = four `transpose4x4_s32` (via `vtrn1q_s32` /
  `vtrn2q_s32` / `vtrn1q_s64` / `vtrn2q_s64`) + top-right
  ↔ bottom-left block swap. **On rebase**: two SIMD TUs
  (AVX2 + NEON) must move in lockstep with scalar Xiph
  reference — any change to butterfly in `psnr_hvs.c`
  requires matched edits to both SIMD TUs and re-run of
  `test_psnr_hvs_{avx2,neon}`. `accumulate_error()` must keep
  threading outer `ret` by pointer (ADR-0159 summation-order
  lesson; local float accumulator would drift Netflix
  golden by ~5.5e-5). `#pragma STDC FP_CONTRACT OFF` is ignored
  by aarch64 GCC (non-fatal `-Wunknown-pragmas`) but kept for
  portability; aarch64 GCC does not contract `a + b * c` across
  statements at default optimization anyway.
  **IMPORTANT — Intel icx (`intel-llvm`)**: `#pragma STDC FP_CONTRACT
  OFF` is also silently ignored by icx unless `-fp-model=precise` is
  also on command line. `vmaf_fp_model_args` and `vmaf_strict_fp_args` in
  `core/src/meson.build` are the shared compiler-ID policy for x86 and AArch64
  carve-outs, scalar references, and `core/test/meson.build`'s
  `_simd_strict_fp_args`. Unix icx uses `-fp-model=precise` followed by
  `-ffp-contract=off`; `icx-cl` uses `/fp:precise /Qfma-`; MSVC uses
  `/fp:precise`; clang-cl forwards `/clang:-ffp-contract=off`. Do not copy raw
  strict-FP literals back into individual targets or duplicate the mapping in
  the test build. `vmaf_cuda_host_strict_fp_args` separately forwards the
  native host spelling through nvcc (`/fp:precise` on Windows). Do not remove
  these flags without re-running `--suite=fast --suite=simd` under icx. Traced
  via 2026-05-30
  all-backends CI failure and closed by
  `T-MSVC-FFP-CONTRACT-D9002-2026-09-19`. See
  [ADR-0160](../../../docs/adr/0160-psnr-hvs-neon-bitexact.md)
  and [rebase-notes 0052](../../../docs/rebase-notes.md).
  **two flags are order-sensitive and must not be re-sorted.**
  `-fp-model=precise` implies `-ffp-contract=on`, so it goes FIRST and
  `-ffp-contract=off` LAST; the other order re-enables the contraction the
  pair exists to disable. Measured on
  `speed_matmul_avx2` scalar tail with icx 2026.0: `-mfma
  -ffp-contract=off` emits zero `vfmadd`, adding `-fp-model=precise`
  after it emits nine, and putting `-fp-model=precise` before it emits
  zero again. `core/test/meson.build` must continue to alias the shared
  `vmaf_strict_fp_args` variable — SIMD tests compile their own copies of
  scalar references, so replacing the alias with a divergent list puts two
  sides of every bit-exactness comparison on different contraction settings.
  That is what broke `test_ssimulacra2_simd` first time reorder
  was tried; see `T-ICX-FP-CONTRACT-FLAG-ORDER-2026-09-07` in
  [state.md](../../../docs/state.md).
- **Scalar references never call libm `fmaf()`** (ADR-1253). Where scalar
  reference must be bit-exact with SIMD kernel that fuses — `_mm256_fmadd_ps`,
  `vfmla` — it calls `vmaf_fmaf_exact()` from
  [`common/fmaf_exact.h`](common/fmaf_exact.h), which evaluates product and
  sum in `double` and rounds once. `fmaf()` is genuine fused multiply-add on
  glibc, musl and UCRT but **not** on legacy `msvcrt.dll` that MSYS2's
  `MINGW64` links, which is environment required `Windows MinGW64` lane
  builds in; there scalar path rounds twice and SIMD path once.
  ADR-1207's gate measured `ssimulacra2` 0.37 points apart on 48-frame clip
  before fix. two call sites today are `picture_to_linear_rgb` in
  `ssimulacra2.c` and both passes of `ms_ssim_decimate.c`. `grep -rn
  '\bfmaf\?('` over scalar feature sources must stay empty for any
  reference with FMA-using twin.
- **`fastdvdnet_pre.c` 5-frame-window contract** (fork-local,
  ADR-0215): FastDVDnet temporal pre-filter extractor is wired
  to I/O contract `frames: float32 NCHW [1, 5, H, W]` (channel
  axis stacks `[t-2, t-1, t, t+1, t+2]`) → `denoised: float32 NCHW
  [1, 1, H, W]`. Three pieces are load-bearing on rebase: (1)
  centre index is 2 (`FASTDVDNET_PRE_CENTRE`) — `gather_window`
  computes channel-k offsets relative to it; (2) ring buffer
  holds 5 slots and replicates closest available end frame for
  channel positions outside available window (clip start +
  end); (3) registered feature name is
  `fastdvdnet_pre_l1_residual` — downstream consumers (future
  FFmpeg `vmaf_pre_temporal` filter, training harnesses) bind to
  that exact string. **T6-7b update (ADR-0255)**: registry now
  ships real upstream FastDVDnet weights (`smoke: false`) wrapped by
  luma adapter in `ai/scripts/export_fastdvdnet_pre.py`;
  previous smoke-only placeholder is history. C-side contract is
  unchanged; wrapper keeps I/O names (`frames` / `denoised`)
  byte-identical, handles `Y → [Y, Y, Y]` tiling, supplies
  constant `sigma = 25/255` noise map, and performs BT.601 RGB→Y
  collapse internally. Two rebase-sensitive invariants flow from
  wrapper: (4) upstream's `nn.PixelShuffle` is swapped for
  allowlist-safe `Reshape`/`Transpose`/`Reshape` decomposition at
  export time (`DepthToSpace` is not on ONNX op allowlist —
  ADR-0255 §Decision); (5) upstream commit is pinned at
  `c8fdf6182a0340e89dd18f5df25b47337cbede6f` and exporter
  enforces upstream weights sha256
  `9d9d8413c33e3d9d961d07c530237befa1197610b9d60602ff42fd77975d2a17`
  to keep weights drop reproducible. See
  [ADR-0215](../../../docs/adr/0215-fastdvdnet-pre-filter.md) and
  [ADR-0255](../../../docs/adr/0255-fastdvdnet-pre-real-weights.md).
- **`transnet_v2.c` 100-frame-window contract** (fork-local,
  ADR-0223 + ADR-0257) — TransNet V2 shot-boundary detector is
  wired to I/O contract `frames: float32 [1, 100, 3, 27, 48]`
  (100-frame window of 27x48 RGB thumbnails) → `boundary_logits:
  float32 [1, 100]`. Four pieces are load-bearing on rebase:
  (1) ring buffer holds 100 slots and replicates *oldest*
  available frame across pre-clip slots (head-clamp at clip
  start) — corresponding output logit is read from
  `output_logits[WINDOW-1]` because `gather_window` lays
  most-recent push at LAST channel; (2) dual feature-name
  surface — extractor emits both
  `shot_boundary_probability` (sigmoid of centre-slot
  logit) **and** `shot_boundary` (binary 0/1 thresholded at 0.5);
  downstream consumers (per-shot CRF predictor T6-3b,
  FFmpeg shot-cut filter shipping with T6-3b) bind to *both*
  exact strings; (3) shipped ONNX under
  `model/tiny/transnet_v2.onnx` is real upstream weights as of
  ADR-0257 (`smoke: false`, MIT, upstream commit pin
  `77498b8e`); wrapper layer that adapts NTCHW→NTHWC and
  selects only `output_1` lives in
  `ai/scripts/export_transnet_v2.py` and must be re-run if
  upstream commit pin moves; (4) export pipeline replaces
  rank-2 `UnsortedSegmentSum` in upstream's `ColorHistograms`
  branch with equivalent `ScatterND` reduction='add'
  subgraph — semantics-preserving but load-bearing rewrite that
  any future upstream-graph re-conversion has to repeat. See
  [ADR-0223](../../../docs/adr/0223-transnet-v2-shot-detector.md)
  [ADR-0257](../../../docs/adr/0257-transnet-v2-real-weights.md).

- **`cambi.c` GPU port is hybrid host/GPU per
  [ADR-0205](../../../docs/adr/0205-cambi-gpu-feasibility.md) +
  [ADR-0210](../../../docs/adr/0210-cambi-vulkan-integration.md)
  (T7-36 integration).** Vulkan kernel offloads only
  embarrassingly-parallel phases (preprocessing scaffold +
  derivative + 7×7 SAT spatial mask + 2× decimate + 3-tap mode
  filter) to GPU; precision-sensitive
  `calculate_c_values` sliding-histogram pass + top-K spatial
  pooling stay on host. Any CPU-side change to c-value
  formula or histogram update protocol must keep host
  residual call site
  (`cambi_vulkan.c::cambi_vk_extract` → `vmaf_cambi_calculate_c_values`)
  lock-step with CPU `calculate_c_values` — they are
  intentionally same code, called against GPU-produced
  image + mask buffers.
  - **`cambi_internal.h` invariant**: this internal-only header
    exposes cambi.c's file-static helpers (`get_spatial_mask`,
    `decimate`, `filter_mode`, `calculate_c_values`,
    `spatial_pooling`, `weight_scores_per_scale`,
    `get_pixels_in_window`, `cambi_preprocessing`,
    `increment_range` / `decrement_range` /
    `get_derivative_data_for_row` callbacks) to GPU twin via
    thin trampoline block at bottom of `cambi.c`. **Do not
    rename or change signatures of those helpers without
    updating trampoline block + header in same PR
    or GPU build breaks.** trampoline body is *only*
    fork-added code inside `cambi.c`; upstream-mirror body
    above stays byte-identical to keep Netflix sync clean.
  - Strategy III (fully-on-GPU c-values via direct per-pixel
    histogram) is documented in
    [research digest 0020](../../../docs/research/0020-cambi-gpu-strategies.md)
    but deferred to future batch — *do not* attempt to
    optimise it inside v1 hybrid integration.

- **VIF kernelscale stays on precomputed
  `vif_filter1d_table_s` flow — Strategy E in Research-0024.**
  fork carries 11-entry `enum vif_kernelscale_enum`
  plus `vif_filter1d_table_s[11][4][65]` of frozen `const float`
  Gaussian taps in [`vif_tools.h`](vif_tools.h). Netflix
  upstream chain (`4ad6e0ea` runtime helpers, `8c645ce3`
  prescale options, `41d42c9e` edge-mirror bugfix) computes
  Gaussians at runtime — that loses SIMD bit-exact
  contract that ADR-0138 / 0139 / 0142 / 0143 froze. **Do not
  port `4ad6e0ea` / `8c645ce3` verbatim.** future port that
  adds runtime helpers as *opt-in second path* (Strategy C)
  is allowed; it must not touch default
  `vif_kernelscale=1.0` + `vif_prescale=1.0` code path.
  Mirror bugfix `41d42c9e` is separate decision — must come
  with paired `places=4 → places=3` golden loosening per
  ADR-0142 Netflix-authority precedent. See
  [Research-0024](../../../docs/research/0024-vif-upstream-divergence.md)
  for full divergence analysis + decision matrix.

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
  [Research-0024 §"Same divergence test for motion + float_adm"](../../../docs/research/0024-vif-upstream-divergence.md).

### `picture_copy()` carries a `channel` parameter

Upstream commit `d3647c73` (T-NEW-1, ported via this fork's
`upstream/port-d3647c73-feature-speed`) widened
`picture_copy()` / `picture_copy_hbd()` signatures with new
`int channel` argument so new `speed_chroma` and
`speed_temporal` extractors can lift U / V planes from
`VmafPicture`. Every fork-local extractor that calls
`picture_copy()` (`cuda/integer_ms_ssim_cuda.c`,
`vulkan/ssim_vulkan.c`, `vulkan/ms_ssim_vulkan.c`) passes
`channel=0`; upstream-mirror `float_*` callers already do.
**If future upstream commit evolves signature further
(extra parameter, type change): update those four fork-local
call sites in lockstep with upstream-mirror ones.** Silently
trailing upstream signature change fails compilation on any
GPU backend. See
[`docs/rebase-notes.md` §0075](../../../docs/rebase-notes.md).

### `vmaf_fex_ssim` is registered fork-side, not upstream

Upstream Netflix's `feature_extractor.c` does **not** list
`&vmaf_fex_ssim` in its `feature_extractor_list[]`, and upstream
does not compile `integer_ssim.c` either — both are dormant on
upstream master branch. fork wires both paths up so that
`vmaf --feature ssim` resolves at CLI;
fix-up touches three upstream-mirror surfaces (registry-array
row in `feature_extractor.c`, matching `extern` declaration,
and `#include "config.h"` in `integer_ssim.c`) plus one
fork-local meson-build line. **On every upstream sync, re-check
that fork's three additions remain in place.** If upstream
ever lands its own integer-SSIM registration, drop fork's row
in favour of upstream's; file structure is identical so
diff should resolve cleanly in `git rebase`. `config.h` include
in `integer_ssim.c` is load-bearing on Vulkan-enabled LTO builds —
without it `VmafFeatureExtractor` struct layout disagrees
between TUs (different `HAVE_CUDA` / `HAVE_SYCL` / `HAVE_VULKAN`
visibility) and GCC fires `-Wlto-type-mismatch` at link time.
generated `config.h` include in `feature_extractor.h` is
project-wide guard for this layout; keep it there so every extractor
definition and every registry consumer sees same backend fields.

### SSIM SIMD dispatch globals are pthread_once-installed (ADR-0871)

`float_ssim.c` and `float_ms_ssim.c` install SSIM/iqa_convolve
SIMD dispatch tables (`g_ssim_precompute`, `g_ssim_variance`,
`g_ssim_accumulate`, `g_iqa_convolve` in `iqa/ssim_tools.c`) by
calling `iqa_ssim_install_dispatch_once(&s_dispatch_guard,
installer_cb)` from their per-extractor `init()`. Guard
parameter is retained for API symmetry only. Actual mutual
exclusion is provided by file-static `pthread_once_t` inside
`iqa/ssim_tools.c`, shared across both TUs. Without that sharing,
two per-TU guards would each be allowed to fire once, racing
on same dispatch globals (TSan-confirmed 2026-05-30).

**Invariants** when modifying SSIM-related dispatch code:

- Never call `iqa_ssim_set_dispatch` / `iqa_convolve_set_dispatch`
  directly from per-extractor `init()` body. setters
  themselves are unsynchronised by design; only correct
  serialisation point is `iqa_ssim_install_dispatch_once`.
- installer callbacks across TUs must remain idempotent (install
  same ISA-best function pointers). If future SIMD path
  (e.g. SVE, AVX10) is added, extend existing installer rather
  than creating parallel one — once-guard fires exactly once
  process-wide.
- If future upstream commit refactors `float_ssim::init` or
  `float_ms_ssim::init`, preserve
  `iqa_ssim_install_dispatch_once(...)` call. Replacing it with
  bare dispatch install resurrects data race.

See [ADR-0871](../../../docs/adr/0871-ssim-dispatch-pthread-once.md)
and underlying research digest at
[`docs/research/tsan-race-audit-2026-05-30.md`](../../../docs/research/tsan-race-audit-2026-05-30.md).

### `speed_chroma` / `speed_temporal` are float-build-only

two upstream Speed extractors register inside
`#if VMAF_FLOAT_FEATURES` block in `feature_extractor.c`. They
are absent from default `meson setup` build; users who want
them must pass `-Denable_float=true`. Do **not** lift them out
of `#if` block — they call into Speed-specific helpers
in `vif_tools.c` that are themselves only compiled in
float-features path.

[ADR-0253](../../../docs/adr/0253-speed-qa-extractor.md)
(Proposed) records deferral on extending this surface with
SpEED-QA full-frame reduction or SpEED-driven model. Status quo
is binding contract until one of three named triggers in
that ADR fires.

### `speed_internal.c` is the shared CPU helper TU for the SpEED GPU twins (ADR-0964)

`core/src/feature/speed_internal.{h,c}` is contract between
CPU SpEED extractor (`speed.c`) and GPU twins
(`feature/{cuda,hip,sycl}/speed_{chroma,temporal}_*.{c,cpp}`).
header declares 9 functions (dimensions, float-stride,
filter+downscale, covariance, eigendecomp, QR factorise, Q^T
multiply, backward-substitution, regularity check); GPU TUs
`#include` it and call 7 of them.

**Wiring invariant — any new feature extractor that ships GPU
twins must have FIVE companion changes in same PR**:

1. **CPU implementation TU** — scalar reference under
   `core/src/feature/<name>.c` (or, for shared math,
   `<name>_internal.c` under same directory).
2. **Header declaration** in `core/src/feature/<name>.h` or
   `<name>_internal.h` — GPU-callable surface.
3. **Meson source list** —
   - CPU TU added to appropriate block in `core/src/meson.build`
     (`libvmaf_feature_sources` or under `if float_enabled`).
   - HIP TU added to `core/src/hip/meson.build` `hip_sources +=
     files(...)`.
   - SYCL TU added to `core/src/meson.build` `sycl_feature_sources`.
   - CUDA TU added to `core/src/meson.build`
     `libvmaf_feature_sources` under `if is_cuda_enabled`.
4. **Registry entry** in
   `core/src/feature/feature_extractor.c` — `extern` declaration
   under right `#if HAVE_<BACKEND>` block plus row in
   `feature_extractor_list[]`.
5. **CPU-vs-GPU parity test** under `core/test/`, mirroring
   `test_sycl_motion3_parity.c` (or
   `test_sycl_speed_{chroma,temporal}_parity.c` for SpEED
   example). Must skip cleanly when relevant GPU device is
   not visible.

If any of five is missing, symptom is silent:
extractor name does not resolve in `vmaf_get_feature_extractor_by_name()`
and GPU pipeline never runs, while CI stays green because no
parity gate fires.  `feature_extractor_list_audit()` function
in `feature_extractor.c` catches duplicate registrations, not
*missing* ones.

### `matrix_mul` dispatches; `si_mat_mul` deliberately does not (ADR-1196)

`speed.c`'s `matrix_mul()` no longer contains multiply loop. It
takes `speed_matmul_fn` (declared in
[`speed_matmul.h`](speed_matmul.h)) and forwards to
`speed_matmul_scalar` / `speed_matmul_avx2` / `speed_matmul_avx512`,
chosen in `speed_dispatch_cpu_kernel()` from `vmaf_get_cpu_flags()`.
pointer is threaded explicitly through
`matrix_qr_decomposition()` and `solve_linear_system()`, which upstream
Netflix does not do — expect signature conflict there on next
`/sync-upstream`, and re-thread rather than dropping parameter.

**Two invariants hold that pointer's value:**

1. **kernels must stay bit-identical to scalar reference.**
   argument is that `j` in `dst[i][j] += x[i][k] * y[k][j]` is output
   index, not reduction axis, so vector width cannot reorder any single
   element's accumulation over `k`. only way to break that is FMA
   contraction, which is why
   `x86/speed_matmul_avx2.c` and `x86/speed_matmul_avx512.c` each compile
   in their own `-ffp-contract=off` static library in
   `core/src/meson.build`. **Do not move either file into
   `x86_avx2_sources` / `x86_avx512_sources`** — those libraries are built
   with `-mfma` and contraction on, and `memcmp` cases in
   `core/test/test_speed_simd.c` will start failing.
2. **`speed_matmul_scalar` is intentionally non-`static`.** parity
   test compares twins against production reference itself, not
   against copy. Re-`static`-ing it breaks test link.

`si_mat_mul()` in `speed_internal.c` — ADR-0964 duplicate of same
i-k-j loop, used by host side of GPU SpEED twins — is
dispatched through `speed_matmul_avx512` / `speed_matmul_avx2` / `speed_matmul_scalar`
per ADR-1237 (gated behind same bit-exact contract as `speed.c`'s `matrix_mul()`).

**Source-of-truth note**: `speed_internal.c` duplicates ~600 LOC
of pure math (eigendecomp, QR, matrix helpers) from `speed.c`.
This is deliberate (see ADR-0964 Alternatives) — keeping
`speed.c` clean of `extern` exposures preserves its
Netflix-mirrored status for `/sync-upstream` cadence. If
either copy gets bug-fix, mirror it to other;
CPU-vs-SYCL and CPU-vs-CUDA parity tests will surface drift at CI time.

**CUDA TU dependency on `CudaFunctions` schema (ADR-0965)**: two
CUDA SpEED TUs (`cuda/speed_chroma_cuda.c` and
`cuda/speed_temporal_cuda.c`) call into `CudaFunctions` table
using **two specific members**:

- `cuMemHostAlloc((void **)&ptr, size, flags)` — pinned host
  allocation (NOT `cuMemAllocHost`; that variant is not in table).
- `cuMemFreeHost(ptr)` — pinned host free.

two CUDA TUs also use `CHECK_CUDA_GOTO(cu_f, CALL, label)` for all
fallible CUDA calls (NOT legacy `CHECK_CUDA` macro which was removed).
If `CudaFunctions` table ever gains or renames these members, update
all four `ALLOC_HOST` / `FREE_HOST` macro call sites in both TUs in
same PR. See `core/src/cuda/cuda_helper.cuh` for macro contract and
`core/src/cuda/picture_cuda.c` / `core/src/cuda/common.c` for
canonical usage of these members across codebase.

### CodeQL `cpp/declaration-hides-variable` rename invariants (2026-05-09)

64-alert sweep of 2026-05-09 (see
[`docs/rebase-notes.md`](../../../docs/rebase-notes.md) entry of
same date) renamed inner-scope shadows in
`x86/adm_avx2.c`, `x86/adm_avx512.c`, `x86/vif_avx2.c`,
`x86/vif_avx512.c`, plus `cambi.c`. **Do not let upstream port
re-introduce unprefixed names** — CodeQL re-flags them and
strict `cpp/declaration-hides-variable` gate trips. rebase-safe
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

Bit-exactness: sweep is provably no-op (renames + scope-tighten

- identical-typed deletes only). Netflix CPU golden 3 must remain
green across rebases — re-run
`PYTHONPATH=$PWD/python python3 -m pytest python/test/quality_runner_test.py
-k test_run_vmaf python/test/vmafexec_test.py
python/test/vmafexec_feature_extractor_test.py -m "not slow"` block
after port-upstream of any of these files.
- **NIQE fork-pkl parity invariants** (`niqe.c` / `niqe_math.h` /
  `niqe_model.h`, fork-local, ADR-1112): NIQE has **no upstream twin** — it
  replicates fork Python harness
  (`compat/python-vmaf/core/noref_feature_extractor.py::NiqeNorefFeatureExtractor`),
  not LIVE MATLAB or scikit-video. Two divergences from upstream NIQE are
  **load-bearing** and must survive any refactor:
  1. AGGD mean parameter `N` carries **trailing `*aggdratio`** factor
     (`niqe_math.h::niqe_extract_aggd`). Upstream omits it; pkl was
     trained with it. Dropping it shifts `N` ~0.428 → ~0.245.
  2. MSCN maps (`niqe_compute_mscn` casts to `float`) **and** PIL
     bicubic half-resolution output (`niqe_bicubic_resize` final
     `(double)(float)acc`) are **rounded through float32**. PIL returns
     float32 ('F'-mode) image and harness quantizes MSCN maps; without
     these rounds scale-2 features drift ~4e-7 and, through
     ill-conditioned averaged covariance, score by ~1e-4.
  pristine model `niqe_model.h` is generated from
  `model/other_models/niqe_v0.1.pkl` in per-block **interleaved** feature
  order (permutation in header comment); build-time checksums
  (`mu.sum`, `trace(cov)`, sha256 prefixes) pin it. end-to-end gate is
  `core/test/test_niqe.c` against `testdata/scores_cpu_niqe.json` at places=4.
  niqe is registered in **C++23 `feature_extractor.cpp`** registry, not
  dead `feature_extractor.c` twin.
- **BRISQUE MATLAB-pipeline parity invariants** (`brisque.c` /
  `brisque_math.h` / `brisque_model.h`, fork-local, ADR-1115): BRISQUE has **no
  upstream twin** — it replicates **gregfreeman MATLAB pipeline that trained
  bundled model** (`model/other_models/brisque_live.model`), NOT
  widely-copied krshrimali C++ port. Four choices are **load-bearing** and must
  survive any refactor (porting C++ behaviour instead would mis-predict
  against trained model):
  1. MSCN field (features f1/f2) is fit with **GGD**, not AGGD
     (`brisque_fit_ggd`). krshrimali uses AGGD — bug vs paper Table I and
     trained model.
  2. Gaussian window uses **sigma = 7/6** (`brisque_build_window`), not
     truncated `1.166` from C++ port.
  3. half-resolution downscale is **MATLAB antialiased bicubic**
     (`brisque_resize_coeffs`, scale 0.5, kernel width 8 → fixed 10 taps,
     symmetric-reflect index map), not OpenCV INTER_CUBIC.
  4. Range-scaling uses **inline `computescore.cpp` `min_[36]`/`max_[36]`
     arrays** baked into `brisque.c` — NOT conflicting `allrange` file in
     same upstream repo (which reference code never reads; substituting it
     corrupts every score). No output clamp. Prediction is plain `svm_predict`
     (== `svm_predict_probability` for EPSILON_SVR).
  AGGD fit excludes exact zeros from both sign buckets (strict `x<0` /
  `x>0`, MATLAB semantics — unlike NIQE, which buckets zeros right). model
  is **embedded at build time** by `xxd -i` Meson `custom_target` over
  `model/other_models/brisque_live.model` (same path as libvmaf's JSON models;
  symbols `src_brisque_live_model[]` / `_len`, declared in `brisque_model.h`) —
  big C array is NOT committed (it exceeds 1 MB large-file gate; only
  binary model + tiny declaration header live in-tree). `init()` parses
  that buffer via `svm_parse_model_from_buffer`, or, if `model_path` option
  is set, loads on-disk model via `svm_load_model`. end-to-end gate is
  `core/test/test_brisque.c` against
  `testdata/scores_cpu_brisque.json` (snapshotted because AGGD strict-sign
  fit is FP-summation-order-sensitive on near-flat content). First feature
  extractor to consume vendored libsvm. Registered in **C++23
  `feature_extractor.cpp`** registry.

- **Perceptual side-data weighting golden-isolation invariant**
  (`perceptual_weight.{c,h}` + weighting branch in
  `core/src/libvmaf.c::vmaf_feature_score_pooled`, fork-local, ADR-1118):
  Pelorus-driven pooling weights MUST be **inert** unless BOTH () weighting is
  enabled (`vmaf_set_perceptual_weight_enabled`, default OFF) AND (b) valid
  Pelorus blob was registered for frame (`vmaf_set_perceptual_sidedata`).
  This is **load-bearing for Netflix golden gate** — golden pairs carry
  no side-data, so they must score **bit-exact**. Two rules survive any refactor:
  1. `perceptual_weight.c::vmaf_perceptual_weight_at_index` returns **exactly
     `1.0`** for any disabled / empty / absent / non-finite case. Never let it
     return "close to 1.0" value on no-side-data path.
  2. `vmaf_feature_score_pooled` must branch on
     `vmaf_perceptual_weight_active()`: when inactive it runs **literal
     upstream** MEAN / HARMONIC_MEAN expressions (`sum/pic_cnt`,
     `pic_cnt/i_sum − 1`) in original order — NOT weighted formula that
     merely evaluates to same number. Byte-identical, not numerically close.
     weighted accumulators (`w_sum` / `w_score_sum` / `w_i_sum`) are only
     summed when active. MIN/MAX are intentionally never weighted.
  end-to-end guard is `core/test/test_perceptual_weight.c` (bit-exact
  pooling without side-data, with enabled-but-absent, and with present-but-
  disabled). reader is CPU-only and consumes vendored Pelorus parser
  (ADR-1113) — do not edit those vendored files. R1–R6 graceful-degrade
  (`grid==0` → frame-level scalar; bad ABI → unweighted + log) must also hold.
  3. **Complexity modulation (ADR-1120, Pelorus ABI ≥ 1.3):**
     `derive_salience` scales salience by
     `complexity_modulation(blob)` = `(1 − 0.5·complexity)` floored at `0.25`
     when `PEL_SEC_COMPLEXITY` is present. This MUST collapse to factor `1.0`
     (no modulation) when section is **absent** or `complexity` is
     non-finite — that is what keeps no-side-data golden path bit-exact.
     Never change `complexity_modulation` to return anything but `1.0` on
     absent/NaN path. load-bearing guard is `test_complexity_modulates_weight`
     / `test_complexity_modulates_grid_zero` (toggle-proven: stubbing helper
     to no-op fails test). section is grid-independent, so it also
     modulates `grid==0` scalar path.

### Scalar VIF statistic and float-motion plane helpers (2026-09-02, c-rework-vif-motion)

- **`integer_vif.c` = single scalar reference SIMD tails run against.**
  `vif_statistic_8`, `vif_statistic_16` and `vif_compute_line_residuals`
  (called by `x86/vif_avx2.c`, `x86/vif_avx512.c` and `arm64/vif_neon.c` for
  columns their 16-wide blocks do not cover) all go through
  `vif_horizontal_pixel` → `vif_accumulate_pixel` → `vif_store_residuals`.
  Those helpers hold upstream arithmetic verbatim (operand types and
  evaluation order included) and are `FORCE_INLINE`. Change statistic
  there once and mirror it in three kernels. Never re-inline private
  copy into one entry point, or SIMD block path and scalar tail
  diverge for widths that are not multiples of 16. Bit-exactness relies on
  fork's `-std=c23` (contraction off) and no `-march` (no FMA) flags; if
  either changes, re-run 31-case `--precision max` matrix in
  [`docs/research/2026-09-02-c-rework-vif-motion-bit-exact.md`](../../../docs/research/2026-09-02-c-rework-vif-motion-bit-exact.md).
- **`log_generate` uses `roundf`**, proven bit-identical to upstream's
  `round` over all `VIF_LOG2_TABLE_SIZE` entries. same LUT feeds
  AVX-512 gather path (ADR-0500); do not switch rounding modes.
- **`write_scores` append order is output contract**: four scale scores,
  then `integer_vif` / `_num` / `_den`, then num / den per scale 0..3.
  `double` totals are explicit left-to-right sums — keep them out of loops.
- **`vif_tools.c` float filters**: `vif_use_avx2_convolution` is only
  place ADR-0504 AVX2-only decision lives (AVX-512 float convolution was
  removed for golden parity — do not re-add it here); `vif_mirror_index` is
  reflect-101 (`-idx` / `2n - idx - 2`) and is shared by all three vertical
  passes and horizontal pass. `vif_pixel_statistic_s` keeps `vif_sigma_nsq`
  as `double` in `log2f` arguments — narrowing it changes promotion.
- **`float_motion.c` planes**: `MotionState.plane[0..2]` are Y, U, V; U and V
  exist only with `motion_add_uv`, so `motion_free_planes` (only teardown)
  must keep `motion_add_uv` guard. `motion_chroma_heights` rejects
  chroma-less formats **before** any allocation. `motion_score_pair` adds
  Y, then U, then V — `double` add order is load-bearing for
  `motion_add_uv` parity with CUDA / SYCL twins. `motion_clip` /
  `motion_blend_clip` are only places `motion_fps_weight` /
  `motion_max_val` clip is applied.
- **C translation units keep `NULL`** (ADR-1138): `integer_vif.c` and
  `float_motion.c` carry file-scoped
  `NOLINTBEGIN/END(modernize-use-nullptr)` bracket; keep `NOLINTEND` at
  end of file when appending. `vif_tools.c` has no null-pointer constants
  (`grep -c NULL core/src/feature/vif_tools.c` and `grep -c NOLINT` on
  same file both print `0`) and therefore carries no bracket — do not add
  one unless upstream hunk brings `NULL` into that file. `flush()` in
  `float_motion.c` carries cited `cppcheck-suppress constParameterCallback`
  because `VmafFeatureExtractor.flush` callback type fixes its prototype.

## Governing ADRs

- [ADR-0024](../../../docs/adr/0024-netflix-golden-preserved.md) —
  three CPU golden pairs never change.
- [ADR-0041](../../../docs/adr/0041-lpips-sq-extractor.md) — LPIPS
  extractor registration pattern.
- [ADR-0042](../../../docs/adr/0042-tinyai-docs-required-per-pr.md) —
  DNN-backed extractors ship docs under `docs/ai/`.
- [ADR-0236](../../../docs/adr/0236-dists-extractor.md) — `dists_sq`
  mirrors LPIPS' two-input tiny-AI extractor shape. Keep
  `VMAF_DISTS_SQ_MODEL_PATH`, `model_path`, registry id
  `dists_sq_placeholder_v0`, and `score` scalar output aligned until
  real DISTS weights replace smoke checkpoint.
- **LPIPS / DISTS high-bit-depth input invariant** — both extractors
  accept planar 8/10/12/16-bit YUV but keep ONNX tensor ABI as
  ImageNet-normalised RGB8. High-bit-depth samples are little-endian
  16-bit containers rounded into 8-bit domain before shared
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
  and CUDA / SYCL / Vulkan twins must keep that literal aligned
  with CPU reference.
- [ADR-0205](../../../docs/adr/0205-cambi-gpu-feasibility.md) +
  [ADR-0210](../../../docs/adr/0210-cambi-vulkan-integration.md) —
  cambi Vulkan integration (Strategy II, hybrid host/GPU).
  Precision-sensitive `calculate_c_values` + top-K stay on host;
  GPU phases are integer + bit-exact.
- [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  GPU-parity CI gate: per-feature `FEATURE_TOLERANCE` map in
  `scripts/ci/cross_backend_parity_gate.py` is single source of
  truth. Every new GPU twin needs entry.

## Newly-arrived shipped surfaces (rebase awareness)

- **MS-SSIM `enable_lcs` GPU implementation (T7-35, PR #207 MERGED)**
  — wires existing CPU `enable_lcs` 15-extra-metrics through
  CUDA + Vulkan + SYCL MS-SSIM kernels. On rebase: ensure
  option metadata stays declared on GPU paths even if
  body is still TODO.
- **`psnr` cross-backend `enable_chroma` option parity (ADR-0453)** —
  `psnr_cuda`, `psnr_sycl`, and `psnr_vulkan` now honour
  `enable_chroma` (default `true`) consistently with CPU reference.
  Passing `enable_chroma=false` produces luma-only output on all three
  GPU backends. option default must remain `true`; any change to
  default or `n_planes` clamp logic requires coordinated update
  across all three GPU twins. See CUDA AGENTS.md / Vulkan AGENTS.md
  invariant notes and [ADR-0453](../../../docs/adr/0453-psnr-enable-chroma-gpu-parity.md).
- **`psnr` / `float_psnr` cross-backend `uncapped` option parity
  (ADR-1193)** — integer `psnr` and `float_psnr` extractors on CPU and
  all eight GPU twins (CUDA / SYCL / HIP / Metal x integer / float) carry
  opt-in `uncapped` boolean, default `false`. `psnr_max` has two roles:
  `mse == 0` infinity sentinel (unconditional, and what Netflix golden
  60 / 84 / 108 dB assertions pin) and truncation of genuinely computed
  values above it (dropped when `uncapped` is true). Two invariants:
  `!uncapped` arm must stay pre-ADR-1193 expression **verbatim** rather
  than re-derivation, because with `min_sse` below ~1.9e-11 ceiling
  rises past ~208 dB that zero MSE floored to 1e-16 produces.
  Re-derived `mse == 0 -> psnr_max` arm would move default score.
  Second invariant: option must **not** be
  `VMAF_OPT_FLAG_FEATURE_PARAM`, since CPU
  extractor appends without name dict while GPU twins append with
  one. Flagging it would make two backends emit different feature
  keys for same request. Adding it to one backend only is silent
  cross-backend divergence no CPU test catches. See
  [ADR-1193](../../../docs/adr/1193-psnr-uncapped-option.md) and
  `core/test/test_psnr_uncapped.c`.
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
  recommended U-2-Net `u2netp` replacement is also
  Google-Drive-walled and uses ONNX `Resize` which is not on
  fork's `op_allowlist.c` (ADR-0265). C-side `input` →
  `saliency_map` tensor-name contract is invariant across both
  blockers; any future drop-in replaces `.onnx` and bumps
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
  as existing NEON ports per
  [ADR-0161](../../../docs/adr/0161-ssimulacra2-simd-bitexact.md)
  / [ADR-0162](../../../docs/adr/0162-ssimulacra2-iir-blur-simd.md)
  / [ADR-0163](../../../docs/adr/0163-ssimulacra2-ptlr-simd.md).
- **`float_ms_ssim` `enable_chroma` (ADR-0583, PR opened 2026-05-16)**:
  `float_ms_ssim.c` has `bool enable_chroma` field in `MsSsimState`
  and per-plane loop in `extract()` emitting `float_ms_ssim_cb` /
  `float_ms_ssim_cr`. default is `false` (luma-only, backward-
  compatible). GPU twins (`_cuda`, `_sycl`, `_vulkan`) do not yet carry
  this option — they are planned follow-up. If upstream Netflix adds
  any option to `float_ms_ssim.c`, mirror it to all GPU twins in
  same PR per twin-parity invariant.
- **Upstream ports**: `feature/motion` options from `b949cebf`
  (T-NEW-1) MERGED via PR #197 (2026-04-29). `feature/speed`
  port from `d3647c73` (`speed_chroma` + `speed_temporal`) is
  PR #213 (open). 32-bit ADM/cpu fallbacks (`8a289703` +
  `1b6c3886`) are PR #212 (open).

- **Per-frame `malloc`/`aligned_malloc` for geometry-sized buffers is forbidden
  on hot paths** (ADR-0452): any buffer whose size is determined by input
  geometry (`w`, `h`, `stride`) MUST be hoisted to `init_fex` and freed in
  `close_fex`. geometry is known at init time. Per-frame heap traffic for
  geometry-sized scratch eliminates up to ~79 MB/frame of allocator pressure at
  1080p and causes arena lock contention in threaded mode. Examples: `float_vif`
  hoists `10 × plane_sz` to `VifState::vif_buf` per ADR-0452; `ssimulacra2`
  hoists its workspace similarly. If upstream port re-introduces per-frame
  allocation for geometry-sized buffer, move it to init/close in same PR.
  Small constant-size (geometry-independent) allocations inside hot paths
  are acceptable but must be justified in PR description.

- **VIF log2 LUT invariant** (ADR-0500): `VifPublicState.log2_table` is
  32768-entry table (64 KB, not 65537 entries / 128 KB). normalisation in
  `log2_32` / `log2_64` always produces indices in `[32768..65535]`; mask
  `& (VIF_LOG2_TABLE_SIZE - 1u)` strips bit 15 to get `[0..32767]` index.
  If upstream Netflix changes LUT size or normalisation logic, audit
  mask in `integer_vif.h` and three gather sites in `vif_avx512.c`
  before merging. new `log_generate` fills `log2_table[i] = log2f(32768+i)*2048`;
  original filled `log2_table[i] = log2f(i)*2048` for `i` in `[32767..65535]`.
- **compute_vif filter-cache parameter** (ADR-0500): `compute_vif` in `vif.c`
  accepts two nullable trailing parameters `precomputed_filters` /
  `precomputed_filter_widths`. `float_vif.c` caller passes pre-computed
  Gaussian coefficients from `VifState.filter_cache` / `filter_width_cache`
  (populated once in `init()`). internal `vifdiff` path passes NULL to
  retain original per-call `vif_get_filter()` path. If upstream changes
  `compute_vif`'s signature, both declaration in `vif.h` and internal
  call in `vif.c` need updating.

- **`feature_extractor_list[]` is exactly-once** (ADR-0544):
  static `feature_extractor_list[]` in `feature_extractor.c` must
  list every `&vmaf_fex_*` symbol **at most once** under its correct
  `#ifdef HAVE_*` guard. duplicate is silently masked by
  first-match `vmaf_get_feature_extractor_by_name()` but breaks
  ctx-pool's iterator dispatch: pool's `get_fex_list_entry()` keys
  on `fex->name` so same name registered twice still collapses to
  one pool entry there, but any caller that walks registry
  directly (e.g. iterator dispatch path that fans out
  `vmaf_use_features_from_model`) allocates one entry per registered
  pointer and runs `init`/`extract`/`flush` once per copy per picture.
  audit helper `vmaf_feature_extractor_list_audit()` runs from
  `vmaf_init()` and returns `-EINVAL` on duplicates;
  `test_feature_extractor_list_no_duplicates` C unit test exercises
  it on live registry. When adding new backend or extractor:
  add **one** `extern VmafFeatureExtractor vmaf_fex_*_<bk>` decl and
  **one** `&vmaf_fex_*_<bk>` entry inside matching `#if HAVE_<BK>`
  block — do not paste whole `&vmaf_fex_*` cluster.

- **Register extractors in `feature_extractor.cpp`, NOT
  `feature_extractor.c`** (ADR-0846 / ADR-1110): build compiles
  C++23 `feature_extractor.cpp` (see `core/src/meson.build`);
  old `feature_extractor.c` is **dead twin** left over from
  ADR-0846 conversion and is *not* in any build target. Editing only
  `.c` makes `vmaf_get_feature_extractor_by_name()` return NULL
  (symptom: `problem loading feature extractor: <name>` from
  CLI). When adding new extractor, put `extern` decl +
  `feature_extractor_list[]` entry in `.cpp`. (stale `.c`
  should be removed in separate cleanup.)

- **`delta_e_itp` PQ-only invariant** (ADR-1110): ΔE-ITP
  extractor (`delta_e_itp.c`) ships **PQ (ST-2084) transfer
  only**; `init()` rejects any `transfer` other than `pq` with
  `-EINVAL`. PQ matrices/constants are triple-sourced against
  ITU-R BT.2124-0; HLG (Annex 3) and BT.1886/SDR (Conversion 5)
  paths are single-sourced and intentionally deferred. Do **not**
  loosen `transfer` guard to accept `hlg`/`bt1886` without first
  cross-validating those constants against independent source.
  PQ EOTF/EOTF⁻¹ live in `delta_e_itp_math.h`; out-of-gamut LMS/ICtCp
  values are deliberately **not clamped** (BT.2124 Annex 4) — unit
  test's places=4 ITP-triple oracle depends on this.
  `scale_chroma_planes` / `scale_chroma_planes_hbd` helpers are
  copied verbatim from `ciede.c` but are independent copies.
- **`y_funque_plus` atoms-only invariants** (ADR-1114): Y-FUNQUE+
  extractor (`y_funque_plus.c`) has **no upstream twin** and is
  clean-room reimplementation from papers (MIT `funque_plus` used
  only as cross-check). It ships **three atoms only**
  (`y_funque_plus_ms_ssim` / `_dlm` / `_mad`); fused ScaledSVR MOS
  score is deliberately **not** shipped (upstream commits no frozen
  regressor — see Deferred row in `docs/state.md`). Load-bearing
  details rebase or "cleanup" must not silently break:
  1. **Haar butterfly** uses pywt `'haar'` convention
     `cH=(a+b-c-d)/2`, `cV=(a-b+c-d)/2` (verified directly against
     `pywt.dwt2`). design dossier prose listed H/V **swapped** — do
     not "fix" code to match stale prose; code is correct
     and DLM's psi-angle mask depends on it.
  2. **DLM num/den abs-asymmetry** (`yf_dlm_pool`): numerator pools
     `rest^3` **without** abs while denominator pools ref detail
     **with** abs (mirrors upstream `pyr_features.py:54/61`). Symmetrising
     them is real behaviour change, not cleanup.
  3. **OpenCV `INTER_CUBIC`** (Keys cubic `a=-0.75`, src coord `2i+0.5`,
     `BORDER_REPLICATE`) is dominant cross-host parity component;
     TU compiles in its own static lib with `-ffp-contract=off` (
     same rationale as `ssimulacra2`). Keep both.
  4. Nadenau Y-channel CSF weights **only** detail subbands;
     approx subbands are never weighted. analytic constants
     (`a=1/256`, `b_Y=-5.4715e-3`, `c_Y=1.91`) regenerate official
     lookup table to 8 dp. Oracle values in `core/test/test_y_funque_plus.c`
     were re-derived against `pywt` + OpenCV reference at places=4.

## Reflect-101 mirror padding — invariants (ADR-1166)

separable float convolution in `common/convolution_internal.h` uses
**reflect-101** mirror padding, and fold is deliberately **iterative**:

```c
FORCE_INLINE int convolution_reflect101(int idx, int size)
{
    if (size <= 1) return 0;
    while (idx < 0 || idx >= size)
        idx = (idx < 0) ? -idx : (2 * size - idx - 2);
    return idx;
}
```

Load-bearing details rebase or "simplification" must not break:

1. **loop is not decoration.** Upstream (and this fork, before ADR-1166)
   bounced once. One bounce only lands in range when `size >= radius + 1`;
   at `size == 2` tap of `-2` folds to `+2` and tap of `+3` folds to `-1`,
   and caller dereferences out of bounds. Two live CPU paths reached those
   sizes — `float_vif` on 9..15 px frames and `float_motion` with
   `motion_add_uv` on 4x4 4:2:0 chroma. Do not collapse it back to
   `if/else if`.
2. **`size <= 1` short circuit is required for termination**, not for
   correctness: at `size == 1` fold alternates between `-2` and `+2`
   forever.
3. **fold is bit-identical to single bounce for every in-contract
   size** (loop exits on first iteration), which is what lets this be
   pure safety fix with no score movement.
   `core/test/test_convolution_edge_small.c::test_large_plane_bit_identical`
   pins that against explicit single-bounce reference; if you change
   fold, that test must still pass unmodified.
4. **`convolution.c`'s `convolution_clamp_borders()` is load-bearing too.**
   `borders_right` / `borders_bottom` are derived as
   `dim - (filter_width - radius)` and go **negative** for plane narrower
   than filter, which makes trailing border loop start at negative
   index and write before destination. clamp is no-op for every
   `dim >= filter_width`.

motion extractors' own `mirror()` bodies (`integer_motion.c`,
`integer_motion_v2.c`, `x86/motion_avx2.c`, `x86/motion_avx512.c`,
`arm64/motion_v2_neon.c`, and CUDA / HIP / Metal twins) are **still
single-bounce on purpose**: they sit behind `init()` guard that rejects
`w < 3 || h < 3`, so defective sizes are unreachable. That is deliberate
divergence from Netflix/vmaf#1581, which instead fixes `mirror()` so tiny
frames can be scored. Changing it is behaviour decision, not cleanup —
see `docs/rebase-notes.md`.

## Minimum-dimension guards cover every plane, not just luma (ADR-1166)

`float_motion.c::motion_check_min_dim_all_planes` validates **chroma**
dimensions too when `motion_add_uv` is set, because `motion_blur_plane` is
called per plane with `ref_pic->w[c]` / `ref_pic->h[c]`. chroma geometry
must stay in step with `core/src/picture.c` (`(dim + ss) >> ss`); luma-only
guard is exactly bug Netflix/vmaf#1582 describes.

`float_vif.c`'s guard is derived from `vif_get_min_dim(kernelscale)` —
largest `((filter_width_s / 2) + 1) << s` over four-scale ladder, 16 at
default kernelscale — not from scale-0 filter alone. Do not replace it with
constant.

## `compat_builtin.h`: never `__lzcnt` (ADR-1166)

MSVC `__builtin_clz` / `__builtin_clzll` shim must use `_BitScanReverse` /
`_BitScanReverse64`. `__lzcnt` emits LZCNT instruction unconditionally with
no runtime feature gate. On x86-64 without ABM/LZCNT `F3` prefix is
ignored and it retires as BSR, returning MSB index instead of
leading-zero count. Silently wrong VIF and ADM shifts result, with no
fault and no CI signal (every hosted Windows runner has LZCNT).
Netflix/vmaf#1422 proposes
`__lzcnt` form; Netflix/vmaf#1551 is upstream's own retraction of it.
`scripts/ci/check-msvc-clz-shim.sh` fails `fast` suite if it comes back.

## `convolution_f32_c_s` dispatches to SIMD — fix the twins, not just the scalar

`core/src/feature/common/convolution.c::convolution_f32_c_s` returns straight
into `convolution_f32_avx_s` whenever `VMAF_X86_CPU_FLAG_AVX2` is set. That is
every CI runner and dev workstation. **fix applied only to scalar
body in `convolution.c` is dead code on x86.**

AVX2 (`convolution_avx.c`) and AVX-512 (`convolution_avx512.c`) twins each
derive same vertical border split — `radius` and `height - radius` — at
three sites apiece, once per kernel variant (`_s`, `_sq_s`, `_xy_s`). Six sites
total. All of them must stay clamped via `convolution_clamp_borders` in
`convolution_internal.h`: for plane shorter than radius, `height - radius`
is negative, so trailing border loop starts at negative row and
leading one runs past end. Both are heap **writes**, not reads.

**Testing scalar kernel does not test this.**
`core/test/test_convolution_edge_small.c` calls `convolution_y_c_s` /
`convolution_x_c_s` directly and so never reaches dispatch;
`test_motion_min_dim.c` only calls `init()`. Anything asserting convolution
is safe at small sizes must go through public API — see
`core/test/test_motion_convolution_oob.c`.

**guard must mirror kernel it protects, not option that named it.**
`motion_blur_plane` keeps `filter_size = 5` for `motion_filter_size == 1` and
merely swaps in `FILTER_5_NO_OP_s`, so radius is 2 regardless. guard that
reads option value instead of filter width kernel uses
will let defective sizes through.

**Chroma plane geometry is ceiling, `(dim + ss) >> ss`, matching
`picture.c`.** Using `h / 2` under-allocates by one row for every odd luma
height, and even-height fixtures — including both Netflix golden resolutions —
never catch it.

## GPU-twin `VmafOption` tables mirror the CPU table (2026-09-05)

Option iteration terminates on table entry's null `name`, not address
of entry. Preserve that sentinel, aliases and default-value omission when
rebasing `feature_extractor.cpp` or `feature_name.cpp`; existing
`test_feature` and `test_feature_extractor` cases pin those behaviors.

two shared pool structs in `feature_extractor.h` keep C-compatible layouts
under ADR-0772. Their narrowly scoped `uninitMemberVarNoCtor` markers depend
on `vmaf_fex_ctx_pool_create()` zero-initializing outer pool and all three
slot construction paths value-initializing entries before activation. New
construction sites must preserve and re-verify that contract; do not broaden
those markers to other types or uninitialized-use checks.

`vmaf_feature_name_from_options()` (`feature_name.cpp`) builds emitted
feature key from extractor's **own** `options[]` table: every entry that
carries `VMAF_OPT_FLAG_FEATURE_PARAM` and holds non-default value appends
`_<alias>_<value>`. GPU twin whose table is missing one FEATURE_PARAM entry
therefore emits *different key* than its CPU twin for same opts dict, and
model lookup misses. default model
(`model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json`) is live example: it requests
`VMAF_integer_feature_adm3_score` with `adm_csf_mode=2`, `adm_dlm_weight=0.7`,
`adm_enhn_gain_limit=1.0`, `adm_min_val=0.5`, `adm_noise_weight=0.02`, and
key it looks up is
`integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`.

Two rules follow, and they are not same rule:

1. **table mirrors CPU table entry-for-entry** — name, alias,
   `type`, `default_val`, `min`, `max`, and `VMAF_OPT_FLAG_FEATURE_PARAM`
   bit. Anything that diverges (different alias, different default,
   missing flag) silently changes key. `core/src/feature/integer_adm.c` is
   reference for `adm` family.
2. **Never declared-and-ignored.** Option that changes value twin
   emits must change it. FEATURE_PARAM option whose arithmetic
   only feeds feature twin does **not** emit is one legitimate
   exception. It stays in table for key parity, exactly as
   `adm_dlm_weight` and `adm_min_val` have no arithmetic effect on `adm2` in
   CPU reference either. Entry must carry comment saying so.
   non-FEATURE_PARAM option (`adm_skip_aim`, `debug`) never affects key,
   so it has no key-parity excuse: implement it or leave it out.

**Never fabricate feature to make name resolve.** If twin cannot compute
feature, leave that feature out of its `provided_features[]`.
`vmaf_get_feature_extractor_by_feature_name()` then routes request to
CPU twin through ADR-0530 fallback, which produces correct value under
correct key. Emitting feature from hard-coded stand-in (SYCL and
HIP `integer_adm` twins briefly emitted `VMAF_integer_feature_aim_score` from
literal `aim_num = 0.0`) is strictly worse than not providing it: fallback
stops firing and model silently consumes fabricated score.

**`adm_min_val` clamps `adm3` only.** `integer_adm.c::extract()` applies
`MAX(..., s->adm_min_val)` to adm3 expression alone; `adm2` is emitted
unclamped. Netflix golden `adm_min_val=0.98` case pins
`VMAF_integer_feature_adm2_min_0.98_score` at `0.9345148541666667` —
*below* floor. twin that clamps `adm2` diverges.

**`numden_limit` scales with full-frame area.** `1e-10 * (w * h) /
(1920.0 * 1080.0)` uses picture dimensions, not scale-3 dimensions
per-scale loop variables hold once loop has run. All three GPU twins had
inherited post-loop values (256× too-small floor).

## ADM contrast-masking edge policy is asymmetric (ADR-1204)

`adm_cm_thresh3x3_s` in `adm_tools.c` is CPU reference for 3x3
contrast-masking neighbourhood, and its edge handling is deliberately **not**
symmetric:

```c
i_m1 = (i == 0)     ? 1     : i - 1;   /* near edge MIRRORS to index 1   */
i_p1 = (i == h - 1) ? h - 1 : i + 1;   /* far  edge CLAMPS to last index */
```

Every GPU twin (`cuda/float_adm/`, `hip/float_adm/`, `metal/float_adm.metal`,
`sycl/float_adm_sycl.cpp`) must reproduce **both** halves. symmetric mirror
(`2 * half_w - x - 2`) reads index `w - 2` where CPU reads `w - 1`. It is
tempting because it looks uniform. It survives casual testing because
two only differ when ADM border crop `(int)(dim * ADM_BORDER_FACTOR - 0.5)`
collapses to 0 — band dimensions `<= 14` — since only zero crop pulls
first and last row and column into summation region.

If upstream rewrites `ADM_CM_THRESH_S_*` macro family, re-derive twins
from closed form above, not from macros.

## ssimulacra2's YCbCr -> linear-RGB conversion is FMA everywhere (ADR-0891, ADR-1205)

There are six copies of these three lines — scalar fallback in
`ssimulacra2.c`, CUDA / HIP / Metal / SYCL host conversions, and
AVX2 / AVX-512 / NEON / SVE2 kernels with their scalar tails. All of them must
use single-rounded fused multiply-add, in this order:

```c
R = fmaf(cr_r, Vn, Yn);
G = fmaf(cb_g, Un, Yn);
G = fmaf(cr_g, Vn, G);
B = fmaf(cb_b, Un, Yn);
```

Do not assume 1 ULP deviation here is harmless. pipeline is
ill-conditioned downstream: edge-diff term computes `|img - blur(img)|`,
catastrophic cancellation, and pooling takes 4-norm dominated by largest
survivors. Measured, one ULP in linear RGB became **2.62e-03** score delta.

`core/test/test_ssimulacra2_simd.c` compares SIMD kernels against
**private** scalar reference, not against shipped function, so it will not
catch shipped copy that drifts. Change shipped copies and that reference
together.

## `angle_flag` has exactly one definition (ADR-1194)

integer-ADM 1-degree angle test lives in
[`adm_angle_flag.h`](adm_angle_flag.h) and nowhere else. Do not re-inline it
into backend, and do not "improve" it.

- `adm_angle_flag_fp64()` is **golden-frozen** upstream expression: narrow
  each int64 operand to `float`, then compare in `double`. narrowing is
  lossy past 24-bit significand — that is not bug to fix, it is
  value Netflix golden assertions encode (CLAUDE.md rule 1). scalar
  CPU path, CUDA and HIP call it, at both scale 0 and scales 1-3.
- `adm_angle_flag_i64()` returns **bit-identical** result using only
  64-bit integers. SYCL calls it because
  [`sycl/integer_adm_sycl.cpp`](sycl/integer_adm_sycl.cpp) must contain no
  binary64 instruction at all (one fp64 op anywhere in that translation unit
  makes runtime reject whole SPIR-V module on Arc A-series and
  iGPUs). [`metal/integer_adm.metal`](metal/integer_adm.metal) mirrors it
  by hand because MSL has no `double` type.

Two consequences for anyone editing this area:

1. **MSL copy is manual mirror.** `iadm_angle_flag()` in
   `metal/integer_adm.metal` is line-for-line translation of
   `adm_angle_flag_i64()`. They must be edited together, in same commit.
   Nothing in build catches drift between them — Metal is not built on
   Linux.
2. **`ADM_ANGLE_FLAG_MC` / `ADM_ANGLE_FLAG_D` encode constant.**
   integer form hard-codes significand of `(float)cos(1deg)^2`
   (`0x3F7FEC0A`, `MC = 16772106`, `D = 2^24 - MC = 5110`), and MSL mirror
   repeats `D`. If constant ever changes, all three move together.
   `core/test/test_adm_angle_flag.c` asserts relationship, so partial
   edit fails `fast` suite rather than silently shifting scores.

flipped `angle_flag` selects other branch of `decouple()`'s enhancement
gain limit, so it moves `adm` scores directly. four historical spellings
of this predicate disagreed on about 4e-5 of near-parallel scale-0 band
quadruples; see
[`docs/research/2030-adm-angle-flag-fp64-free.md`](../../../docs/research/2030-adm-angle-flag-fp64-free.md).

## Feature-context pool entry lifetime

`VmafFeatureExtractorContextPool::fex_list` is growable pointer table;
`get_fex_list_entry()` separately allocates each `fex_list_entry` and publishes
it only after `init_fex_list_slot()` succeeds. Do not move initialized entry:
`vmaf_fex_ctx_pool_aquire()` retains it while `pthread_cond_wait()` releases
pool mutex, and release must signal same condition-variable address.
Preserve pointer-table and context-array size checks, and free each options copy
even if its first context allocation failed. Linux
`test_fex_pool_growth` regression forces table relocation while another
acquisition waits. See
[pool-growth digest](../../../docs/research/fex-pool-growth-2026-09-08.md).

## High-bit-depth samples are normalised before accumulation (ADR-1212)

`picture_copy()` divides every 10/12/16-bit sample by 4 / 16 / 256 before
float extractors see it, so CPU "sum of samples" is sum of *normalised*
samples. GPU twin that reads raw plane and accumulates codewords must
apply that scaler itself — on host, to exact integer sums, which is
bit-identical to CPU at 10 and 12 bpc. `float_moment` on CUDA, SYCL and HIP
shipped without it and was 4x–256x off above 8 bpc; nothing caught it because
every parity fixture was 8-bit. Register `-DFIXTURE_BPC=10u` variant of any
new parity test whose extractor consumes samples.

## A score must not depend on the host ISA (ADR-1207, ADR-1208)

Every SIMD kernel in this tree is required to be bit-exact with its scalar
reference. Same input must produce same bits on AVX-512 host,
AVX2 host and host with no SIMD. Two things follow for anyone touching
kernel here:

- per-feature `test_<feature>_simd.c` files compare SIMD kernel against
  scalar reference **defined inside test TU**, because shipped scalar
  functions are `static`. That reference can drift away from shipped one —
  it did, twice (ADR-1205, ADR-1208). Passing `test_<feature>_simd` is
  therefore necessary but not sufficient.
- `core/test/test_feature_isa_invariance.c` is gate that compares
  shipped SIMD path against shipped scalar path, end to end, via
  `VmafConfiguration.cpumask`. Run it after any kernel change.

Concretely, when kernel promotes `float` inputs to `double`, do promotion
**before** arithmetic, not after. `(double)a - (double)b` is exact for two
floats; `(double)(a - b)` is not, and mixing two between vector body and
its scalar tail makes result depend on vector width.

## Twin option tables mirror the CPU's aliases and semantics (ADR-1214)

When a GPU twin copies an option from the CPU extractor, copy the `alias` and
range too: ADR-1183 builds the emitted feature name from the alias and value of
every non-default option, so `cs` on the twin and `scf` on the CPU means two
different keys for one feature. And copy the *semantics* from the branch the
twin actually implements — `adm_csf_scale` is a Barten-mode argument, so in the
Watson-only twins it must be a no-op exactly as it is on the CPU.

## Error exits use unwind helpers, not label ladders (HISS-01, 2026-09-21)

Cleanup `goto` is gone from `ciede`, `feature_collector`, `feature_dists`,
`feature_lpips`, `float_moment`, `float_ms_ssim`, `float_psnr`, `float_ssim`,
`motion` and `pu21`. Pattern that replaced it: one `static` unwind helper per
constructor, in same translation unit, releasing resources in same order old
`free_*:` chain used. Shallow exits reach same helper; members not yet acquired
are still NULL, and `free(NULL)` is no-op. Where release set cannot be inferred
from NULL — feature-collector mutex and aggregate vector, pu21 buffer pair —
helper takes explicit stage enum and releases every stage at or below it,
highest first.

Two rules for anyone adding error path here:

- Do not reintroduce `goto`. Add case to unwind helper instead.
- Free order is behaviour. Enumerate exit paths before and after any edit
  and check them against each other. `pthread_mutex_destroy` before
  `free(fc)`, `aligned_free` in acquisition-reverse order, `vmaf_picture_unref`
  before scratch release.

Two upstream-parity quirks are preserved on purpose: `float_psnr` leaves its
buffers to extractor teardown when bit depth is unsupported, and `ciede`
reports `-EINVAL` rather than `-ENOMEM` for same case. Both matched old label
ladders. Fixing either is behavioural change and needs own commit plus test.

## Split helpers must not split an expression (HISS-04, ADR-1253)

When function here is split to fit 60-LOC bound, helper is `static` in same
TU, never reached through function pointer, never in another TU. Arithmetic
moves whole: each statement lives on one side of helper boundary. Reason is
FMA contraction — `a * b + c` inside helper contracts same way it did inline,
but `t = a * b;` in caller plus `t + c` in helper does not, and that is 1 ULP
that ssimulacra2 pooling amplifies into visible score delta (ADR-1205).
Reductions keep their order: move whole accumulation loop, never partial sums.

Functions carrying ADR-0141 §2 bit-exactness carve-outs stay unsplit —
`compute_adm`, `adm_dwt2_s`, `calc_ssim`, `brisque_fit_aggd`,
`niqe_extract_aggd`, `create_recursive_gaussian`, `picture_to_linear_rgb`.
Their paired SIMD ports match them line for line; splitting one forces
matching splits in four SIMD files and breaks scalar-diff audit story.

## Integer ADM's 16-bit vertical DWT sums in int64

- `adm_dwt2_vpass16_tap4()` (`integer_adm.h`) = only 16-bit vertical DWT
  response. Scalar `adm_dwt2_vpass_16()`, `adm_dwt2_16_avx2()`,
  `adm_dwt2_16_avx512()` call it.
- Low-pass taps 1-3 sum 50582 -> int32 partial sum overflows at 16 bpc once
  3 samples >= 42456. Upstream form = int32 = UB.
- Normalised result fits int32 -> int64 form bit-exact with old wrap. Never
  "optimise" back to int32; outputs match, UB returns.
- Guard: `test_integer_adm_dwt16_range` (sanitizer lane halts on the UB).
- 8-bit pass stays int32: 255 * 50582 fits.
- T-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18.
