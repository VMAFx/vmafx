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

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "feature/integer_motion.h"
#include "libvmaf/picture.h"
#include "feature/common/alignment.h"

static inline int mirror(int idx, int size)
{
    if (idx < 0)
        return -idx;
    if (idx >= size)
        return 2 * size - idx - 2;
    return idx;
}

// SIMD phase 2: x_conv + abs + SAD for one row of int32 y_row.
// Processes 16 int32 columns at a time via mullo_epi32 + int64 accumulation.
static inline uint32_t x_conv_row_sad_avx512(const int32_t *y_row, unsigned w)
{
    const __m512i g0 = _mm512_set1_epi32(3571);
    const __m512i g1 = _mm512_set1_epi32(16004);
    const __m512i g2 = _mm512_set1_epi32(26386);
    const __m512i round64 = _mm512_set1_epi64(1 << 15);

    uint32_t row_sad = 0;

    // Scalar left edge (columns 0, 1) — mirror boundary
    unsigned j;
    for (j = 0; j < 2 && j < w; j++) {
        int64_t accum = 0;
        for (int k = 0; k < 5; k++) {
            int col = mirror((int)j - 2 + k, (int)w);
            accum += (int64_t)filter[k] * y_row[col];
        }
        int32_t val = (int32_t)((accum + (1 << 15)) >> 16);
        row_sad += abs(val);
    }

    // SIMD middle: need y_row[j-2]..y_row[j+17], so j+18 <= w
    __m512i sad_acc = _mm512_setzero_si512();
    for (; j + 18 <= w; j += 16) {
        __m512i y0 = _mm512_loadu_si512((__m512i *)(y_row + j - 2));
        __m512i y1 = _mm512_loadu_si512((__m512i *)(y_row + j - 1));
        __m512i y2 = _mm512_loadu_si512((__m512i *)(y_row + j));
        __m512i y3 = _mm512_loadu_si512((__m512i *)(y_row + j + 1));
        __m512i y4 = _mm512_loadu_si512((__m512i *)(y_row + j + 2));

        // Each product fits in int32
        __m512i p0 = _mm512_mullo_epi32(y0, g0);
        __m512i p1 = _mm512_mullo_epi32(y1, g1);
        __m512i p2 = _mm512_mullo_epi32(y2, g2);
        __m512i p3 = _mm512_mullo_epi32(y3, g1);
        __m512i p4 = _mm512_mullo_epi32(y4, g0);

        // Safe pairs that fit in int32
        __m512i s04 = _mm512_add_epi32(p0, p4);
        __m512i s13 = _mm512_add_epi32(p1, p3);

        // Widen to int64 and accumulate (lo 8 elements)
        __m512i acc_lo = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(s04));
        acc_lo = _mm512_add_epi64(acc_lo, _mm512_cvtepi32_epi64(_mm512_castsi512_si256(s13)));
        acc_lo = _mm512_add_epi64(acc_lo, _mm512_cvtepi32_epi64(_mm512_castsi512_si256(p2)));

        // hi 8 elements
        __m512i acc_hi = _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(s04, 1));
        acc_hi = _mm512_add_epi64(acc_hi, _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(s13, 1)));
        acc_hi = _mm512_add_epi64(acc_hi, _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(p2, 1)));

        // Round and arithmetic right shift >>16 (native in AVX-512)
        acc_lo = _mm512_srai_epi64(_mm512_add_epi64(acc_lo, round64), 16);
        acc_hi = _mm512_srai_epi64(_mm512_add_epi64(acc_hi, round64), 16);

        // Narrow int64 -> int32 (signed saturation)
        __m256i res_lo = _mm512_cvtsepi64_epi32(acc_lo);
        __m256i res_hi = _mm512_cvtsepi64_epi32(acc_hi);

        // Combine into 16 x int32, abs, accumulate
        __m512i result = _mm512_inserti64x4(_mm512_castsi256_si512(res_lo), res_hi, 1);
        __m512i abs_result = _mm512_abs_epi32(result);

        sad_acc = _mm512_add_epi32(sad_acc, abs_result);
    }

    row_sad += (uint32_t)_mm512_reduce_add_epi32(sad_acc);

    // Scalar right edge + tail
    for (; j < w; j++) {
        int64_t accum = 0;
        for (int k = 0; k < 5; k++) {
            int col = mirror((int)j - 2 + k, (int)w);
            accum += (int64_t)filter[k] * y_row[col];
        }
        int32_t val = (int32_t)((accum + (1 << 15)) >> 16);
        row_sad += abs(val);
    }

    return row_sad;
}

