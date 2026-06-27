/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  NEON implementations of compute_1st_moment / compute_2nd_moment for
 *  the float_moment feature extractor.  Closes the only remaining
 *  fully-scalar row in the SIMD matrix (T7-19, ADR-0179).
 *
 *  Bit-exactness contract: lanes are widened to f64 before summation
 *  (matches `float_psnr_neon.c`'s pattern) so the order divergence is
 *  bounded to the per-lane cross-lane add and the per-row tail.
 */
#include <arm_neon.h>
#include <assert.h>
#include <stddef.h>

#include "moment_neon.h"

int compute_1st_moment_neon(const float *pic, int w, int h, int stride, double *score)
{
    assert(pic != NULL);
    assert(score != NULL);
    assert(w > 0);
    assert(h > 0);

    const int stride_f = stride / (int)sizeof(float);
    float64x2_t dsum0 = vdupq_n_f64(0.0);
    float64x2_t dsum1 = vdupq_n_f64(0.0);

    for (int i = 0; i < h; ++i) {
        const float *row = pic + (size_t)i * (size_t)stride_f;
        int j = 0;

        for (; j + 4 <= w; j += 4) {
            const float32x4_t v = vld1q_f32(row + j);
            dsum0 = vaddq_f64(dsum0, vcvt_f64_f32(vget_low_f32(v)));
            dsum1 = vaddq_f64(dsum1, vcvt_f64_f32(vget_high_f32(v)));
        }
        double tail = 0.0;
        for (; j < w; ++j)
            tail += (double)row[j];
        if (tail != 0.0)
            dsum0 = vaddq_f64(dsum0, vsetq_lane_f64(tail, vdupq_n_f64(0.0), 0));
    }

    double cum = vaddvq_f64(vaddq_f64(dsum0, dsum1));
    cum /= (double)w * (double)h;
    *score = cum;
    return 0;
}

int compute_2nd_moment_neon(const float *pic, int w, int h, int stride, double *score)
{
    assert(pic != NULL);
    assert(score != NULL);
    assert(w > 0);
    assert(h > 0);

    const int stride_f = stride / (int)sizeof(float);
    float64x2_t dsum0 = vdupq_n_f64(0.0);
    float64x2_t dsum1 = vdupq_n_f64(0.0);

    for (int i = 0; i < h; ++i) {
        const float *row = pic + (size_t)i * (size_t)stride_f;
        int j = 0;

        for (; j + 4 <= w; j += 4) {
            const float32x4_t v = vld1q_f32(row + j);
            const float32x4_t sq = vmulq_f32(v, v);
            dsum0 = vaddq_f64(dsum0, vcvt_f64_f32(vget_low_f32(sq)));
            dsum1 = vaddq_f64(dsum1, vcvt_f64_f32(vget_high_f32(sq)));
        }
        double tail = 0.0;
        for (; j < w; ++j) {
            /* Square in float (not double) so the tail is bit-exact to the
             * scalar reference (moment.c:compute_2nd_moment, `pic_ * pic_`)
             * and to the SIMD main loop (`vmulq_f32(v, v)`).  Squaring in
             * double here would round differently and break the ADR-0179
             * bit-exactness contract on rows whose width is not a multiple of
             * the SIMD lane count. */
            const float p = row[j];
            tail += (double)(p * p);
        }
        if (tail != 0.0)
            dsum0 = vaddq_f64(dsum0, vsetq_lane_f64(tail, vdupq_n_f64(0.0), 0));
    }

    double cum = vaddvq_f64(vaddq_f64(dsum0, dsum1));
    cum /= (double)w * (double)h;
    *score = cum;
    return 0;
}
