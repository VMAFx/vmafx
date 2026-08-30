/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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

#include <immintrin.h>

#include "../iqa/ssim_accumulate_lane.h"
#include "ssim_avx512.h"

/*
 * Bit-exact AVX-512 SSIM SIMD helpers — 16-lane widening of the AVX2
 * variant in ssim_avx2.c. Scalar invariants preserved byte-for-byte:
 *   - `precompute` / `variance` are pure elementwise float ops.
 *   - `accumulate` uses SIMD only for float intermediates (srsc,
 *     denominators, sv). The double-precision numerator of l and c
 *     (`2.0 * ...`) and the final `l*c*s` product are computed per-lane
 *     in scalar double, matching scalar C's type promotion rules.
 * See docs/adr/0139-ssim-simd-bitexact-double.md.
 */

void ssim_precompute_avx512(const float *ref, const float *cmp, float *ref_sq, float *cmp_sq,
                            float *ref_cmp, int n)
{
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m512 r = _mm512_loadu_ps(ref + i);
        __m512 c = _mm512_loadu_ps(cmp + i);
        _mm512_storeu_ps(ref_sq + i, _mm512_mul_ps(r, r));
        _mm512_storeu_ps(cmp_sq + i, _mm512_mul_ps(c, c));
        _mm512_storeu_ps(ref_cmp + i, _mm512_mul_ps(r, c));
    }
    for (; i < n; i++) {
        ref_sq[i] = ref[i] * ref[i];
        cmp_sq[i] = cmp[i] * cmp[i];
        ref_cmp[i] = ref[i] * cmp[i];
    }
}

void ssim_variance_avx512(float *ref_sigma_sqd, float *cmp_sigma_sqd, float *sigma_both,
                          const float *ref_mu, const float *cmp_mu, int n)
{
    __m512 zero = _mm512_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m512 rm = _mm512_loadu_ps(ref_mu + i);
        __m512 cm = _mm512_loadu_ps(cmp_mu + i);

        __m512 rs = _mm512_loadu_ps(ref_sigma_sqd + i);
        rs = _mm512_sub_ps(rs, _mm512_mul_ps(rm, rm));
        rs = _mm512_max_ps(rs, zero);
        _mm512_storeu_ps(ref_sigma_sqd + i, rs);

        __m512 cs = _mm512_loadu_ps(cmp_sigma_sqd + i);
        cs = _mm512_sub_ps(cs, _mm512_mul_ps(cm, cm));
        cs = _mm512_max_ps(cs, zero);
        _mm512_storeu_ps(cmp_sigma_sqd + i, cs);

        __m512 sb = _mm512_loadu_ps(sigma_both + i);
        sb = _mm512_sub_ps(sb, _mm512_mul_ps(rm, cm));
        _mm512_storeu_ps(sigma_both + i, sb);
    }
    for (; i < n; i++) {
        ref_sigma_sqd[i] -= ref_mu[i] * ref_mu[i];
        if (ref_sigma_sqd[i] < 0.0f)
            ref_sigma_sqd[i] = 0.0f;
        cmp_sigma_sqd[i] -= cmp_mu[i] * cmp_mu[i];
        if (cmp_sigma_sqd[i] < 0.0f)
            cmp_sigma_sqd[i] = 0.0f;
        sigma_both[i] -= ref_mu[i] * cmp_mu[i];
    }
}

/*
 * Per-half (8-lane) double-precision reduction. Mirrors the per-lane
 * formula in `ssim_accumulate_lane` exactly (same operands, same op
 * order, no FMA contraction) but performs the float→double promotion,
 * the `2.0 * rm * cm + C1` numerator, the `(2.0 * srsc + C2)`
 * numerator, and both divisions in `__m512d` vector form. Lane-wise
 * IEEE-754 guarantees byte-identical per-lane `lv`, `cv`, `sv`,
 * `ssim_lane = lv*cv*sv` against the scalar fallback. Results are
 * spilled to aligned `double[8]` so the caller can sum them
 * left-to-right scalar-style — preserving the running-sum
 * associativity required by ADR-0139.
 */