uint64_t motion_score_pipeline_16_avx512(const uint8_t *prev_u8, ptrdiff_t prev_stride,
                                         const uint8_t *cur_u8, ptrdiff_t cur_stride,
                                         int32_t *y_row, unsigned w, unsigned h, unsigned bpc)
{
    const uint16_t *prev = (const uint16_t *)prev_u8;
    const uint16_t *cur = (const uint16_t *)cur_u8;
    const ptrdiff_t p_stride = prev_stride / 2;
    const ptrdiff_t c_stride = cur_stride / 2;

    const __m512i g0 = _mm512_set1_epi32(3571);
    const __m512i g1 = _mm512_set1_epi32(16004);
    const __m512i g2 = _mm512_set1_epi32(26386);
    const __m512i round64 = _mm512_set1_epi64(1 << (bpc - 1));
    const __m512i bpc_vec = _mm512_set1_epi64(bpc);

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        const uint16_t *pp[5], *cp[5];
        for (int k = 0; k < 5; k++) {
            int r = mirror((int)i - 2 + k, (int)h);
            pp[k] = prev + r * p_stride;
            cp[k] = cur + r * c_stride;
        }

        unsigned j;
        __m512i nz_acc = _mm512_setzero_si512();
        for (j = 0; j + 16 <= w; j += 16) {
            __m512i d0 =
                _mm512_sub_epi32(_mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(pp[0] + j))),
                                 _mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(cp[0] + j))));
            __m512i d1 =
                _mm512_sub_epi32(_mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(pp[1] + j))),
                                 _mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(cp[1] + j))));
            __m512i d2 =
                _mm512_sub_epi32(_mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(pp[2] + j))),
                                 _mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(cp[2] + j))));
            __m512i d3 =
                _mm512_sub_epi32(_mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(pp[3] + j))),
                                 _mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(cp[3] + j))));
            __m512i d4 =
                _mm512_sub_epi32(_mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(pp[4] + j))),
                                 _mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i *)(cp[4] + j))));

            __m512i prod0 = _mm512_mullo_epi32(d0, g0);
            __m512i prod1 = _mm512_mullo_epi32(d1, g1);
            __m512i prod2 = _mm512_mullo_epi32(d2, g2);
            __m512i prod3 = _mm512_mullo_epi32(d3, g1);
            __m512i prod4 = _mm512_mullo_epi32(d4, g0);

            __m512i acc_lo = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(prod0));
            acc_lo = _mm512_add_epi64(acc_lo, _mm512_cvtepi32_epi64(_mm512_castsi512_si256(prod1)));
            acc_lo = _mm512_add_epi64(acc_lo, _mm512_cvtepi32_epi64(_mm512_castsi512_si256(prod2)));
            acc_lo = _mm512_add_epi64(acc_lo, _mm512_cvtepi32_epi64(_mm512_castsi512_si256(prod3)));
            acc_lo = _mm512_add_epi64(acc_lo, _mm512_cvtepi32_epi64(_mm512_castsi512_si256(prod4)));

            __m512i acc_hi = _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(prod0, 1));
            acc_hi = _mm512_add_epi64(acc_hi,
                                      _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(prod1, 1)));
            acc_hi = _mm512_add_epi64(acc_hi,
                                      _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(prod2, 1)));
            acc_hi = _mm512_add_epi64(acc_hi,
                                      _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(prod3, 1)));
            acc_hi = _mm512_add_epi64(acc_hi,
                                      _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(prod4, 1)));

            acc_lo = _mm512_srav_epi64(_mm512_add_epi64(acc_lo, round64), bpc_vec);
            acc_hi = _mm512_srav_epi64(_mm512_add_epi64(acc_hi, round64), bpc_vec);

            __m256i res_lo = _mm512_cvtsepi64_epi32(acc_lo);
            __m256i res_hi = _mm512_cvtsepi64_epi32(acc_hi);

            __m512i result = _mm512_inserti64x4(_mm512_castsi256_si512(res_lo), res_hi, 1);
            _mm512_storeu_si512((__m512i *)(y_row + j), result);
            nz_acc = _mm512_or_si512(nz_acc, result);
        }

        int32_t nz_tail = 0;
        for (; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < 5; k++) {
                int32_t diff = pp[k][j] - cp[k][j];
                accum += (int64_t)filter[k] * diff;
            }
            y_row[j] = (int32_t)((accum + (1 << (bpc - 1))) >> bpc);
            nz_tail |= y_row[j];
        }

        if (_mm512_test_epi32_mask(nz_acc, nz_acc) == 0 && !nz_tail)
            continue;

        sad += x_conv_row_sad_avx512(y_row, w);
    }

    return sad;
}

