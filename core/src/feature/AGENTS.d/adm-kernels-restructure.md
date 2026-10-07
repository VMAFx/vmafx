---
paths:
  - core/src/feature/integer_adm.c
  - core/src/feature/adm_tools.c
invariant: Integer ADM kernels header separation, AIM clipping differences, and adm_min_val clamping.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer ADM Restructuring and AIM Clipping

- **`integer_adm_kernels.h` = scalar integer ADM kernels** (fork-local,
  ADR-1402 PR): `integer_adm.c` drivers and x86 twins share them. No
  second copy of scalar kernel in `x86/adm_avx2.c` / `x86/adm_avx512.c`.
  `AdmCmCtx.row_data` = frame-wide vector constants of interior-row
  callback (`CmFrameConsts`); NULL on scalar path. Row fold stays in
  `adm_cm_rows()` / `i4_adm_cm_rows()` (ADR-1167).
- **Integer AIM is unclipped, float AIM is clipped at 1: keep both**
  (ADR-1417). `adm_result_finalise()` -> `vmaf_adm_scale_ratios()` =
  `aim_num / den`, as upstream `integer_adm.c`. `compute_adm()` ->
  `vmaf_adm_finalize_scores()` = `MIN(aim_num / aim_den, 1)`, as upstream
  `adm.c`. Flat reference + additive impairment: integer aim > 1 (3.1756 on
  64x64 patch picture), adm3 down to `adm_min_val`. Shipped
  `vmaf_v1.0.16` models read integer adm3. Do not add clip to integer
  path or drop it from float one unless upstream does. No golden assertion
  has integer aim > 1; guard = `test_integer_adm_aim_unclipped`.
- **`integer_adm.c` / `adm_tools.c` are restructured upstream-mirror
  files** (ADR-1141, 2026-09-02): every kernel expression is verbatim
  but code no longer lines up textually with Netflix/vmaf — re-port
  upstream hunks by hand into owning helper (function map in
  [rebase-notes](../../../../docs/rebase-notes.md)
  "refactor/c-rework-adm"). Invariants rebase or follow-up must
  keep: (1) `adm_cm_thresh()` / `i4_adm_cm_thresh()` /
  `adm_cm_thresh3x3_s()` are closed form of nine upstream
  `ADM_CM_THRESH_S_*` corner / edge / interior macros — mirror-to-1
  before first edge, clamp-to-last past last edge, nine terms in
  macro order (float twin's summation order is golden-gated); scale-0
  integer centre term is int32, no `(int16_t)` cast (ADR-1402).
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

**`adm_min_val` clamps `adm3` only.** `integer_adm.c::extract()` applies
`MAX(..., s->adm_min_val)` to adm3 expression alone; `adm2` is emitted
unclamped. Netflix golden `adm_min_val=0.98` case pins
`VMAF_integer_feature_adm2_min_0.98_score` at `0.9345148541666667` —
*below* floor. twin that clamps `adm2` diverges.
