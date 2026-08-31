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
| float_adm / float_motion / float_psnr NEON | **No** | No dedicated unit test; covered end-to-end by Netflix golden gate on `ubuntu-24.04-arm` runner |

The float-ADM / float-motion / float-PSNR NEON functions lack dedicated
bit-exact unit tests. They are exercised indirectly through the full VMAF
score comparison against Netflix golden data in the build-matrix CI job.
Adding dedicated unit tests is a follow-up to this PR.

## Follow-up (2026-08-31) — compiler-matched float-ADM DWT2 contraction

The dedicated `test_float_adm_dwt2_neon` test added on PR #1161 reproduced a
macOS ARM failure after the scalar translation-unit-wide contraction guard was
removed to restore the immutable Netflix score contract. A Clang 22 AArch64
cross-build failed 18 cells of the 3x5 fixture by 1–5 ULP; the same source
passed under GCC 16.

LLVM IR established the cause: Clang lowered the production scalar four-tap
accumulation to `llvm.fmuladd`, while the guarded NEON translation unit emitted
separate `fmul` and `fadd`. GCC 16 kept both scalar and NEON arithmetic split.
It also established that `vmlaq_laneq_f32` is not an unconditional fused
operation under Clang's `contract(off)`; the unconditional fused intrinsic is
`vfmaq_laneq_f32`.

The smallest parity-preserving fix is therefore compiler-matched and NEON-local:

| Compiler | Vector accumulation | Tail / horizontal accumulation |
|---|---|---|
| Clang | `vfmaq_laneq_f32` | `fmaf` |
| GCC | `vmulq_laneq_f32` then `vaddq_f32` | separate multiply/add |

Both implementations start from `0.0f` and keep the production scalar tap
order. The translation unit retains `-ffp-contract=off` so only the explicit
Clang operations fuse. The exhaustive geometry/stride suite passes with both
Clang 22 and GCC 16 AArch64 cross-builds through QEMU. No scalar ADM source,
Netflix golden assertion, snapshot, or tolerance changed.