uint64_t motion_score_pipeline_8_avx512(const uint8_t *prev, ptrdiff_t prev_stride,
                                        const uint8_t *cur, ptrdiff_t cur_stride, int32_t *y_row,
                                        unsigned w, unsigned h, unsigned bpc)
{
    (void)bpc;
    const __m512i f0 = _mm512_set1_epi16(3571);
    const __m512i f1 = _mm512_set1_epi16(16004);
    const __m512i f2 = _mm512_set1_epi16(26386);
    const __m512i round8 = _mm512_set1_epi32(1 << 7);

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        const uint8_t *p[5], *c[5];
        for (int k = 0; k < 5; k++) {
            int r = mirror((int)i - 2 + k, (int)h);
            p[k] = prev + r * prev_stride;
            c[k] = cur + r * cur_stride;
        }

        unsigned j;
        __m512i nz_acc = _mm512_setzero_si512();
        for (j = 0; j + 32 <= w; j += 32) {
            __m512i d0 =
                _mm512_sub_epi16(_mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(p[0] + j))),
                                 _mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(c[0] + j))));
            __m512i d1 =
                _mm512_sub_epi16(_mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(p[1] + j))),
                                 _mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(c[1] + j))));
            __m512i d2 =
                _mm512_sub_epi16(_mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(p[2] + j))),
                                 _mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(c[2] + j))));
            __m512i d3 =
                _mm512_sub_epi16(_mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(p[3] + j))),
                                 _mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(c[3] + j))));
            __m512i d4 =
                _mm512_sub_epi16(_mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(p[4] + j))),
                                 _mm512_cvtepu8_epi16(_mm256_loadu_si256((__m256i *)(c[4] + j))));

            __m512i lo = _mm512_mullo_epi16(d0, f0);
            __m512i hi = _mm512_mulhi_epi16(d0, f0);
            __m512i acc_lo = _mm512_unpacklo_epi16(lo, hi);
            __m512i acc_hi = _mm512_unpackhi_epi16(lo, hi);

            lo = _mm512_mullo_epi16(d1, f1);
            hi = _mm512_mulhi_epi16(d1, f1);
            acc_lo = _mm512_add_epi32(acc_lo, _mm512_unpacklo_epi16(lo, hi));
            acc_hi = _mm512_add_epi32(acc_hi, _mm512_unpackhi_epi16(lo, hi));

            lo = _mm512_mullo_epi16(d2, f2);
            hi = _mm512_mulhi_epi16(d2, f2);
            acc_lo = _mm512_add_epi32(acc_lo, _mm512_unpacklo_epi16(lo, hi));
            acc_hi = _mm512_add_epi32(acc_hi, _mm512_unpackhi_epi16(lo, hi));

            lo = _mm512_mullo_epi16(d3, f1);
            hi = _mm512_mulhi_epi16(d3, f1);
            acc_lo = _mm512_add_epi32(acc_lo, _mm512_unpacklo_epi16(lo, hi));
            acc_hi = _mm512_add_epi32(acc_hi, _mm512_unpackhi_epi16(lo, hi));

            lo = _mm512_mullo_epi16(d4, f0);
            hi = _mm512_mulhi_epi16(d4, f0);
            acc_lo = _mm512_add_epi32(acc_lo, _mm512_unpacklo_epi16(lo, hi));
            acc_hi = _mm512_add_epi32(acc_hi, _mm512_unpackhi_epi16(lo, hi));

            acc_lo = _mm512_srai_epi32(_mm512_add_epi32(acc_lo, round8), 8);
            acc_hi = _mm512_srai_epi32(_mm512_add_epi32(acc_hi, round8), 8);

            __m256i lo_lo = _mm512_castsi512_si256(acc_lo);
            __m256i lo_hi = _mm512_extracti64x4_epi64(acc_lo, 1);
            __m256i hi_lo = _mm512_castsi512_si256(acc_hi);
            __m256i hi_hi = _mm512_extracti64x4_epi64(acc_hi, 1);

            __m256i cols_0_7 = _mm256_permute2x128_si256(lo_lo, hi_lo, 0x20);
            __m256i cols_8_15 = _mm256_permute2x128_si256(lo_lo, hi_lo, 0x31);
            __m256i cols_16_23 = _mm256_permute2x128_si256(lo_hi, hi_hi, 0x20);
            __m256i cols_24_31 = _mm256_permute2x128_si256(lo_hi, hi_hi, 0x31);

            _mm256_storeu_si256((__m256i *)(y_row + j), cols_0_7);
            _mm256_storeu_si256((__m256i *)(y_row + j + 8), cols_8_15);
            _mm256_storeu_si256((__m256i *)(y_row + j + 16), cols_16_23);
            _mm256_storeu_si256((__m256i *)(y_row + j + 24), cols_24_31);

            __m512i stored =
                _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_or_si256(cols_0_7, cols_8_15)),
                                   _mm256_or_si256(cols_16_23, cols_24_31), 1);
            nz_acc = _mm512_or_si512(nz_acc, stored);
        }

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

        if (_mm512_test_epi32_mask(nz_acc, nz_acc) == 0 && !nz_tail)
            continue;

        sad += x_conv_row_sad_avx512(y_row, w);
    }

    return sad;
}

