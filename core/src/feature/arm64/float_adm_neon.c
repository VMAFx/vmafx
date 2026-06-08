/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <math.h>
#include <arm_neon.h>
#include "float_adm_neon.h"
#include "mem.h"

/* float_adm_dwt2_neon is defined in float_adm_dwt2_neon.c, which lives in
 * its own translation unit compiled with explicit `-ffp-contract=off` plus
 * `#pragma clang fp contract(off)` / GCC optimize attribute guards.  The
 * DWT2 kernel used `vmlaq_laneq_f32` (hardware FMA), which produced a 1-ULP
 * divergence from the scalar reference on ARM CI.  Separating it ensures no
 * other TU in this file inherits or resets the contraction guard.
 * See float_adm_dwt2_neon.c and ADR-1057. */

void float_adm_csf_neon(const float *src, float *dst, float *flt, int w, int h, int src_stride,
                        int dst_stride, float factor, float one_by_30)
{
    int src_px_stride = src_stride / sizeof(float);
    int dst_px_stride = dst_stride / sizeof(float);

    float32x4_t v_factor = vdupq_n_f32(factor);
    float32x4_t v_one_by_30 = vdupq_n_f32(one_by_30);

    int i, j;

    for (i = 0; i < h; ++i) {
        int src_off = i * src_px_stride;
        int dst_off = i * dst_px_stride;

        j = 0;
        for (; j + 3 < w; j += 4) {
            float32x4_t s = vld1q_f32(src + src_off + j);
            float32x4_t dst_val = vmulq_f32(v_factor, s);
            vst1q_f32(dst + dst_off + j, dst_val);
            float32x4_t abs_dst = vabsq_f32(dst_val);
            float32x4_t flt_val = vmulq_f32(v_one_by_30, abs_dst);
            vst1q_f32(flt + dst_off + j, flt_val);
        }

        /* Scalar tail. */
        for (; j < w; ++j) {
            float dst_val = factor * src[src_off + j];
            dst[dst_off + j] = dst_val;
            flt[dst_off + j] = one_by_30 * fabsf(dst_val);
        }
    }
}

float float_adm_csf_den_scale_neon(const float *src, int w, int h, int src_stride, int left,
                                   int top, int right, int bottom, float factor)
{
    (void)w;
    (void)h;
    int src_px_stride = src_stride / sizeof(float);

    float32x4_t v_factor = vdupq_n_f32(factor);
    /* ADR-0873 / ADR-0138: accumulate in double to match the AVX2
     * _mm256_cvtps_pd strategy and bound tree-reduction ULP error. */
    float64x2_t v_accum0 = vdupq_n_f64(0.0);
    float64x2_t v_accum1 = vdupq_n_f64(0.0);

    int i, j;
    double accum = 0.0;

    for (i = top; i < bottom; ++i) {
        const float *row = src + i * src_px_stride;

        j = left;
        for (; j + 3 < right; j += 4) {
            float32x4_t s = vld1q_f32(row + j);
            float32x4_t val = vabsq_f32(vmulq_f32(v_factor, s));
            /* val^3 = val * val * val */
            float32x4_t val2 = vmulq_f32(val, val);
            float32x4_t val3 = vmulq_f32(val2, val);
            /* Widen to double before accumulating (ADR-0873). */
            v_accum0 = vaddq_f64(v_accum0, vcvt_f64_f32(vget_low_f32(val3)));
            v_accum1 = vaddq_f64(v_accum1, vcvt_f64_f32(vget_high_f32(val3)));
        }

        /* Scalar tail. */
        for (; j < right; ++j) {
            double val = fabs((double)factor * (double)row[j]);
            accum += val * val * val;
        }
    }

    accum += vaddvq_f64(vaddq_f64(v_accum0, v_accum1));

    return (float)accum;
}

float float_adm_sum_cube_neon(const float *x, int w, int h, int stride, int left, int top,
                              int right, int bottom)
{
    (void)w;
    (void)h;
    int px_stride = stride / sizeof(float);

    /* ADR-0873 / ADR-0138: accumulate in double to match the AVX2
     * _mm256_cvtps_pd strategy and bound tree-reduction ULP error. */
    float64x2_t v_accum0 = vdupq_n_f64(0.0);
    float64x2_t v_accum1 = vdupq_n_f64(0.0);
    double accum = 0.0;

    int i, j;

    for (i = top; i < bottom; ++i) {
        const float *row = x + i * px_stride;

        j = left;
        for (; j + 3 < right; j += 4) {
            float32x4_t s = vld1q_f32(row + j);
            float32x4_t val = vabsq_f32(s);
            /* val^3 = val * val * val */
            float32x4_t val2 = vmulq_f32(val, val);
            float32x4_t val3 = vmulq_f32(val2, val);
            /* Widen to double before accumulating (ADR-0873). */
            v_accum0 = vaddq_f64(v_accum0, vcvt_f64_f32(vget_low_f32(val3)));
            v_accum1 = vaddq_f64(v_accum1, vcvt_f64_f32(vget_high_f32(val3)));
        }

        /* Scalar tail. */
        for (; j < right; ++j) {
            double val = fabs((double)row[j]);
            accum += val * val * val;
        }
    }

    accum += vaddvq_f64(vaddq_f64(v_accum0, v_accum1));

    return (float)accum;
}
