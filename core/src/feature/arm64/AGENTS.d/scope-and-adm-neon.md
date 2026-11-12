---
paths:
  - core/src/feature/arm64/*adm*neon*
  - core/src/feature/arm64/*.h
invariant: Self-contained SIMD headers, scalar bit-exactness, and integer/float ADM NEON non-contracting kernels.
---
<!-- markdownlint-disable MD013 -->
# ARM64 scope, self-contained headers, and ADM NEON rules

## Scope

Per-feature aarch64 NEON + SVE2 SIMD implementations.
Every TU mirrors scalar reference one level up; runtime dispatch from
feature `*_dispatch.c` via `vmaf_get_cpu_flags_arm()`
(see [`../../arm/cpu.c`](../../../arm/cpu.c)).

```text
feature/arm64/
  <feature>_neon.{c,h}      # NEON path (aarch64 baseline; always available on ARCH_AARCH64)
  ssimulacra2_sve2.{c,h}    # SVE2 path (ADR-0213) — runtime-gated via HWCAP2_SVE2
  moment_sve2.{c,h}         # SVE2 path for float_moment (ADR-0584) — VLA, lanes added in raster order (ADR-1500)
  ms_ssim_decimate_neon.*   # 9-tap LPF SIMD (one of four byte-identical TUs — see parent AGENTS.md)
```

## Ground rules

- **Every SIMD `.h` file MUST be self-contained.** Include every standard
  header for types in own declarations; do not rely on transitive includes from
  consumer `.c` files. Header declaring `ptrdiff_t` parameter MUST include
  `<stddef.h>` directly. Standalone-include failures on Apple Clang and Ubuntu
  ARM Clang = CI regressions (see PR #914 for cambi family; fixed for motion
  family in accompanying PR).
- **Parent rules** apply in full (see [../AGENTS.md](../../AGENTS.md) +
  [../../AGENTS.md](../../../AGENTS.md)).
- **Bit-exactness with scalar reference is non-negotiable.** Same rule as AVX2 /
  AVX-512 sibling: every NEON kernel mirrors scalar TU byte-for-byte under
  `FLT_EVAL_METHOD == 0`. Bit-exact regression tests in
  [`../../../test/`](../../../test/) (`test_*_simd.c`, migrated through
  [`simd_bitexact_test.h`](../../../test/simd_bitexact_test.h) harness per
  ADR-0245) catch ULP drift.
- **Integer-ADM DWT2: one NEON path on every AArch64 platform
  ([ADR-1257](../../../../../docs/adr/1257-retire-darwin-adm-dwt2-legacy-dispatch.md),
  supersedes ADR-1057 Apple wrapper).** `integer_adm.c` dispatches
  `adm_dwt2_8_neon()` on Apple + Linux alike. Never reintroduce
  platform-specific first-column rule or score offset.
- **`adm_dwt2_8_neon()` horizontal 8-wide loop stops at
  `half_w >= 2 ? half_w - 1 - ((half_w - 2) % 8) : 1`** (Netflix/vmaf
  `ea012e387`, adapted). Column 0 + every column from that bound on go through
  `adm_dwt2_8_neon_hpass_column()` and `ind_x`, which applies mirror. Without
  bound: vector store runs past half-resolution row and, on last row, into next
  band of ADM slab. `test_adm_dwt2_neon` checks bit-exactness + guard band at
  production band stride.
- **`adm_decouple_neon()` returns scalar decouple's bits at every gain
  limit** (Netflix/vmaf `9e48141b`). Integral limit: four columns per step,
  int32 `rst * gain`; fractional limit or fewer than four columns:
  `adm_decouple_cols()` (`integer_adm_kernels.h`), ADR-1413 truncated
  double product. angle test is `adm_angle_flag_fp64()`'s double
  expression. Details and guards: [`../AGENTS.d/adm-rounding.md`](../../AGENTS.d/adm-rounding.md).
- **Scale 1-3 DWT, scale 1-3 decouple and contrast masking of every scale
  return scalar's bits** (Netflix/vmaf `8bc5a5c6a`, `b41d2340a`, adapted).
  `adm_cm_neon()` / `i4_adm_cm_neon()` = interior-row callbacks of scalar
  drivers `adm_cm_rows()` / `i4_adm_cm_rows()` (row folded once, ADR-1167;
  scale-0 lanes uint64, scale 1-3 int64). Scale-0 centre tap int32, excess in
  int64 clamped to [0, INT32_MAX] (ADR-1402); upstream narrows tap to int16:
  never import that. Edge rows/cols, rows < 4 cols, leftover cols, i4 factor
  > INT32_MAX: scalar sample. `adm_decouple_s123_neon()`: vector path at gain
  limit 1 only, angle flag via scalar `adm_angle_flag()` for overshooting
  lanes; other limits scalar `adm_decouple_s123_cols()`.
  `adm_dwt2_s123_combined_neon()`: int64 taps + scale rounding + arithmetic
  shift, narrowed by truncation (`i4_dwt2_tap4()`); de-interleaved block only
  where `ind_x[k][j] = 2j - 1 + k` (j >= 1, 2j + 8 < w). Dispatch: cm only
  without `csf_requires_normalization`, as x86. Guards:
  `test_integer_adm_simd` (cm scale 0 + i4, decouple gains),
  `test_adm_dwt2_neon` (s123), GCC + clang under qemu.
- **`#pragma STDC FP_CONTRACT OFF` kept at TU level** even though aarch64 GCC
  ignores it with non-fatal `-Wunknown-pragmas`. Pragma is portable; aarch64 GCC
  does not contract `a + b * c` across statements at default optimisation
  anyway. Removal on rebase loses cross-architecture documentation.
- **Float-ADM DWT2 unconditionally non-contracting (ADR-1057, 2026-08-31).**
  Golden-producing scalar `adm_dwt2_s` carries function-scoped Clang
  `contract(off)` pragma and GCC `optimize("-ffp-contract=off")` attribute.
  Do not widen either guard to all of `adm_tools.c`: earlier file-scope form
  changed unrelated ADM reductions. `float_adm_dwt2_neon.c` starts every
  accumulator at +0, then uses four explicit `vmulq_laneq_f32` + `vaddq_f32`
  steps and matching scalar sequences. Initial +0 load-bearing for
  signed-zero parity with `adm_dwt2_s`. Keep NEON TU `-ffp-contract=off`;
  do not introduce `vfmaq`, `fmaf`, or other fused form. Guarded by bit-exact
  `test_float_adm_dwt2_neon` (including signed zero) under Clang and GCC
  AArch64/QEMU.
- **`float_adm_dwt2_neon()` row helpers (ADR-1142, HISS-04).** Entry point
  keeps name + signature (dispatched from `adm.c`). Per row:
  `dwt2_vertical_row_neon()` (4-wide loop over `dwt2_vertical_4_neon()`,
  called for `flo` then `fhi`; scalar tail) and `dwt2_horizontal_row_neon()`
  (scalar, `ind_x`). Every function there carries GCC
  `optimize("-ffp-contract=off")` attribute: GCC's `vaddq_f32` / `vmulq_*` are
  plain `+` / `*`, so helper without it can fuse when built outside
  meson carve-out. New helper -> attribute, whole statements only, `accum`
  sequence unchanged. Vertical vector sum sits in its own helper because
  NEON intrinsic macros count as statements (`readability-function-size`,
  129 > 120 when inline).