/* -----------------------------------------------------------------------
 * sad_avx512 — pixel-wise absolute difference sum between two pictures.
 *
 * Operates on the luma plane (data[0]) only.  Both pictures must have the
 * same dimensions and bit-depth.  Processes 32 uint16 samples per SIMD
 * iteration using _mm512_abs_epi16 + widening accumulation.
 * ----------------------------------------------------------------------- */
void sad_avx512(VmafPicture *pic_a, VmafPicture *pic_b, uint64_t *sad_out)
{
    const unsigned w = pic_a->w[0];
    const unsigned h = pic_a->h[0];
    const ptrdiff_t stride_a = pic_a->stride[0] / 2; /* stride in uint16 samples */
    const ptrdiff_t stride_b = pic_b->stride[0] / 2;
    const uint16_t *a = (const uint16_t *)pic_a->data[0];
    const uint16_t *b = (const uint16_t *)pic_b->data[0];

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        const uint16_t *row_a = a + i * stride_a;
        const uint16_t *row_b = b + i * stride_b;
        __m512i acc = _mm512_setzero_si512();
        unsigned j = 0;

        for (; j + 32 <= w; j += 32) {
            __m512i va = _mm512_loadu_si512((const __m512i *)(row_a + j));
            __m512i vb = _mm512_loadu_si512((const __m512i *)(row_b + j));
            /* Signed subtract, then abs -> |a[k]-b[k]| per int16 lane */
            __m512i diff = _mm512_sub_epi16(va, vb);
            __m512i abs_diff = _mm512_abs_epi16(diff);
            /* Widen uint16 -> uint32 in two halves and accumulate */
            acc = _mm512_add_epi32(acc, _mm512_cvtepu16_epi32(_mm512_castsi512_si256(abs_diff)));
            acc = _mm512_add_epi32(acc,
                                   _mm512_cvtepu16_epi32(_mm512_extracti64x4_epi64(abs_diff, 1)));
        }

        sad += (uint64_t)(uint32_t)_mm512_reduce_add_epi32(acc);

        /* Scalar tail */
        for (; j < w; j++) {
            sad += (uint64_t)(unsigned)abs((int)row_a[j] - (int)row_b[j]);
        }
    }

    *sad_out = sad;
}

