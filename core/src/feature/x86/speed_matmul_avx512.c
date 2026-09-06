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
 * AVX-512 twin of speed_matmul_scalar() (core/src/feature/speed.c).
 *
 * See speed_matmul_avx2.c for the bit-exactness argument; the same
 * `-ffp-contract=off` carve-out applies here (x86_speed_matmul_avx512
 * static lib in core/src/meson.build).  The masked tail lets SpEED's
 * native 25-column QR matrices finish in two vector steps (16 + 9 lanes)
 * instead of the six SSE steps plus scalar remainder the baseline C
 * loop compiles to.
 */

#include <assert.h>
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

#include "speed_matmul_avx512.h"

void speed_matmul_avx512(float *dst, int dst_stride, const float *x, int x_stride, const float *y,
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
            __m512 a0 = _mm512_setzero_ps();
            __m512 a1 = _mm512_setzero_ps();
            for (int k = 0; k < inner; k++) {
                const __m512 xv = _mm512_set1_ps(xrow[k]);
                const float *yr = y + (size_t)k * (size_t)y_stride + (size_t)j;
                a0 = _mm512_add_ps(a0, _mm512_mul_ps(xv, _mm512_loadu_ps(yr)));
                a1 = _mm512_add_ps(a1, _mm512_mul_ps(xv, _mm512_loadu_ps(yr + 16)));
            }
            _mm512_storeu_ps(drow + j, a0);
            _mm512_storeu_ps(drow + j + 16, a1);
        }

        for (; j < cols; j += 16) {
            const int remaining = cols - j;
            const __mmask16 m = (remaining >= 16) ?
                                    (__mmask16)0xFFFFU :
                                    (__mmask16)((UINT32_C(1) << (unsigned)remaining) - UINT32_C(1));
            __m512 a0 = _mm512_setzero_ps();
            for (int k = 0; k < inner; k++) {
                const __m512 xv = _mm512_set1_ps(xrow[k]);
                const float *yr = y + (size_t)k * (size_t)y_stride + (size_t)j;
                a0 = _mm512_add_ps(a0, _mm512_mul_ps(xv, _mm512_maskz_loadu_ps(m, yr)));
            }
            _mm512_mask_storeu_ps(drow + j, m, a0);
        }
    }
}