static inline void ssim_block_double_half_avx512(__m256 rm_f, __m256 cm_f, __m256 srsc_f,
                                                 __m256 l_den_f, __m256 c_den_f, __m256 sv_ff,
                                                 __m512d vC1d, __m512d vC2d, __m512d v2d,
                                                 double *t_lv, double *t_cv, double *t_sv,
                                                 double *t_ssim)
{
    /* Float→double widening is exact for finite floats (IEEE-754). */
    const __m512d rm = _mm512_cvtps_pd(rm_f);
    const __m512d cm = _mm512_cvtps_pd(cm_f);
    const __m512d srsc = _mm512_cvtps_pd(srsc_f);
    const __m512d l_den = _mm512_cvtps_pd(l_den_f);
    const __m512d c_den = _mm512_cvtps_pd(c_den_f);
    const __m512d sv = _mm512_cvtps_pd(sv_ff);

    /* lv = (2.0 * rm * cm + C1) / l_den. Order matches scalar
     * `ssim_accumulate_lane`: ((2*rm)*cm + C1)/l_den. No FMA
     * contraction (separate _mm512_mul_pd + _mm512_add_pd). */
    const __m512d two_rm = _mm512_mul_pd(v2d, rm);
    const __m512d two_rm_cm = _mm512_mul_pd(two_rm, cm);
    const __m512d lv_num = _mm512_add_pd(two_rm_cm, vC1d);
    const __m512d lv = _mm512_div_pd(lv_num, l_den);

    /* cv = (2.0 * srsc + C2) / c_den. */
    const __m512d two_srsc = _mm512_mul_pd(v2d, srsc);
    const __m512d cv_num = _mm512_add_pd(two_srsc, vC2d);
    const __m512d cv = _mm512_div_pd(cv_num, c_den);

    /* ssim_lane = lv * cv * sv. Same left-associative order as
     * scalar `lv * cv * sv` (i.e. `(lv*cv)*sv`). */
    const __m512d lv_cv = _mm512_mul_pd(lv, cv);
    const __m512d ssim_lane = _mm512_mul_pd(lv_cv, sv);

    _mm512_store_pd(t_lv, lv);
    _mm512_store_pd(t_cv, cv);
    _mm512_store_pd(t_sv, sv);
    _mm512_store_pd(t_ssim, ssim_lane);
}

/* 16-wide SIMD block — see ssim_avx2.c for the AVX2 companion.
 *
 * Performance note (PR #333 opt #2): the float intermediates are
 * computed in `__m512` as before, but the per-lane double reduction
 * is now done via two 8-wide `__m512d` passes (low half + high half)
 * instead of 16 scalar invocations of `ssim_accumulate_lane`. This
 * preserves bit-exactness against scalar (each lane uses the same
 * IEEE-754 ops on the same operands; lane-wise vector double = lane
 * scalar double by IEEE-754 mandate) while removing the 384-byte
 * stack spill + 16-iteration scalar loop that dominated the function
 * profile. The final lane-by-lane summation into `local_*` stays
 * scalar left-to-right so the running-sum associativity required by
 * ADR-0139 is unchanged.
 */