/* -----------------------------------------------------------------------
 * y_convolution_8_avx512 — vertical 5-tap Gaussian on an 8-bit source.
 *
 * Signature mirrors integer_motion.c::y_convolution_8 (static).
 * inp_size_bits is unused (always 8); kept for API symmetry with the
 * 16-bit variant.
 * ----------------------------------------------------------------------- */
void y_convolution_8_avx512(const void *src_void, uint16_t *dst, unsigned width, unsigned height,
                            ptrdiff_t src_stride, ptrdiff_t dst_stride, unsigned inp_size_bits)
{
    (void)inp_size_bits;
    const uint8_t *src = (const uint8_t *)src_void;
    const unsigned radius = (unsigned)(filter_width / 2);
    const unsigned top_edge = vmaf_ceiln(radius, 1);
    const unsigned bottom_edge = vmaf_floorn(height - (unsigned)(filter_width - (int)radius), 1);
    const unsigned shift_var = 8u;
    const unsigned add_before_shift = 1u << (shift_var - 1u);

    const __m512i round_v = _mm512_set1_epi32((int32_t)add_before_shift);

    /* Top edge rows — scalar mirror boundary */
    for (unsigned i = 0; i < top_edge; i++) {
        for (unsigned j = 0; j < width; j++) {
            uint32_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                int i_tap = (int)i - (int)radius + k;
                if (i_tap < 0)
                    i_tap = -i_tap;
                else if (i_tap >= (int)height)
                    i_tap = (int)height - (i_tap - (int)height + 2);
                accum += filter[k] * src[(ptrdiff_t)i_tap * src_stride + j];
            }
            dst[i * dst_stride + j] = (uint16_t)((accum + add_before_shift) >> shift_var);
        }
    }

    /* Interior rows — AVX-512 vectorised, 32 uint8 pixels per iteration */
    for (unsigned i = top_edge; i < bottom_edge; i++) {
        const uint8_t *s0 = src + (ptrdiff_t)(i - 2) * src_stride;
        const uint8_t *s1 = src + (ptrdiff_t)(i - 1) * src_stride;
        const uint8_t *s2 = src + (ptrdiff_t)i * src_stride;
        const uint8_t *s3 = src + (ptrdiff_t)(i + 1) * src_stride;
        const uint8_t *s4 = src + (ptrdiff_t)(i + 2) * src_stride;

        unsigned j = 0;
        for (; j + 32 <= width; j += 32) {
            /* Zero-extend 32 uint8 -> 32 uint16 */
            __m512i v0 = _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(s0 + j)));
            __m512i v1 = _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(s1 + j)));
            __m512i v2 = _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(s2 + j)));
            __m512i v3 = _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(s3 + j)));
            __m512i v4 = _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(s4 + j)));

            /* Low 16 pixels (indices 0..15): widen uint16->uint32 and filter */
            __m512i acc_lo = _mm512_mullo_epi32(_mm512_cvtepu16_epi32(_mm512_castsi512_si256(v0)),
                                                _mm512_set1_epi32(3571));
            acc_lo = _mm512_add_epi32(
                acc_lo, _mm512_mullo_epi32(_mm512_cvtepu16_epi32(_mm512_castsi512_si256(v1)),
                                           _mm512_set1_epi32(16004)));
            acc_lo = _mm512_add_epi32(
                acc_lo, _mm512_mullo_epi32(_mm512_cvtepu16_epi32(_mm512_castsi512_si256(v2)),
                                           _mm512_set1_epi32(26386)));
            acc_lo = _mm512_add_epi32(
                acc_lo, _mm512_mullo_epi32(_mm512_cvtepu16_epi32(_mm512_castsi512_si256(v3)),
                                           _mm512_set1_epi32(16004)));
            acc_lo = _mm512_add_epi32(
                acc_lo, _mm512_mullo_epi32(_mm512_cvtepu16_epi32(_mm512_castsi512_si256(v4)),
                                           _mm512_set1_epi32(3571)));
            acc_lo = _mm512_srli_epi32(_mm512_add_epi32(acc_lo, round_v), shift_var);

            /* High 16 pixels (indices 16..31) */
            __m512i acc_hi = _mm512_mullo_epi32(
                _mm512_cvtepu16_epi32(_mm512_extracti64x4_epi64(v0, 1)), _mm512_set1_epi32(3571));
            acc_hi = _mm512_add_epi32(
                acc_hi, _mm512_mullo_epi32(_mm512_cvtepu16_epi32(_mm512_extracti64x4_epi64(v1, 1)),
                                           _mm512_set1_epi32(16004)));
            acc_hi = _mm512_add_epi32(
                acc_hi, _mm512_mullo_epi32(_mm512_cvtepu16_epi32(_mm512_extracti64x4_epi64(v2, 1)),
                                           _mm512_set1_epi32(26386)));
            acc_hi = _mm512_add_epi32(
                acc_hi, _mm512_mullo_epi32(_mm512_cvtepu16_epi32(_mm512_extracti64x4_epi64(v3, 1)),
                                           _mm512_set1_epi32(16004)));
            acc_hi = _mm512_add_epi32(
                acc_hi, _mm512_mullo_epi32(_mm512_cvtepu16_epi32(_mm512_extracti64x4_epi64(v4, 1)),
                                           _mm512_set1_epi32(3571)));
            acc_hi = _mm512_srli_epi32(_mm512_add_epi32(acc_hi, round_v), shift_var);

            /* Narrow int32 -> uint16 and store */
            _mm256_storeu_si256((__m256i *)(dst + i * dst_stride + j),
                                _mm512_cvtepi32_epi16(acc_lo));
            _mm256_storeu_si256((__m256i *)(dst + i * dst_stride + j + 16),
                                _mm512_cvtepi32_epi16(acc_hi));
        }

        /* Scalar tail */
        for (; j < width; j++) {
            uint32_t accum = (uint32_t)filter[0] * s0[j] + (uint32_t)filter[1] * s1[j] +
                             (uint32_t)filter[2] * s2[j] + (uint32_t)filter[3] * s3[j] +
                             (uint32_t)filter[4] * s4[j];
            dst[i * dst_stride + j] = (uint16_t)((accum + add_before_shift) >> shift_var);
        }
    }

    /* Bottom edge rows — scalar mirror boundary */
    for (unsigned i = bottom_edge; i < height; i++) {
        for (unsigned j = 0; j < width; j++) {
            uint32_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                int i_tap = (int)i - (int)radius + k;
                if (i_tap < 0)
                    i_tap = -i_tap;
                else if (i_tap >= (int)height)
                    i_tap = (int)height - (i_tap - (int)height + 2);
                accum += filter[k] * src[(ptrdiff_t)i_tap * src_stride + j];
            }
            dst[i * dst_stride + j] = (uint16_t)((accum + add_before_shift) >> shift_var);
        }
    }
}

