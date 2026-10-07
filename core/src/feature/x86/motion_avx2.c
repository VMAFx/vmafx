/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "feature/integer_motion.h"
#include "feature/x86/motion_avx2.h"

static inline int mirror(int idx, int size)
{
    if (idx < 0)
        return -idx;
    if (idx >= size)
        return 2 * size - idx - 2;
    return idx;
}

// Emulate arithmetic right shift of int64 by 16 in AVX2.
// AVX2 lacks srai_epi64; this uses the blend trick:
//   low dwords come from logical shift, high dwords from arithmetic shift.
static inline __m256i srai_epi64_16(__m256i v)
{
    __m256i lo = _mm256_srli_epi64(v, 16);
    __m256i hi = _mm256_srai_epi32(v, 16);
    return _mm256_blend_epi32(lo, hi, 0xAA);
}

// Low dword of each of the four int64 lanes of `lo` and of `hi`, as 8 x int32.
static inline __m256i pack_epi64_lo32(__m256i lo, __m256i hi)
{
    const __m256i perm_idx = _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0);
    __m128i res_lo = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(lo, perm_idx));
    __m128i res_hi = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(hi, perm_idx));
    return _mm256_inserti128_si256(_mm256_castsi128_si256(res_lo), res_hi, 1);
}

// acc_lo / acc_hi += v, its low / high four int32 widened to int64.
static inline void add_widen_epi32(__m256i v, __m256i *acc_lo, __m256i *acc_hi)
{
    *acc_lo = _mm256_add_epi64(*acc_lo, _mm256_cvtepi32_epi64(_mm256_castsi256_si128(v)));
    *acc_hi = _mm256_add_epi64(*acc_hi, _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v, 1)));
}

// x_conv + abs of one y_row column the vector loop does not reach (mirror boundary).
static inline uint32_t x_conv_abs_scalar(const int32_t *y_row, unsigned j, unsigned w)
{
    int64_t accum = 0;
    for (int k = 0; k < 5; k++) {
        int col = mirror((int)j - 2 + k, (int)w);
        accum += (int64_t)filter[k] * y_row[col];
    }
    int32_t val = (int32_t)((accum + (1 << 15)) >> 16);
    return (uint32_t)abs(val);
}

// x_conv + abs of the 8 columns starting at `y`; reads y[-2] .. y[9].
static inline __m256i x_conv_abs8_avx2(const int32_t *y)
{
    const __m256i g0 = _mm256_set1_epi32(3571);
    const __m256i g1 = _mm256_set1_epi32(16004);
    const __m256i g2 = _mm256_set1_epi32(26386);
    const __m256i round64 = _mm256_set1_epi64x(1 << 15);

    __m256i y0 = _mm256_loadu_si256((const __m256i *)(y - 2));
    __m256i y1 = _mm256_loadu_si256((const __m256i *)(y - 1));
    __m256i y2 = _mm256_loadu_si256((const __m256i *)y);
    __m256i y3 = _mm256_loadu_si256((const __m256i *)(y + 1));
    __m256i y4 = _mm256_loadu_si256((const __m256i *)(y + 2));

    // Each product fits in int32, and so do the two symmetric pairs.
    __m256i s04 = _mm256_add_epi32(_mm256_mullo_epi32(y0, g0), _mm256_mullo_epi32(y4, g0));
    __m256i s13 = _mm256_add_epi32(_mm256_mullo_epi32(y1, g1), _mm256_mullo_epi32(y3, g1));
    __m256i p2 = _mm256_mullo_epi32(y2, g2);

    // Widen to int64 and accumulate.
    __m256i acc_lo = _mm256_setzero_si256();
    __m256i acc_hi = _mm256_setzero_si256();
    add_widen_epi32(s04, &acc_lo, &acc_hi);
    add_widen_epi32(s13, &acc_lo, &acc_hi);
    add_widen_epi32(p2, &acc_lo, &acc_hi);

    // Round and arithmetic right shift >>16.
    acc_lo = srai_epi64_16(_mm256_add_epi64(acc_lo, round64));
    acc_hi = srai_epi64_16(_mm256_add_epi64(acc_hi, round64));

    return _mm256_abs_epi32(pack_epi64_lo32(acc_lo, acc_hi));
}

// Horizontal sum of 8 x int32.
static inline uint32_t hsum_epi32_avx2(__m256i v)
{
    __m128i lo128 = _mm256_castsi256_si128(v);
    __m128i hi128 = _mm256_extracti128_si256(v, 1);
    __m128i sum128 = _mm_add_epi32(lo128, hi128);
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_SHUFFLE(1, 0, 3, 2)));
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_SHUFFLE(0, 1, 0, 1)));
    return (uint32_t)_mm_cvtsi128_si32(sum128);
}

