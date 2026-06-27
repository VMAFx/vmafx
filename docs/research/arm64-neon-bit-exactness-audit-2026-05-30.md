<!-- markdownlint-disable MD060 -->
# ARM64 NEON bit-exactness audit — 2026-05-30

**Purpose**: Verify that the `-ffp-contract=off` carve-out introduced in
PR #282 (AVX2/AVX-512) was correctly applied to all floating-point NEON
TUs, and that the ARM64 test coverage on `ubuntu-24.04-arm` runners is
adequate.

## Methodology

1. Read all 19 files in `core/src/feature/arm64/*.c`.
2. Classified each as integer-only or float-arithmetic.
3. Cross-checked against `core/src/meson.build` lines 579–643.
4. Checked every `#pragma STDC FP_CONTRACT` occurrence in the arm64 directory.
5. Traced call sites for all exported NEON function names using `grep -rn`.
6. Read every `test_*_simd.c` file for NEON coverage.
7. Checked `.github/workflows/libvmaf-build-matrix.yml` for the
   `ubuntu-24.04-arm` runner configuration and `meson test` invocation.

## Findings

### Finding 1 — Critical: float TUs in `arm64_v8` without `-ffp-contract=off`

The `arm64_v8` static lib (meson.build line 592–604) compiled 9 float-arithmetic
TUs with only `vmaf_cflags_common + ['-DARCH_AARCH64']`:

- `float_adm_neon.c`
- `float_psnr_neon.c`
- `float_motion_neon.c`
- `ciede_neon.c`
- `ms_ssim_decimate_neon.c`
- `convolve_neon.c`
- `motion_v2_neon.c`
- `psnr_hvs_neon.c`
- `moment_neon.c`

GCC ≥ 10 and Clang on aarch64 default to `-ffp-contract=fast`, which can
auto-fuse `a*b+c` patterns in plain C into `fmla`/`fmls` instructions.
NEON intrinsics (`vmlaq_laneq_f32`, `vfmaq_n_f32`) are already opaque to
the contraction pass, but the scalar tails and non-intrinsic arithmetic
in these files are not. The mismatch means the scalar tail and the NEON
main loop can round differently from the scalar reference.

`psnr_hvs_neon.c` has a `#pragma STDC FP_CONTRACT OFF` guard, but GCC
documents that this pragma has no effect when `-ffp-contract=fast` is in
force at the command-line level. The pragma alone is insufficient.

**Fix**: split `arm64_v8` into `arm64_v8` (integer TUs only) and
`arm64_v8_fp` (float TUs, compiled with `-ffp-contract=off`).

> **Update (2026-06-27, ADR-1057)**: this finding focused on the NEON TUs, but
> for a dispatched SIMD path the *scalar reference* it is compared against must
> also be contraction-stable on aarch64. `float_adm_dwt2_neon` is dispatched and
> FMA-free, but its scalar reference `adm_dwt2_s` / `adm_dwt2_lo_s` lives in
> `adm_tools.c` (the general feature lib, **not** an `-ffp-contract=off` carve-out),
> so on aarch64 the compiler fused the scalar `accum += a*b` into `fmadd` and the
> two paths diverged by ~1 ULP. Two corrections were needed: `adm_tools.c` must
> `#include "config.h"` (its `#ifdef HAVE_CONFIG_H` guard was dead, so `ARCH_AARCH64`
> was invisible in the TU), and the two scalar DWT2 functions carry an
> aarch64-only `-ffp-contract=off` guard. General lesson for future dispatched
> float SIMD: audit the scalar reference's contraction on aarch64, not only the
> NEON TU's.

### Finding 2 — High: `float_adm_neon.c` is dead code

`float_adm_sum_cube_neon`, `float_adm_csf_den_scale_neon`, and
`float_adm_dwt2_neon` are compiled into `arm64_v8` (now `arm64_v8_fp`)
but are never called from any dispatch path. `adm.c` uses static `#define`
aliases (`adm_sum_cube_s`, `adm_csf_den_scale_s`) with no arch-conditional
dispatch. The same gap exists for the x86 AVX2 variants. Neither the NEON
nor the AVX2 `float_adm_*` functions have ever been called at runtime.