/* -----------------------------------------------------------------------
 * y_convolution_16_avx512 — vertical 5-tap Gaussian on a 16-bit source.
 *
 * Signature mirrors integer_motion.c::y_convolution_16 (static).
 * inp_size_bits is the source bit depth (10, 12, or 16).
 * ----------------------------------------------------------------------- */
void y_convolution_16_avx512(const void *src_void, uint16_t *dst, unsigned width, unsigned height,
                             ptrdiff_t src_stride, ptrdiff_t dst_stride, unsigned inp_size_bits)
{
    const uint16_t *src = (const uint16_t *)src_void;
    const unsigned radius = (unsigned)(filter_width / 2);
    const unsigned top_edge = vmaf_ceiln(radius, 1);
    const unsigned bottom_edge = vmaf_floorn(height - (unsigned)(filter_width - (int)radius), 1);
    const unsigned add_before_shift = 1u << (inp_size_bits - 1u);
    const unsigned shift_var = inp_size_bits;

    const __m512i round_v = _mm512_set1_epi32((int32_t)add_before_shift);

    /* Top edge rows — scalar mirror boundary */
    for (unsigned i = 0; i < top_edge; i++) {
        for (unsigned j = 0; j < width; j++) {
            dst[i * dst_stride + j] = (uint16_t)((edge_16(false, src, (int)width, (int)height,
                                                          (int)src_stride, (int)i, (int)j) +
                                                  add_before_shift) >>
                                                 shift_var);
        }
    }

    /* Interior rows — AVX-512 vectorised, 16 uint16 pixels per iteration */
    for (unsigned i = top_edge; i < bottom_edge; i++) {
        const uint16_t *s0 = src + (ptrdiff_t)(i - 2) * src_stride;
        const uint16_t *s1 = src + (ptrdiff_t)(i - 1) * src_stride;
        const uint16_t *s2 = src + (ptrdiff_t)i * src_stride;
        const uint16_t *s3 = src + (ptrdiff_t)(i + 1) * src_stride;
        const uint16_t *s4 = src + (ptrdiff_t)(i + 2) * src_stride;

        unsigned j = 0;
        for (; j + 16 <= width; j += 16) {
            /* Zero-extend 16 uint16 -> 16 uint32 */
            __m512i v0 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(s0 + j)));
            __m512i v1 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(s1 + j)));
            __m512i v2 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(s2 + j)));
            __m512i v3 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(s3 + j)));
            __m512i v4 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(s4 + j)));

            __m512i acc = _mm512_mullo_epi32(v0, _mm512_set1_epi32(3571));
            acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v1, _mm512_set1_epi32(16004)));
            acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v2, _mm512_set1_epi32(26386)));
            acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v3, _mm512_set1_epi32(16004)));
            acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v4, _mm512_set1_epi32(3571)));
            acc = _mm512_srli_epi32(_mm512_add_epi32(acc, round_v), shift_var);

            /* Narrow int32 -> uint16 and store */
            _mm256_storeu_si256((__m256i *)(dst + i * dst_stride + j), _mm512_cvtepi32_epi16(acc));
        }

        /* Scalar tail */
        for (; j < width; j++) {
            uint32_t accum = (uint32_t)filter[0] * s0[j] + (uint32_t)filter[1] * s1[j] +
                             (uint32_t)filter[2] * s2[j] + (uint32_t)filter[3] * s3[j] +
                             (uint32_t)filter[4] * s4[j];
            dst[i * dst_stride + j] = (uint16_t)((accum + add_before_shift) >> shift_var);
        }
    }

    /* Bottom edge rows — scalar mirror boundary */
    for (unsigned i = bottom_edge; i < height; i++) {
        for (unsigned j = 0; j < width; j++) {
            dst[i * dst_stride + j] = (uint16_t)((edge_16(false, src, (int)width, (int)height,
                                                          (int)src_stride, (int)i, (int)j) +
                                                  add_before_shift) >>
                                                 shift_var);
        }
    }
}