// SIMD phase 2: x_conv + abs + SAD for one row of int32 y_row.
// Processes 8 int32 columns at a time via mullo_epi32 + int64 accumulation.
static inline uint32_t x_conv_row_sad_avx2(const int32_t *y_row, unsigned w)
{
    uint32_t row_sad = 0;

    // Scalar left edge (columns 0, 1) — mirror boundary
    unsigned j;
    for (j = 0; j < 2 && j < w; j++)
        row_sad += x_conv_abs_scalar(y_row, j, w);

    // SIMD middle: need y_row[j-2]..y_row[j+9], so j+10 <= w
    __m256i sad_acc = _mm256_setzero_si256();
    for (; j + 10 <= w; j += 8)
        sad_acc = _mm256_add_epi32(sad_acc, x_conv_abs8_avx2(y_row + j));
    row_sad += hsum_epi32_avx2(sad_acc);

    // Scalar right edge + tail
    for (; j < w; j++)
        row_sad += x_conv_abs_scalar(y_row, j, w);

    return row_sad;
}

// prev - cur of 8 uint16 samples, as 8 x int32.
static inline __m256i diff8_u16_avx2(const uint16_t *prev, const uint16_t *cur)
{
    return _mm256_sub_epi32(_mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)prev)),
                            _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)cur)));
}

// y_conv of the frame difference for the 8 columns starting at j (16-bit samples).
static inline __m256i y_conv8_16_avx2(const uint16_t *const pp[5], const uint16_t *const cp[5],
                                      unsigned j, unsigned bpc)
{
    const __m256i g0 = _mm256_set1_epi32(3571);
    const __m256i g1 = _mm256_set1_epi32(16004);
    const __m256i g2 = _mm256_set1_epi32(26386);
    const __m256i round64 = _mm256_set1_epi64x(((int64_t)1 << (bpc - 1)));
    const __m256i bpc_vec = _mm256_set1_epi64x(bpc);

    __m256i acc_lo = _mm256_setzero_si256();
    __m256i acc_hi = _mm256_setzero_si256();
    add_widen_epi32(_mm256_mullo_epi32(diff8_u16_avx2(pp[0] + j, cp[0] + j), g0), &acc_lo, &acc_hi);
    add_widen_epi32(_mm256_mullo_epi32(diff8_u16_avx2(pp[1] + j, cp[1] + j), g1), &acc_lo, &acc_hi);
    add_widen_epi32(_mm256_mullo_epi32(diff8_u16_avx2(pp[2] + j, cp[2] + j), g2), &acc_lo, &acc_hi);
    add_widen_epi32(_mm256_mullo_epi32(diff8_u16_avx2(pp[3] + j, cp[3] + j), g1), &acc_lo, &acc_hi);
    add_widen_epi32(_mm256_mullo_epi32(diff8_u16_avx2(pp[4] + j, cp[4] + j), g0), &acc_lo, &acc_hi);

    acc_lo = _mm256_srlv_epi64(_mm256_add_epi64(acc_lo, round64), bpc_vec);
    acc_hi = _mm256_srlv_epi64(_mm256_add_epi64(acc_hi, round64), bpc_vec);

    return pack_epi64_lo32(acc_lo, acc_hi);
}

// Phase 1 for one row of 16-bit samples: diff + y_conv -> y_row (8 pixels at a
// time, int64 accum). Returns nonzero when any y_row value is nonzero.
static inline int y_conv_row_16_avx2(const uint16_t *const pp[5], const uint16_t *const cp[5],
                                     int32_t *y_row, unsigned w, unsigned bpc)
{
    unsigned j;
    __m256i nz_acc = _mm256_setzero_si256();
    for (j = 0; j + 8 <= w; j += 8) {
        __m256i result = y_conv8_16_avx2(pp, cp, j, bpc);
        _mm256_storeu_si256((__m256i *)(y_row + j), result);
        nz_acc = _mm256_or_si256(nz_acc, result);
    }

    // Scalar tail
    int32_t nz_tail = 0;
    for (; j < w; j++) {
        int64_t accum = 0;
        for (int k = 0; k < 5; k++) {
            int32_t diff = pp[k][j] - cp[k][j];
            accum += (int64_t)filter[k] * diff;
        }
        y_row[j] = (int32_t)((accum + ((int64_t)1 << (bpc - 1))) >> bpc);
        nz_tail |= y_row[j];
    }

    return !_mm256_testz_si256(nz_acc, nz_acc) || nz_tail;
}

uint64_t motion_score_pipeline_16_avx2(const uint8_t *prev_u8, ptrdiff_t prev_stride,
                                       const uint8_t *cur_u8, ptrdiff_t cur_stride, int32_t *y_row,
                                       unsigned w, unsigned h, unsigned bpc)
{
    const uint16_t *prev = (const uint16_t *)prev_u8;
    const uint16_t *cur = (const uint16_t *)cur_u8;
    const ptrdiff_t p_stride = prev_stride / 2;
    const ptrdiff_t c_stride = cur_stride / 2;

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        const uint16_t *pp[5];
        const uint16_t *cp[5];
        for (int k = 0; k < 5; k++) {
            int r = mirror((int)i - 2 + k, (int)h);
            pp[k] = prev + r * p_stride;
            cp[k] = cur + r * c_stride;
        }

        // Phase 1: diff + y_conv -> y_row; an all-zero row adds nothing.
        if (!y_conv_row_16_avx2(pp, cp, y_row, w, bpc))
            continue;

        // Phase 2: SIMD x_conv + abs + accumulate
        sad += x_conv_row_sad_avx2(y_row, w);
    }

    return sad;
}

