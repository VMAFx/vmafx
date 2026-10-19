## Netflix/vmaf 8bc5a5c6a + b41d2340a: integer ADM NEON kernels for every scale (2026-10-08)

- `core/src/feature/arm64/adm_neon.c` / `.h`: `adm_cm_neon()`,
  `i4_adm_cm_neon()`, `adm_dwt2_s123_combined_neon()` and
  `adm_decouple_s123_neon()`, dispatched in `init_dispatch_simd()`
  (`integer_adm.c`; contrast masking only without
  `csf_requires_normalization`, as on x86).
- The fork's form differs from upstream's on purpose:
  - contrast masking: the kernels are interior-row callbacks of the scalar
    drivers `adm_cm_rows()` / `i4_adm_cm_rows()` (one fold per row,
    ADR-1167) and finish through `adm_cm_result()` / `i4_adm_cm_result()`,
    where upstream repeats the factor, shift and pooling code. The scale-0
    centre tap stays int32 and the excess is formed in int64 and clamped
    (ADR-1402); upstream narrows the tap to int16 (`vmovn_s32`) and
    subtracts the threshold in int32. The scale-0 row is summed in uint64
    (`adm_cm_fold_s0()`). Upstream's three-row running sums in `tmp_ref` are
    not taken; each block reads its 3x3 neighbourhood.
  - scale 1-3 decouple: the angle flag of an overshooting lane comes from
    the scalar `adm_angle_flag()`, not from upstream's vector double test.
  - scale 1-3 DWT: the int64 taps get the scale's rounding term from
    `i4_dwt2_round()` and an arithmetic shift, as `i4_dwt2_tap4()`, and both
    pictures share the scalar `tmp_ref` layout.
  **On sync**: do not import upstream's `adm_cm_threshold_neon()` (int16
  tap), `adm_cm_sum_row()` or its result code; port a change of the scalar
  kernels into these callbacks.
- Tests: `test_integer_adm_simd` runs the contrast-masking, centre-tap and
  decouple tests on aarch64 too, and gains `test_i4_adm_cm_matches_scalar_kernels`
  (also against `i4_adm_cm_avx2` / `_avx512`); `test_adm_dwt2_neon` gains
  `test_adm_dwt2_s123_neon_matches_scalar`.
