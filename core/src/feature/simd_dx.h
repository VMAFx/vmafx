/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * simd_dx.h — fork-internal SIMD DX helper macros.
 *
 * ISA-specific macros (no cross-ISA abstraction layer — see
 * ADR-0140 and user-memory `feedback_simd_dx_scope.md`). Macro names
 * encode the ISA in the suffix, so a reader never has to guess which
 * intrinsics a macro expands to.
 *
 * Each macro documents its scalar C equivalent so the bit-exactness
 * invariant is auditable against ssim_tools.c / iqa/convolve.c /
 * ssim_accumulate_default_scalar without opening the intrinsics
 * header.
 *
 * Guidance:
 *   - Include this header from SIMD translation units only.
 *   - Always under the matching ISA ifdef (the macros assume the
 *     ISA's intrinsics header is already included by the TU).
 *   - The macros are statement-expressions wrapped in `do { ... }
 *     while (0)` — use with a trailing `;`.
 *
 * Conventions:
 *   - `_F32_F64_*` — single-precision input, double-precision
 *     accumulator. Single-rounded `float * float` then widen to
 *     double then double add. NO FMA.
 *   - `_4L` / `_8L` — SIMD chunk lane count for the F32 input side.
 *   - `_PER_LANE_SCALAR_DOUBLE_REDUCE_*` — store a SIMD float vector
 *     to an aligned temp buffer and reduce in scalar double to match
 *     scalar C's `double = float_expr` assignment promotion.
 *
 * Governing ADRs:
 *   - ADR-0138 — convolve widen-then-add pattern (`_F32_F64_*`).
 *   - ADR-0139 — per-lane scalar double reduction pattern.
 *   - ADR-0140 — this file.
 */

#ifndef VMAF_FEATURE_SIMD_DX_H_
#define VMAF_FEATURE_SIMD_DX_H_

/* ----------------------------------------------------------------- *
 * Pattern 1: widen-add bit-exact reduction (ADR-0138).
 *
 * Scalar C equivalent:
 *
 *     double acc = 0.0;
 *     for (...) {
 *         float  prod_f = a_f[i] * coeff_f[i];   // single-rounded
 *         double prod_d = (double)prod_f;         // promotion on +=
 *         acc += prod_d;                          // double add
 *     }
 *
 * Why not FMA: FMA collapses `*` + `+` into one round, which is
 * closer to the infinitely precise result but *different* from
 * scalar's two-round sequence. Bit-exactness wins.
 * ----------------------------------------------------------------- */

#if defined(__AVX2__) && !defined(SIMD_DX_NO_AVX2)

/* 4-lane F32 chunk → 4-lane F64 accumulator.
 * `acc_pd4` is an __m256d, `a_ps4` / `coeff_ps4` are __m128.
 */
#define SIMD_WIDEN_ADD_F32_F64_AVX2_4L(acc_pd4, a_ps4, coeff_ps4)                                  \
    do {                                                                                           \
        const __m128 _sdx_prod_f = _mm_mul_ps((a_ps4), (coeff_ps4));                               \
        const __m256d _sdx_prod_d = _mm256_cvtps_pd(_sdx_prod_f);                                  \
        (acc_pd4) = _mm256_add_pd(_sdx_prod_d, (acc_pd4));                                         \
    } while (0)

#endif /* __AVX2__ */

#if defined(__AVX512F__) && !defined(SIMD_DX_NO_AVX512)

/* 8-lane F32 chunk → 8-lane F64 accumulator.
 * `acc_pd8` is a __m512d, `a_ps8` / `coeff_ps8` are __m256.
 */
#define SIMD_WIDEN_ADD_F32_F64_AVX512_8L(acc_pd8, a_ps8, coeff_ps8)                                \
    do {                                                                                           \
        const __m256 _sdx_prod_f = _mm256_mul_ps((a_ps8), (coeff_ps8));                            \
        const __m512d _sdx_prod_d = _mm512_cvtps_pd(_sdx_prod_f);                                  \
        (acc_pd8) = _mm512_add_pd(_sdx_prod_d, (acc_pd8));                                         \
    } while (0)

#endif /* __AVX512F__ */

#if defined(__ARM_NEON) && !defined(SIMD_DX_NO_NEON)

/* 4-lane F32 chunk → paired (low, high) 2-lane F64 accumulators.
 *
 * NEON has no float64x4_t, so callers carry two float64x2_t
 * accumulators matching the low/high halves of the float32x4_t
 * single-rounded product.
 *
 * `acc_pd_lo` / `acc_pd_hi` are float64x2_t, `a_ps4` / `coeff_ps4`
 * are float32x4_t. Lane 0-1 of the product goes into `acc_pd_lo`,
 * lane 2-3 into `acc_pd_hi` — matching scalar C's left-to-right
 * summation order.
 */
#define SIMD_WIDEN_ADD_F32_F64_NEON_4L(acc_pd_lo, acc_pd_hi, a_ps4, coeff_ps4)                     \
    do {                                                                                           \
        const float32x4_t _sdx_prod_f = vmulq_f32((a_ps4), (coeff_ps4));                           \
        const float64x2_t _sdx_prod_lo = vcvt_f64_f32(vget_low_f32(_sdx_prod_f));                  \
        const float64x2_t _sdx_prod_hi = vcvt_high_f64_f32(_sdx_prod_f);                           \
        (acc_pd_lo) = vaddq_f64(_sdx_prod_lo, (acc_pd_lo));                                        \
        (acc_pd_hi) = vaddq_f64(_sdx_prod_hi, (acc_pd_hi));                                        \
    } while (0)

#endif /* __ARM_NEON */

/* ----------------------------------------------------------------- *
 * Pattern 2: per-lane scalar double reduction (ADR-0139).
 *
 * For kernels whose scalar C loop computes a double-precision
 * numerator + divide + product (`2.0 * ref_mu[i] * cmp_mu[i] + C1`
 * etc.), SIMD cannot stay in vector float — the `2.0` C literal is
 * a double and promotes its float operands. The workaround:
 *
 *   1. Keep the bit-exact float ops in SIMD (non-reducing
 *      elementwise multiplies / divides).
 *   2. Store the vector float temporaries to an aligned stack
 *      buffer of `LANES` floats.
 *   3. Run an inner scalar-for-loop 0..LANES-1 that reproduces the
 *      scalar C type promotions exactly.
 *
 * The macros below are *declarative* — they expand to the aligned
 * buffer declaration; the per-lane loop stays in the caller (it
 * needs access to feature-specific scalar math).
 *
 * Usage example:
 *
 *   SIMD_ALIGNED_F32_BUF_AVX2(t_rm);
 *   SIMD_ALIGNED_F32_BUF_AVX2(t_cm);
 *   ...
 *   _mm256_store_ps(t_rm, rm_vec);
 *   _mm256_store_ps(t_cm, cm_vec);
 *   for (int k = 0; k < 8; k++) {
 *       double lv = (2.0 * t_rm[k] * t_cm[k] + C1) / t_l_den[k];
 *       ...
 *   }
 *
 * LANES is implicit in the macro name: 8 for AVX2, 16 for AVX-512,
 * 4 for NEON.
 * ----------------------------------------------------------------- */

#if defined(__AVX2__) && !defined(SIMD_DX_NO_AVX2)

/* Declare an 8-element float buffer aligned to 32 B for __m256
 * stores. Scope: block. */
#define SIMD_ALIGNED_F32_BUF_AVX2(name) _Alignas(32) float name[8]

/* Lane count for AVX2 `ps` operations. */
#define SIMD_LANES_AVX2 8

#endif /* __AVX2__ */

#if defined(__AVX512F__) && !defined(SIMD_DX_NO_AVX512)

#define SIMD_ALIGNED_F32_BUF_AVX512(name) _Alignas(64) float name[16]
#define SIMD_LANES_AVX512 16

#endif /* __AVX512F__ */

#if defined(__ARM_NEON) && !defined(SIMD_DX_NO_NEON)

/* NEON 128-bit `float32x4_t` covers 4 lanes. 16-byte aligned. */
#define SIMD_ALIGNED_F32_BUF_NEON(name) _Alignas(16) float name[4]
#define SIMD_LANES_NEON 4

#endif /* __ARM_NEON */

#endif /* VMAF_FEATURE_SIMD_DX_H_ */