// prev - cur of 16 uint8 samples, as 16 x int16.
static inline __m256i diff16_u8_avx2(const uint8_t *prev, const uint8_t *cur)
{
    return _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)prev)),
                            _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)cur)));
}

// acc_lo / acc_hi += d * f, each int16 product widened to int32 (unpacklo /
// unpackhi order: the low and the high half of each 128-bit lane).
static inline void add_mul_widen_epi16(__m256i d, __m256i f, __m256i *acc_lo, __m256i *acc_hi)
{
    __m256i lo = _mm256_mullo_epi16(d, f);
    __m256i hi = _mm256_mulhi_epi16(d, f);
    *acc_lo = _mm256_add_epi32(*acc_lo, _mm256_unpacklo_epi16(lo, hi));
    *acc_hi = _mm256_add_epi32(*acc_hi, _mm256_unpackhi_epi16(lo, hi));
}

// y_conv of the frame difference for the 16 columns starting at j (8-bit
// samples, shift >>8), stored to y_row[j .. j+15]. Returns the OR of the 16
// values folded into 8 lanes.
static inline __m256i y_conv16_8_avx2(const uint8_t *const p[5], const uint8_t *const c[5],
                                      unsigned j, int32_t *y_row)
{
    const __m256i f0 = _mm256_set1_epi16(3571);
    const __m256i f1 = _mm256_set1_epi16(16004);
    const __m256i f2 = _mm256_set1_epi16(26386);
    const __m256i round8 = _mm256_set1_epi32(1 << 7);

    __m256i acc_lo = _mm256_setzero_si256();
    __m256i acc_hi = _mm256_setzero_si256();
    add_mul_widen_epi16(diff16_u8_avx2(p[0] + j, c[0] + j), f0, &acc_lo, &acc_hi);
    add_mul_widen_epi16(diff16_u8_avx2(p[1] + j, c[1] + j), f1, &acc_lo, &acc_hi);
    add_mul_widen_epi16(diff16_u8_avx2(p[2] + j, c[2] + j), f2, &acc_lo, &acc_hi);
    add_mul_widen_epi16(diff16_u8_avx2(p[3] + j, c[3] + j), f1, &acc_lo, &acc_hi);
    add_mul_widen_epi16(diff16_u8_avx2(p[4] + j, c[4] + j), f0, &acc_lo, &acc_hi);

    acc_lo = _mm256_srai_epi32(_mm256_add_epi32(acc_lo, round8), 8);
    acc_hi = _mm256_srai_epi32(_mm256_add_epi32(acc_hi, round8), 8);

    __m256i cols_0_7 = _mm256_permute2x128_si256(acc_lo, acc_hi, 0x20);
    __m256i cols_8_15 = _mm256_permute2x128_si256(acc_lo, acc_hi, 0x31);
    _mm256_storeu_si256((__m256i *)(y_row + j), cols_0_7);
    _mm256_storeu_si256((__m256i *)(y_row + j + 8), cols_8_15);
    return _mm256_or_si256(cols_0_7, cols_8_15);
}

// Phase 1 for one row of 8-bit samples: diff + y_conv -> y_row (16 columns at a
// time). Returns nonzero when any y_row value is nonzero.
static inline int y_conv_row_8_avx2(const uint8_t *const p[5], const uint8_t *const c[5],
                                    int32_t *y_row, unsigned w)
{
    unsigned j;
    __m256i nz_acc = _mm256_setzero_si256();
    for (j = 0; j + 16 <= w; j += 16)
        nz_acc = _mm256_or_si256(nz_acc, y_conv16_8_avx2(p, c, j, y_row));

    // Scalar tail
    int32_t nz_tail = 0;
    for (; j < w; j++) {
        int32_t accum = 0;
        for (int k = 0; k < 5; k++) {
            int32_t diff = p[k][j] - c[k][j];
            accum += (int32_t)filter[k] * diff;
        }
        y_row[j] = (accum + (1 << 7)) >> 8;
        nz_tail |= y_row[j];
    }

    return !_mm256_testz_si256(nz_acc, nz_acc) || nz_tail;
}

uint64_t motion_score_pipeline_8_avx2(const uint8_t *prev, ptrdiff_t prev_stride,
                                      const uint8_t *cur, ptrdiff_t cur_stride, int32_t *y_row,
                                      unsigned w, unsigned h, unsigned bpc)
{
    (void)bpc;
    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        const uint8_t *p[5];
        const uint8_t *c[5];
        for (int k = 0; k < 5; k++) {
            int r = mirror((int)i - 2 + k, (int)h);
            p[k] = prev + r * prev_stride;
            c[k] = cur + r * cur_stride;
        }

        // Phase 1: diff + y_conv -> y_row; an all-zero row adds nothing.
        if (!y_conv_row_8_avx2(p, c, y_row, w))
            continue;

        // Phase 2: SIMD x_conv + abs + accumulate
        sad += x_conv_row_sad_avx2(y_row, w);
    }

    return sad;
}
