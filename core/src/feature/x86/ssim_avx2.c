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
#include "ssim_avx2.h"

/*
 * Bit-exact AVX2 SSIM SIMD helpers — mirror the scalar reference in
 * libvmaf/src/feature/iqa/ssim_tools.c byte-for-byte under
 * FLT_EVAL_METHOD == 0. `precompute` and `variance` are pure elementwise
 * float operations (no reductions) — float IEEE-754 ops are identical to
 * scalar by construction. `accumulate` is the tricky one: scalar computes
 * l and c in *double* (because of the `2.0 * ...` literal) and does the
 * final `l*c*s` product in double too, whereas doing it all in vector
 * float diverges by ~0.13 float ULPs on MS-SSIM. We keep SIMD for the
 * float-valued intermediates (srsc, denominators, sv) and do the
 * double-precision numerator + division + product per-lane in scalar
 * double, preserving both the type promotions and the reduction order.
 * See docs/adr/0139-ssim-simd-bitexact-double.md.
 */

void ssim_precompute_avx2(const float *ref, const float *cmp, float *ref_sq, float *cmp_sq,
                          float *ref_cmp, int n)
{
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 r = _mm256_loadu_ps(ref + i);
        __m256 c = _mm256_loadu_ps(cmp + i);
        _mm256_storeu_ps(ref_sq + i, _mm256_mul_ps(r, r));
        _mm256_storeu_ps(cmp_sq + i, _mm256_mul_ps(c, c));
        _mm256_storeu_ps(ref_cmp + i, _mm256_mul_ps(r, c));
    }
    for (; i < n; i++) {
        ref_sq[i] = ref[i] * ref[i];
        cmp_sq[i] = cmp[i] * cmp[i];
        ref_cmp[i] = ref[i] * cmp[i];
    }
}

void ssim_variance_avx2(float *ref_sigma_sqd, float *cmp_sigma_sqd, float *sigma_both,
                        const float *ref_mu, const float *cmp_mu, int n)
{
    __m256 zero = _mm256_setzero_ps();
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 rm = _mm256_loadu_ps(ref_mu + i);
        __m256 cm = _mm256_loadu_ps(cmp_mu + i);

        __m256 rs = _mm256_loadu_ps(ref_sigma_sqd + i);
        rs = _mm256_sub_ps(rs, _mm256_mul_ps(rm, rm));
        rs = _mm256_max_ps(rs, zero);
        _mm256_storeu_ps(ref_sigma_sqd + i, rs);

        __m256 cs = _mm256_loadu_ps(cmp_sigma_sqd + i);
        cs = _mm256_sub_ps(cs, _mm256_mul_ps(cm, cm));
        cs = _mm256_max_ps(cs, zero);
        _mm256_storeu_ps(cmp_sigma_sqd + i, cs);

        __m256 sb = _mm256_loadu_ps(sigma_both + i);
        sb = _mm256_sub_ps(sb, _mm256_mul_ps(rm, cm));
        _mm256_storeu_ps(sigma_both + i, sb);
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

/* 8-wide SIMD block: compute float intermediates, spill to aligned
 * buffers, reduce per-lane in scalar double (ADR-0139). */
static inline void ssim_accumulate_block_avx2(const float *ref_mu, const float *cmp_mu,
                                              const float *ref_sigma_sqd,
                                              const float *cmp_sigma_sqd, const float *sigma_both,
                                              int i, __m256 vC1, __m256 vC2, __m256 vC3,
                                              __m256 vzero, float C1, float C2, double *local_ssim,
                                              double *local_l, double *local_c, double *local_s)
{
    const __m256 rm = _mm256_loadu_ps(ref_mu + i);
    const __m256 cm = _mm256_loadu_ps(cmp_mu + i);
    const __m256 rs = _mm256_loadu_ps(ref_sigma_sqd + i);
    const __m256 cs = _mm256_loadu_ps(cmp_sigma_sqd + i);
    const __m256 sb = _mm256_loadu_ps(sigma_both + i);
    const __m256 srsc = _mm256_sqrt_ps(_mm256_mul_ps(rs, cs));
    const __m256 l_den =
        _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(rm, rm), _mm256_mul_ps(cm, cm)), vC1);
    const __m256 c_den = _mm256_add_ps(_mm256_add_ps(rs, cs), vC2);
    const __m256 sb_neg = _mm256_cmp_ps(sb, vzero, _CMP_LT_OQ);
    const __m256 srsc_le0 = _mm256_cmp_ps(srsc, vzero, _CMP_LE_OQ);
    const __m256 clamped_sb = _mm256_blendv_ps(sb, vzero, _mm256_and_ps(sb_neg, srsc_le0));
    const __m256 sv_f = _mm256_div_ps(_mm256_add_ps(clamped_sb, vC3), _mm256_add_ps(srsc, vC3));

    _Alignas(32) float t_rm[8];
    _Alignas(32) float t_cm[8];
    _Alignas(32) float t_srsc[8];
    _Alignas(32) float t_l_den[8];
    _Alignas(32) float t_c_den[8];
    _Alignas(32) float t_sv[8];
    _mm256_store_ps(t_rm, rm);
    _mm256_store_ps(t_cm, cm);
    _mm256_store_ps(t_srsc, srsc);
    _mm256_store_ps(t_l_den, l_den);
    _mm256_store_ps(t_c_den, c_den);
    _mm256_store_ps(t_sv, sv_f);

    for (int k = 0; k < 8; k++) {
        ssim_accumulate_lane(t_rm[k], t_cm[k], t_srsc[k], t_l_den[k], t_c_den[k], t_sv[k], C1, C2,
                             local_ssim, local_l, local_c, local_s);
    }
}

void ssim_accumulate_avx2(const float *ref_mu, const float *cmp_mu, const float *ref_sigma_sqd,
                          const float *cmp_sigma_sqd, const float *sigma_both, int n, float C1,
                          float C2, float C3, double *ssim_sum, double *l_sum, double *c_sum,
                          double *s_sum)
{
    const __m256 vC1 = _mm256_set1_ps(C1);
    const __m256 vC2 = _mm256_set1_ps(C2);
    const __m256 vC3 = _mm256_set1_ps(C3);
    const __m256 vzero = _mm256_setzero_ps();
    double local_ssim = 0.0;
    double local_l = 0.0;
    double local_c = 0.0;
    double local_s = 0.0;

    int i = 0;
    for (; i + 8 <= n; i += 8) {
        ssim_accumulate_block_avx2(ref_mu, cmp_mu, ref_sigma_sqd, cmp_sigma_sqd, sigma_both, i, vC1,
                                   vC2, vC3, vzero, C1, C2, &local_ssim, &local_l, &local_c,
                                   &local_s);
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
