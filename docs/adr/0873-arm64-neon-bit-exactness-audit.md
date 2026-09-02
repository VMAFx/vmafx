<!-- markdownlint-disable MD038 MD060 -->
# ADR-0873: ARM64 NEON bit-exactness audit — `-ffp-contract=off` carve-out scope

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: `simd`, `arm64`, `neon`, `bit-exactness`, `build`, `ci`

## Context

PR #282 added `-ffp-contract=off` to the x86 AVX2/AVX-512 static libs.
PR #339 (DRAFT) adds `-fp-model=precise` for icx. A parallel audit of the
ARM64 NEON path (`core/src/feature/arm64/*.c`) was required to verify that:

1. Every NEON file containing float arithmetic is protected from
   compiler-driven FMA contraction (`-ffp-contract=fast` is the GCC/Clang
   default on aarch64).
2. The build system's static-lib carve-out structure in `meson.build`
   lines 579–643 actually covers every NEON source file.
3. Test coverage on the `ubuntu-24.04-arm` CI runner exercises the NEON
   paths and compares against the scalar reference.

The audit found three issues:

**Gap 1 — `arm64_static_lib` compiled without `-ffp-contract=off`.**
The main `arm64_v8` static lib (`arm64_static_lib`, line 599–604) is compiled
with `vmaf_cflags_common + ['-DARCH_AARCH64']` only. This lib contains
`float_adm_neon.c`, `float_motion_neon.c`, `float_psnr_neon.c`, `ciede_neon.c`,
`ms_ssim_decimate_neon.c`, and `convolve_neon.c` — all of which contain
floating-point arithmetic. On GCC ≥ 10 and on Clang with aarch64 target the
compiler may auto-fuse `a*b+c` patterns into FMLA instructions in plain C
code. Three of those files are material:

- `/core/src/feature/arm64/float_adm_neon.c` — `vmlaq_laneq_f32` in the
  vertical DWT pass and plain `filter_lo[i] * s_i + ... ` in the scalar
  horizontal pass; and `float32x4_t vaddq_f32(v_inner, val3)` in the
  `sum_cube`/`csf_den_scale` reductions, which accumulate in float32 (the
  same reduction-stability gap ADR-0138 addressed for AVX2).
- `/core/src/feature/arm64/ms_ssim_decimate_neon.c` — `vfmaq_n_f32` is an
  explicit FMA intrinsic in `h_pass_neon_4` and `v_pass_neon_4`. The scalar
  reference functions `h_pass_scalar` / `v_pass_scalar` use `fmaf()`, so
  scalar and NEON both commit to fused-multiply-add. The scalar path already
  uses FMA consistently, so there is no asymmetry for this file.
- `/core/src/feature/arm64/psnr_hvs_neon.c` — contains a `#pragma STDC
  FP_CONTRACT OFF` TU-level guard for its scalar float accumulators.
  This is the correct defensive pattern but the pragma's effectiveness
  depends on compiler support; the compile-flag backup is absent.

The pragma-only approach in `psnr_hvs_neon.c` and the total absence of
protection in `float_adm_neon.c` and other float-arithmetic files are the
primary gap.

**Gap 2 — `float_adm_neon.c` has no dispatch caller.**
`float_adm_sum_cube_neon`, `float_adm_csf_den_scale_neon`, and
`float_adm_dwt2_neon` are compiled into the library but are never called
from `adm.c` / `float_adm.c`. There is no `#if ARCH_AARCH64` dispatch block
in the float_adm extractor equivalent to the x86 `integer_adm.c` lines 3353/3367.
These functions are dead code today.

**Gap 3 — `test_motion_v2_simd.c` skips NEON entirely.**
`core/test/test_motion_v2_simd.c` tests `motion_score_pipeline_16_avx2`
against scalar. The NEON sibling `motion_score_pipeline_16_neon` has no
corresponding test. The `ubuntu-24.04-arm` CI matrix job runs `meson test`
which includes `test_motion_v2_simd`, but the test body early-exits via
`#if ARCH_X86 ... #else (void)fprintf(stderr, "skipping: non-x86 arch\n") #endif`.

