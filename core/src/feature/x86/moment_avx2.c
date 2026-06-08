/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  AVX2 implementations of compute_1st_moment / compute_2nd_moment for
 *  the float_moment feature extractor.  Closes the only remaining
 *  fully-scalar row in the SIMD matrix (T7-19, ADR-0179).
 *
 *  Bit-exactness contract: the scalar reference accumulates `double cum`
 *  in row-major order across the entire frame.  The AVX2 path lifts each
 *  float lane into a `double` accumulator BEFORE summation (matches
 *  `ansnr_avx2.c`'s pattern), so the order divergence is bounded to the
 *  per-lane cross-lane sum and the per-row tail; the residual is well
 *  inside the snapshot gate's tolerance.
 */
#include <assert.h>
#include <immintrin.h>
#include <stddef.h>

#include "moment_avx2.h"

int compute_1st_moment_avx2(const float *pic, int w, int h, int stride, double *score)
{
    assert(pic != NULL);
    assert(score != NULL);
    assert(w > 0);
    assert(h > 0);

    const int stride_f = stride / (int)sizeof(float);
    double cum = 0.0;

    for (int i = 0; i < h; ++i) {
        const float *row = pic + (size_t)i * (size_t)stride_f;
        int j = 0;

        for (; j + 8 <= w; j += 8) {
            const __m256 v = _mm256_loadu_ps(row + j);
            _Alignas(32) float tmp[8];
            _mm256_store_ps(tmp, v);
            for (int k = 0; k < 8; ++k)
                cum += (double)tmp[k];
        }
        for (; j < w; ++j)
            cum += (double)row[j];
    }

    cum /= (double)w * (double)h;
    *score = cum;
    return 0;
}

int compute_2nd_moment_avx2(const float *pic, int w, int h, int stride, double *score)
{
    assert(pic != NULL);
    assert(score != NULL);
    assert(w > 0);
    assert(h > 0);

    const int stride_f = stride / (int)sizeof(float);
    double cum = 0.0;

    for (int i = 0; i < h; ++i) {
        const float *row = pic + (size_t)i * (size_t)stride_f;
        int j = 0;

        for (; j + 8 <= w; j += 8) {
            const __m256 v = _mm256_loadu_ps(row + j);
            const __m256 vsq = _mm256_mul_ps(v, v);
            _Alignas(32) float tmp[8];
            _mm256_store_ps(tmp, vsq);
            for (int k = 0; k < 8; ++k)
                cum += (double)tmp[k];
        }
        for (; j < w; ++j) {
            const float p = row[j];
            cum += (double)p * (double)p;
        }
    }

    cum /= (double)w * (double)h;
    *score = cum;
    return 0;
}