/* -----------------------------------------------------------------------
 * x_convolution_16_avx512 — horizontal 5-tap Gaussian on a 16-bit source.
 *
 * Signature mirrors integer_motion.c::x_convolution_16 (static) and
 * x_convolution_16_neon in arm64/motion_neon.c.
 * ----------------------------------------------------------------------- */
void x_convolution_16_avx512(const uint16_t *src, uint16_t *dst, unsigned width, unsigned height,
                             ptrdiff_t src_stride, ptrdiff_t dst_stride)
{
    const unsigned radius = (unsigned)(filter_width / 2);
    const unsigned left_edge = vmaf_ceiln(radius, 1);
    const unsigned right_edge = vmaf_floorn(width - (unsigned)(filter_width - (int)radius), 1);
    const unsigned shift_add_round = 32768u;

    const __m512i round_v = _mm512_set1_epi32((int32_t)shift_add_round);

    /* Left edge columns — scalar mirror boundary */
    for (unsigned i = 0; i < height; i++) {
        for (unsigned j = 0; j < left_edge; j++) {
            dst[i * dst_stride + j] = (uint16_t)((edge_16(true, src, (int)width, (int)height,
                                                          (int)src_stride, (int)i, (int)j) +
                                                  shift_add_round) >>
                                                 16);
        }
    }

    /* Interior columns — AVX-512 vectorised, 16 uint16 per iteration */
    for (unsigned i = 0; i < height; i++) {
        /* src_row points to src[i][left_edge - radius] */
        const uint16_t *src_row = src + (ptrdiff_t)i * src_stride + (left_edge - radius);
        unsigned j = left_edge;

        for (; j + 16 <= right_edge; j += 16) {
            __m512i v0 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(src_row)));
            __m512i v1 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(src_row + 1)));
            __m512i v2 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(src_row + 2)));
            __m512i v3 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(src_row + 3)));
            __m512i v4 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(src_row + 4)));

            __m512i acc = _mm512_mullo_epi32(v0, _mm512_set1_epi32(3571));
            acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v1, _mm512_set1_epi32(16004)));
            acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v2, _mm512_set1_epi32(26386)));
            acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v3, _mm512_set1_epi32(16004)));
            acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v4, _mm512_set1_epi32(3571)));
            acc = _mm512_srli_epi32(_mm512_add_epi32(acc, round_v), 16);

            _mm256_storeu_si256((__m256i *)(dst + i * dst_stride + j), _mm512_cvtepi32_epi16(acc));

            src_row += 16;
        }

        /* Scalar tail for remaining interior columns */
        for (; j < right_edge; j++) {
            uint32_t accum = (uint32_t)filter[0] * src_row[0] + (uint32_t)filter[1] * src_row[1] +
                             (uint32_t)filter[2] * src_row[2] + (uint32_t)filter[3] * src_row[3] +
                             (uint32_t)filter[4] * src_row[4];
            dst[i * dst_stride + j] = (uint16_t)((accum + shift_add_round) >> 16);
            src_row++;
        }
    }

    /* Right edge columns — scalar mirror boundary */
    for (unsigned i = 0; i < height; i++) {
        for (unsigned j = right_edge; j < width; j++) {
            dst[i * dst_stride + j] = (uint16_t)((edge_16(true, src, (int)width, (int)height,
                                                          (int)src_stride, (int)i, (int)j) +
                                                  shift_add_round) >>
                                                 16);
        }
    }
}