## Decision

1. **Split `arm64_static_lib` into two libs** at the meson.build level:
   - `arm64_v8` (existing): integer-only TUs (`vif_neon.c`, `adm_neon.c`,
     `psnr_neon.c`, `ssim_neon.c`, `motion_neon.c`, `cambi_neon.c`).
     No `-ffp-contract=off` needed (all integer arithmetic).
   - `arm64_v8_fp` (new): float TUs (`float_adm_neon.c`, `float_psnr_neon.c`,
     `float_motion_neon.c`, `ciede_neon.c`, `ms_ssim_decimate_neon.c`,
     `convolve_neon.c`, `psnr_hvs_neon.c`, `moment_neon.c`, `motion_v2_neon.c`).
     Compiled with `vmaf_cflags_common + ['-DARCH_AARCH64', '-ffp-contract=off']`.

2. **Add dispatch wiring** in `adm.c` / `float_adm.c` for the three
   `float_adm_*_neon` functions under `#if ARCH_AARCH64`.

3. **Add NEON arm of `test_motion_v2_simd.c`** testing
   `motion_score_pipeline_16_neon` against the inline scalar reference on
   the adversarial negative-diff and mixed-diff fixtures already used for AVX2.

4. The `ms_ssim_decimate_neon.c` FMA situation is **not a gap**: the scalar
   reference consistently uses `fmaf()` and the NEON path uses
   `vfmaq_n_f32`; both paths are FMA, and the existing `test_ms_ssim_decimate`
   verifies byte-level parity on ARM64 (NEON is baseline, no runtime probe
   needed). The existing test constitutes the bit-exactness gate.

5. `float_adm_neon.c`'s `sum_cube` and `csf_den_scale` float32 reduction
   trees should follow the ADR-0138 pattern: accumulate into `float64x2_t`
   via `vcvt_f64_f32` before summing, matching the AVX2 path's
   `_mm256_cvtps_pd` strategy.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `-ffp-contract=off` to the whole `arm64_v8` lib | Simple one-liner | Applies flag to integer TUs where it is a no-op but a lint/clarity noise source | Rejected; the integer TUs need no FP protection |
| Rely solely on `#pragma STDC FP_CONTRACT OFF` inside each TU | No build system change | Compiler-defined; GCC documents that the pragma has no effect when FP_CONTRACT is mandated by `-ffp-contract=fast`; not portable guarantee | Insufficient as the only barrier |
| Leave float_adm NEON as dead code | Zero risk of regression | Wastes build time; the dead symbols will eventually confuse the dispatch maintainer | Not acceptable long term |

## Consequences

- **Positive**: `-ffp-contract=off` now covers all floating-point NEON TUs;
  a matching `arm64_v8_fp` lib mirrors the x86 `x86_avx2_ffp` convention.
  `float_adm_neon.c` enters the dispatch path on ARM64, reducing ADM
  compute time on M-series and AArch64 Linux. NEON motion_v2 gains a
  regression test.
- **Negative**: meson.build gains ~15 lines; dispatching `float_adm` on
  ARM64 requires verifying the Netflix golden gate still passes, which is
  already handled by CI.

## References

- req: PR #282 description — `-ffp-contract=off` for x86 SIMD libs
- req: PR #339 — icx `-fp-model=precise` DRAFT
- Q1.1: user directive — audit ARM64 NEON paths for bit-exactness vs scalar
- ADR-0138 — float-ADM reduction stability (AVX2 precedent)
- ADR-0161 — SSIMULACRA2 NEON `-ffp-contract=off` carve-out
- docs/rebase-notes.md §0052 — psnr_hvs NEON bit-exactness invariant