**Fix**: Added a follow-up comment in `adm.c` citing ADR-0873. Full
dispatch wiring is deferred (requires a function-pointer table analogous
to `integer_adm.c` and Netflix-golden-gate verification).

> **Update (2026-06-27, ADR-1057)**: `float_adm_dwt2_neon` is no longer dead.
> It was extracted into its own TU (`float_adm_dwt2_neon.c`, FMA-free, meson
> lib `arm64_adm_dwt2_neon_lib` built `-ffp-contract=off`) and is dispatched
> at runtime via `adm_dwt2_dispatch` in `adm.c` on aarch64 with NEON. It is now
> bit-exact with the scalar reference — see Finding 1 update below and the
> `test_float_adm_simd` row in the test-coverage table. `float_adm_sum_cube_neon`
> / `float_adm_csf_den_scale_neon` (Finding 3) remain undispatched.

### Finding 3 — High: `float_adm_neon.c` float32 reduction instability

`float_adm_sum_cube_neon` and `float_adm_csf_den_scale_neon` accumulated
`val^3` into a `float32x4_t` accumulator and then merged via `vaddvq_f32`.
This is the same tree-reduction stability gap ADR-0138 fixed for the AVX2
path. The AVX2 versions use `_mm256_cvtps_pd` to widen to double before
summing. The NEON functions did not follow this pattern.

**Fix**: Both functions now use `float64x2_t v_accum0/v_accum1` and
`vcvt_f64_f32` before accumulation, matching the AVX2 precedent.

### Finding 4 — Medium: `test_motion_v2_simd.c` skips NEON entirely

The test has `#if ARCH_X86 ... #else (void)fprintf(..., "skipping") #endif`
— the `ubuntu-24.04-arm` CI runner runs the test binary but it immediately
exits. `motion_score_pipeline_16_neon` has never been regression-tested.

**Fix**: Added NEON test arm reusing the existing adversarial fixtures.

### Non-finding — `ms_ssim_decimate_neon.c` FMA is intentional

`h_pass_neon_4` and `v_pass_neon_4` use `vfmaq_n_f32` (explicit FMA), and
the scalar reference functions `h_pass_scalar` / `v_pass_scalar` use
`fmaf()`. Both paths commit to FMA consistently, which is the correct
contract. The existing `test_ms_ssim_decimate` verifies byte-level parity.
No action needed.

### Non-finding — SVE2 is already correct

`moment_sve2.c` and `ssimulacra2_sve2.c` both carry `#pragma STDC FP_CONTRACT OFF`
and are compiled in their own libs with `-ffp-contract=off`. No gap.

## CI coverage summary

| Test file | NEON covered? | Method |
|---|---|---|
| `test_psnr_hvs_neon.c` | Yes | `memcmp` of 8×8 DCT block |
| `test_ms_ssim_decimate.c` | Yes | `memcmp` on 5 size fixtures |
| `test_iqa_convolve.c` | Yes | `memcmp` |
| `test_moment_simd.c` | Yes | relative tolerance 1e-7 |
| `test_ssimulacra2_simd.c` | Yes | `memcmp` |
| `test_cambi_simd.c` | Yes | `memcmp` |
| `test_integer_adm_simd.c` | Partial (integer ADM only) | — |
| `test_motion_v2_simd.c` | **No → Fixed** | Added in this PR |
| `test_float_adm_simd.c` (DWT2) | **Yes** (2026-06-27, ADR-1057) | `memcmp` of all 4 DWT2 bands vs scalar `adm_dwt2_s`, 9 fixtures; aarch64-only NEON comparison |
| float_adm sum_cube/csf / float_motion / float_psnr NEON | **No** | No dedicated unit test (and undispatched — see Finding 2/3); covered end-to-end by Netflix golden gate on `ubuntu-24.04-arm` runner |

The float-ADM / float-motion / float-PSNR NEON functions lack dedicated
bit-exact unit tests. They are exercised indirectly through the full VMAF
score comparison against Netflix golden data in the build-matrix CI job.
Adding dedicated unit tests is a follow-up to this PR.
