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

#include <arm_neon.h>

#include "../iqa/ssim_accumulate_lane.h"
#include "../simd_dx.h"
#include "ssim_neon.h"

/*
 * Bit-exact NEON SSIM SIMD helpers — mirror the scalar reference in
 * libvmaf/src/feature/iqa/ssim_tools.c byte-for-byte under
 * FLT_EVAL_METHOD == 0. See ADR-0139. `precompute` and `variance` are
 * pure elementwise float ops and are bit-exact to scalar by
 * construction. `accumulate` is the tricky one: scalar computes l and c
 * in double (because of the `2.0 * ...` literal) and does the final
 * l*c*s product in double, so we keep SIMD for the float-valued
 * intermediates (srsc, denominators, sv) and do the double-precision
 * numerator + division + product per-lane in scalar double, preserving
 * both the type promotions and the reduction order.
 */

void ssim_precompute_neon(const float *ref, const float *cmp, float *ref_sq, float *cmp_sq,
                          float *ref_cmp, int n)
{
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t r = vld1q_f32(ref + i);
        float32x4_t c = vld1q_f32(cmp + i);
        vst1q_f32(ref_sq + i, vmulq_f32(r, r));
        vst1q_f32(cmp_sq + i, vmulq_f32(c, c));
        vst1q_f32(ref_cmp + i, vmulq_f32(r, c));
    }
    for (; i < n; i++) {
        ref_sq[i] = ref[i] * ref[i];
        cmp_sq[i] = cmp[i] * cmp[i];
        ref_cmp[i] = ref[i] * cmp[i];
    }
}

/* Vector transcription of the scalar clamp `MAX(0.0, x)` in
 * ssim_variance_scalar (iqa/ssim_tools.c), i.e. `(0.0 > x) ? 0.0 : x`.
 *
 * NOT `vmaxq_f32(x, zero)`: FMAX forces a zero result positive — ARM ARM
 * `FPMax` ends with `if type == FPType_Zero then sign = sign1 AND sign2`
 * — so it maps -0.0f to +0.0f, while the scalar reference (and this
 * kernel's own scalar tail, which spells the clamp `if (x < 0.0f) x =
 * 0.0f`) return -0.0f unchanged. `vcltq_f32` is false for NaN, matching
 * `0.0 > NaN`, so NaN passes through on both paths as before. See
 * test_ssim_neon.c. */
static inline float32x4_t ssim_clamp_nonneg_neon(float32x4_t x, float32x4_t zero)
{
    return vbslq_f32(vcltq_f32(x, zero), zero, x);
}

void ssim_variance_neon(float *ref_sigma_sqd, float *cmp_sigma_sqd, float *sigma_both,
                        const float *ref_mu, const float *cmp_mu, int n)
{
    float32x4_t zero = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t rm = vld1q_f32(ref_mu + i);
        float32x4_t cm = vld1q_f32(cmp_mu + i);

        float32x4_t rs = vld1q_f32(ref_sigma_sqd + i);
        rs = vsubq_f32(rs, vmulq_f32(rm, rm));
        rs = ssim_clamp_nonneg_neon(rs, zero);
        vst1q_f32(ref_sigma_sqd + i, rs);

        float32x4_t cs = vld1q_f32(cmp_sigma_sqd + i);
        cs = vsubq_f32(cs, vmulq_f32(cm, cm));
        cs = ssim_clamp_nonneg_neon(cs, zero);
        vst1q_f32(cmp_sigma_sqd + i, cs);

        float32x4_t sb = vld1q_f32(sigma_both + i);
        sb = vsubq_f32(sb, vmulq_f32(rm, cm));
        vst1q_f32(sigma_both + i, sb);
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

/* 4-wide NEON block — see ssim_avx2.c for the AVX2 companion. */
static inline void ssim_accumulate_block_neon(const float *ref_mu, const float *cmp_mu,
                                              const float *ref_sigma_sqd,
                                              const float *cmp_sigma_sqd, const float *sigma_both,
                                              int i, float32x4_t vC1, float32x4_t vC2,
                                              float32x4_t vC3, float32x4_t vzero, float C1,
                                              float C2, double *local_ssim, double *local_l,
                                              double *local_c, double *local_s)
{
    const float32x4_t rm = vld1q_f32(ref_mu + i);
    const float32x4_t cm = vld1q_f32(cmp_mu + i);
    const float32x4_t rs = vld1q_f32(ref_sigma_sqd + i);
    const float32x4_t cs = vld1q_f32(cmp_sigma_sqd + i);
    const float32x4_t sb = vld1q_f32(sigma_both + i);
    const float32x4_t srsc = vsqrtq_f32(vmulq_f32(rs, cs));
    const float32x4_t l_den = vaddq_f32(vaddq_f32(vmulq_f32(rm, rm), vmulq_f32(cm, cm)), vC1);
    const float32x4_t c_den = vaddq_f32(vaddq_f32(rs, cs), vC2);
    const uint32x4_t sb_neg = vcltq_f32(sb, vzero);
    const uint32x4_t srsc_le0 = vcleq_f32(srsc, vzero);
    const float32x4_t clamped_sb = vbslq_f32(vandq_u32(sb_neg, srsc_le0), vzero, sb);
    const float32x4_t sv_f = vdivq_f32(vaddq_f32(clamped_sb, vC3), vaddq_f32(srsc, vC3));

    SIMD_ALIGNED_F32_BUF_NEON(t_rm);
    SIMD_ALIGNED_F32_BUF_NEON(t_cm);
    SIMD_ALIGNED_F32_BUF_NEON(t_srsc);
    SIMD_ALIGNED_F32_BUF_NEON(t_l_den);
    SIMD_ALIGNED_F32_BUF_NEON(t_c_den);
    SIMD_ALIGNED_F32_BUF_NEON(t_sv);
    vst1q_f32(t_rm, rm);
    vst1q_f32(t_cm, cm);
    vst1q_f32(t_srsc, srsc);
    vst1q_f32(t_l_den, l_den);
    vst1q_f32(t_c_den, c_den);
    vst1q_f32(t_sv, sv_f);

    for (int k = 0; k < SIMD_LANES_NEON; k++) {
        ssim_accumulate_lane(t_rm[k], t_cm[k], t_srsc[k], t_l_den[k], t_c_den[k], t_sv[k], C1, C2,
                             local_ssim, local_l, local_c, local_s);
    }
}

void ssim_accumulate_neon(const float *ref_mu, const float *cmp_mu, const float *ref_sigma_sqd,
                          const float *cmp_sigma_sqd, const float *sigma_both, int n, float C1,
                          float C2, float C3, double *ssim_sum, double *l_sum, double *c_sum,
                          double *s_sum)
{
    const float32x4_t vC1 = vdupq_n_f32(C1);
    const float32x4_t vC2 = vdupq_n_f32(C2);
    const float32x4_t vC3 = vdupq_n_f32(C3);
    const float32x4_t vzero = vdupq_n_f32(0.0f);
    double local_ssim = 0.0;
    double local_l = 0.0;
    double local_c = 0.0;
    double local_s = 0.0;

    int i = 0;
    for (; i + SIMD_LANES_NEON <= n; i += SIMD_LANES_NEON) {
        ssim_accumulate_block_neon(ref_mu, cmp_mu, ref_sigma_sqd, cmp_sigma_sqd, sigma_both, i, vC1,
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
