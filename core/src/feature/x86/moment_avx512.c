/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  AVX-512 implementations of compute_1st_moment / compute_2nd_moment for
 *  the float_moment feature extractor.  Widens the AVX2 8-lane path to
 *  16-lane ZMM; see ADR-0987.
 *
 *  Bit-exactness contract (mirrors moment_avx2.c / ADR-0179):
 *  The scalar reference accumulates `double cum` in row-major order.
 *  This path loads 16 floats into a ZMM register, stores them back to
 *  a 64-byte aligned temporary, then adds each lane sequentially into
 *  the `double` accumulator — same lane-by-lane widening strategy as
 *  the AVX2 path and `float_psnr_avx512.c`.  The per-lane cross-lane
 *  residual is bounded well inside the snapshot gate's `places=4`
 *  tolerance.
 */

#include <assert.h>
#include <immintrin.h>
#include <stddef.h>

#include "moment_avx512.h"

int compute_1st_moment_avx512(const float *pic, int w, int h, int stride, double *score)
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

        for (; j + 16 <= w; j += 16) {
            const __m512 v = _mm512_loadu_ps(row + j);
            _Alignas(64) float tmp[16];
            _mm512_store_ps(tmp, v);
            for (int k = 0; k < 16; ++k)
                cum += (double)tmp[k];
        }
        /* Fall through to AVX2-width tail. */
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

int compute_2nd_moment_avx512(const float *pic, int w, int h, int stride, double *score)
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

        for (; j + 16 <= w; j += 16) {
            const __m512 v = _mm512_loadu_ps(row + j);
            const __m512 vsq = _mm512_mul_ps(v, v);
            _Alignas(64) float tmp[16];
            _mm512_store_ps(tmp, vsq);
            for (int k = 0; k < 16; ++k)
                cum += (double)tmp[k];
        }
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
