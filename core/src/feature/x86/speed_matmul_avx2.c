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
 * AVX2 twin of speed_matmul_scalar() (core/src/feature/speed.c).
 *
 * Bit-exactness: the `j` axis is an output index, so widening it to 8
 * lanes changes nothing about the order in which any single dst element
 * accumulates over `k`.  The multiply and the add are kept as separate
 * _mm256_mul_ps / _mm256_add_ps operations and the translation unit is
 * compiled with `-ffp-contract=off` (x86_speed_matmul_avx2 static lib in
 * core/src/meson.build) so the compiler cannot fuse them into a VFMADD,
 * which would round once instead of twice and diverge from the scalar
 * reference.  test_speed_simd asserts memcmp-equality against that
 * reference.
 */

#include <assert.h>
#include <immintrin.h>
#include <stddef.h>

#include "speed_matmul_avx2.h"

void speed_matmul_avx2(float *dst, int dst_stride, const float *x, int x_stride, const float *y,
                       int y_stride, int rows, int inner, int cols)
{
    assert(dst != NULL);
    assert(x != NULL);
    assert(y != NULL);
    assert(rows >= 0);
    assert(inner >= 0);
    assert(cols >= 0);
    assert(dst_stride >= cols);
    assert(x_stride >= inner);
    assert(y_stride >= cols);

    for (int i = 0; i < rows; i++) {
        float *drow = dst + (size_t)i * (size_t)dst_stride;
        const float *xrow = x + (size_t)i * (size_t)x_stride;
        int j = 0;

        for (; j + 31 < cols; j += 32) {
            __m256 a0 = _mm256_setzero_ps();
            __m256 a1 = _mm256_setzero_ps();
            __m256 a2 = _mm256_setzero_ps();
            __m256 a3 = _mm256_setzero_ps();
            for (int k = 0; k < inner; k++) {
                const __m256 xv = _mm256_set1_ps(xrow[k]);
                const float *yr = y + (size_t)k * (size_t)y_stride + (size_t)j;
                a0 = _mm256_add_ps(a0, _mm256_mul_ps(xv, _mm256_loadu_ps(yr)));
                a1 = _mm256_add_ps(a1, _mm256_mul_ps(xv, _mm256_loadu_ps(yr + 8)));
                a2 = _mm256_add_ps(a2, _mm256_mul_ps(xv, _mm256_loadu_ps(yr + 16)));
                a3 = _mm256_add_ps(a3, _mm256_mul_ps(xv, _mm256_loadu_ps(yr + 24)));
            }
            _mm256_storeu_ps(drow + j, a0);
            _mm256_storeu_ps(drow + j + 8, a1);
            _mm256_storeu_ps(drow + j + 16, a2);
            _mm256_storeu_ps(drow + j + 24, a3);
        }

        for (; j + 7 < cols; j += 8) {
            __m256 a0 = _mm256_setzero_ps();
            for (int k = 0; k < inner; k++) {
                const __m256 xv = _mm256_set1_ps(xrow[k]);
                const float *yr = y + (size_t)k * (size_t)y_stride + (size_t)j;
                a0 = _mm256_add_ps(a0, _mm256_mul_ps(xv, _mm256_loadu_ps(yr)));
            }
            _mm256_storeu_ps(drow + j, a0);
        }

        for (; j < cols; j++) {
            float acc = 0.0f;
            for (int k = 0; k < inner; k++)
                acc += xrow[k] * y[(size_t)k * (size_t)y_stride + (size_t)j];
            drow[j] = acc;
        }
    }
}