static inline void ssim_accumulate_block_avx512(const float *ref_mu, const float *cmp_mu,
                                                const float *ref_sigma_sqd,
                                                const float *cmp_sigma_sqd, const float *sigma_both,
                                                int i, __m512 vC1, __m512 vC2, __m512 vC3,
                                                __m512 vzero, __m512d vC1d, __m512d vC2d,
                                                __m512d v2d, double *local_ssim, double *local_l,
                                                double *local_c, double *local_s)
{
    const __m512 rm = _mm512_loadu_ps(ref_mu + i);
    const __m512 cm = _mm512_loadu_ps(cmp_mu + i);
    const __m512 rs = _mm512_loadu_ps(ref_sigma_sqd + i);
    const __m512 cs = _mm512_loadu_ps(cmp_sigma_sqd + i);
    const __m512 sb = _mm512_loadu_ps(sigma_both + i);
    const __m512 srsc = _mm512_sqrt_ps(_mm512_mul_ps(rs, cs));
    const __m512 l_den =
        _mm512_add_ps(_mm512_add_ps(_mm512_mul_ps(rm, rm), _mm512_mul_ps(cm, cm)), vC1);
    const __m512 c_den = _mm512_add_ps(_mm512_add_ps(rs, cs), vC2);
    const __mmask16 sb_neg = _mm512_cmp_ps_mask(sb, vzero, _CMP_LT_OQ);
    const __mmask16 srsc_le0 = _mm512_cmp_ps_mask(srsc, vzero, _CMP_LE_OQ);
    const __m512 clamped_sb = _mm512_mask_blend_ps(sb_neg & srsc_le0, sb, vzero);
    const __m512 sv_f = _mm512_div_ps(_mm512_add_ps(clamped_sb, vC3), _mm512_add_ps(srsc, vC3));

    /* Split the 16-lane float vectors into two 8-lane __m256 halves
     * for the __m512d widening. Lane order is preserved: low half =
     * lanes 0..7, high half = lanes 8..15 — matching the linear
     * scalar reduction order of the previous implementation. */
    const __m256 rm_lo = _mm512_castps512_ps256(rm);
    const __m256 rm_hi = _mm512_extractf32x8_ps(rm, 1);
    const __m256 cm_lo = _mm512_castps512_ps256(cm);
    const __m256 cm_hi = _mm512_extractf32x8_ps(cm, 1);
    const __m256 srsc_lo = _mm512_castps512_ps256(srsc);
    const __m256 srsc_hi = _mm512_extractf32x8_ps(srsc, 1);
    const __m256 l_den_lo = _mm512_castps512_ps256(l_den);
    const __m256 l_den_hi = _mm512_extractf32x8_ps(l_den, 1);
    const __m256 c_den_lo = _mm512_castps512_ps256(c_den);
    const __m256 c_den_hi = _mm512_extractf32x8_ps(c_den, 1);
    const __m256 sv_lo = _mm512_castps512_ps256(sv_f);
    const __m256 sv_hi = _mm512_extractf32x8_ps(sv_f, 1);

    _Alignas(64) double t_lv[16];
    _Alignas(64) double t_cv[16];
    _Alignas(64) double t_sv[16];
    _Alignas(64) double t_ssim[16];

    ssim_block_double_half_avx512(rm_lo, cm_lo, srsc_lo, l_den_lo, c_den_lo, sv_lo, vC1d, vC2d, v2d,
                                  t_lv, t_cv, t_sv, t_ssim);
    ssim_block_double_half_avx512(rm_hi, cm_hi, srsc_hi, l_den_hi, c_den_hi, sv_hi, vC1d, vC2d, v2d,
                                  &t_lv[8], &t_cv[8], &t_sv[8], &t_ssim[8]);

    /* Lane-by-lane left-to-right scalar accumulation — preserves
     * the running-sum order of the prior implementation (and of
     * `ssim_accumulate_default_scalar`). Per-lane bit-equality of
     * `t_*[k]` against the prior scalar path is guaranteed by
     * IEEE-754 (same ops on same operands). */
    for (int k = 0; k < 16; k++) {
        *local_ssim += t_ssim[k];
        *local_l += t_lv[k];
        *local_c += t_cv[k];
        *local_s += t_sv[k];
    }
}

void ssim_accumulate_avx512(const float *ref_mu, const float *cmp_mu, const float *ref_sigma_sqd,
                            const float *cmp_sigma_sqd, const float *sigma_both, int n, float C1,
                            float C2, float C3, double *ssim_sum, double *l_sum, double *c_sum,
                            double *s_sum)
{
    const __m512 vC1 = _mm512_set1_ps(C1);
    const __m512 vC2 = _mm512_set1_ps(C2);
    const __m512 vC3 = _mm512_set1_ps(C3);
    const __m512 vzero = _mm512_setzero_ps();
    /* Double-precision broadcast constants for the per-lane reduction.
     * `(double)C1` mirrors scalar's implicit float→double promotion in
     * `2.0 * rm * cm + C1` (the `2.0` literal is `double`, dragging C1
     * up via the usual arithmetic conversions). */
    const __m512d vC1d = _mm512_set1_pd((double)C1);
    const __m512d vC2d = _mm512_set1_pd((double)C2);
    const __m512d v2d = _mm512_set1_pd(2.0);
    double local_ssim = 0.0;
    double local_l = 0.0;
    double local_c = 0.0;
    double local_s = 0.0;

    int i = 0;
    for (; i + 16 <= n; i += 16) {
        ssim_accumulate_block_avx512(ref_mu, cmp_mu, ref_sigma_sqd, cmp_sigma_sqd, sigma_both, i,
                                     vC1, vC2, vC3, vzero, vC1d, vC2d, v2d, &local_ssim, &local_l,
                                     &local_c, &local_s);
    }
    for (; i < n; i++) {
        ssim_accumulate_scalar_step(ref_mu[i], cmp_mu[i], ref_sigma_sqd[i], cmp_sigma_sqd[i],
                                    sigma_both[i], C1, C2, C3, &local_ssim, &local_l, &local_c,
                                    &local_s);
    }
    *ssim_sum += local_ssim;
    *l_sum += local_l;
    *c_sum += local_c;
    *s_sum += local_s;
}
