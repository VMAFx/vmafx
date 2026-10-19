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

#include <assert.h>
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "feature/integer_motion.h"
#include "feature/x86/motion_avx512.h"
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

// acc_lo / acc_hi += v, its low / high eight int32 widened to int64.
static inline void add_widen_epi32(__m512i v, __m512i *acc_lo, __m512i *acc_hi)
{
    *acc_lo = _mm512_add_epi64(*acc_lo, _mm512_cvtepi32_epi64(_mm512_castsi512_si256(v)));
    *acc_hi = _mm512_add_epi64(*acc_hi, _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(v, 1)));
}

// Narrow 8 + 8 int64 to 16 x int32 (signed saturation).
static inline __m512i pack_epi64_sat32(__m512i lo, __m512i hi)
{
    __m256i res_lo = _mm512_cvtsepi64_epi32(lo);
    __m256i res_hi = _mm512_cvtsepi64_epi32(hi);
    return _mm512_inserti64x4(_mm512_castsi256_si512(res_lo), res_hi, 1);
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

// x_conv + abs of the 16 columns starting at `y`; reads y[-2] .. y[17].
static inline __m512i x_conv_abs16_avx512(const int32_t *y)
{
    const __m512i g0 = _mm512_set1_epi32(3571);
    const __m512i g1 = _mm512_set1_epi32(16004);
    const __m512i g2 = _mm512_set1_epi32(26386);
    const __m512i round64 = _mm512_set1_epi64(1 << 15);

    __m512i y0 = _mm512_loadu_si512((const __m512i *)(y - 2));
    __m512i y1 = _mm512_loadu_si512((const __m512i *)(y - 1));
    __m512i y2 = _mm512_loadu_si512((const __m512i *)y);
    __m512i y3 = _mm512_loadu_si512((const __m512i *)(y + 1));
    __m512i y4 = _mm512_loadu_si512((const __m512i *)(y + 2));

    // Each product fits in int32, and so do the two symmetric pairs.
    __m512i s04 = _mm512_add_epi32(_mm512_mullo_epi32(y0, g0), _mm512_mullo_epi32(y4, g0));
    __m512i s13 = _mm512_add_epi32(_mm512_mullo_epi32(y1, g1), _mm512_mullo_epi32(y3, g1));
    __m512i p2 = _mm512_mullo_epi32(y2, g2);

    // Widen to int64 and accumulate.
    __m512i acc_lo = _mm512_setzero_si512();
    __m512i acc_hi = _mm512_setzero_si512();
    add_widen_epi32(s04, &acc_lo, &acc_hi);
    add_widen_epi32(s13, &acc_lo, &acc_hi);
    add_widen_epi32(p2, &acc_lo, &acc_hi);

    // Round and arithmetic right shift >>16 (native in AVX-512)
    acc_lo = _mm512_srai_epi64(_mm512_add_epi64(acc_lo, round64), 16);
    acc_hi = _mm512_srai_epi64(_mm512_add_epi64(acc_hi, round64), 16);

    return _mm512_abs_epi32(pack_epi64_sat32(acc_lo, acc_hi));
}

// SIMD phase 2: x_conv + abs + SAD for one row of int32 y_row.
// Processes 16 int32 columns at a time via mullo_epi32 + int64 accumulation.
static inline uint32_t x_conv_row_sad_avx512(const int32_t *y_row, unsigned w)
{
    uint32_t row_sad = 0;

    // Scalar left edge (columns 0, 1) — mirror boundary
    unsigned j;
    for (j = 0; j < 2 && j < w; j++) {
        row_sad += x_conv_abs_scalar(y_row, j, w);
    }

    // SIMD middle: need y_row[j-2]..y_row[j+17], so j+18 <= w
    __m512i sad_acc = _mm512_setzero_si512();
    for (; j + 18 <= w; j += 16) {
        sad_acc = _mm512_add_epi32(sad_acc, x_conv_abs16_avx512(y_row + j));
    }
    row_sad += (uint32_t)_mm512_reduce_add_epi32(sad_acc);

    // Scalar right edge + tail
    for (; j < w; j++) {
        row_sad += x_conv_abs_scalar(y_row, j, w);
    }

    return row_sad;
}

// prev - cur of 16 uint16 samples, as 16 x int32.
static inline __m512i diff16_u16_avx512(const uint16_t *prev, const uint16_t *cur)
{
    return _mm512_sub_epi32(_mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)prev)),
                            _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)cur)));
}

// y_conv of the frame difference for the 16 columns starting at j (16-bit samples).
static inline __m512i y_conv16_16_avx512(const uint16_t *const pp[5], const uint16_t *const cp[5],
                                         unsigned j, unsigned bpc)
{
    const __m512i g0 = _mm512_set1_epi32(3571);
    const __m512i g1 = _mm512_set1_epi32(16004);
    const __m512i g2 = _mm512_set1_epi32(26386);
    const __m512i round64 = _mm512_set1_epi64(((int64_t)1 << (bpc - 1)));
    const __m512i bpc_vec = _mm512_set1_epi64(bpc);

    __m512i acc_lo = _mm512_setzero_si512();
    __m512i acc_hi = _mm512_setzero_si512();
    add_widen_epi32(_mm512_mullo_epi32(diff16_u16_avx512(pp[0] + j, cp[0] + j), g0), &acc_lo,
                    &acc_hi);
    add_widen_epi32(_mm512_mullo_epi32(diff16_u16_avx512(pp[1] + j, cp[1] + j), g1), &acc_lo,
                    &acc_hi);
    add_widen_epi32(_mm512_mullo_epi32(diff16_u16_avx512(pp[2] + j, cp[2] + j), g2), &acc_lo,
                    &acc_hi);
    add_widen_epi32(_mm512_mullo_epi32(diff16_u16_avx512(pp[3] + j, cp[3] + j), g1), &acc_lo,
                    &acc_hi);
    add_widen_epi32(_mm512_mullo_epi32(diff16_u16_avx512(pp[4] + j, cp[4] + j), g0), &acc_lo,
                    &acc_hi);

    acc_lo = _mm512_srav_epi64(_mm512_add_epi64(acc_lo, round64), bpc_vec);
    acc_hi = _mm512_srav_epi64(_mm512_add_epi64(acc_hi, round64), bpc_vec);

    return pack_epi64_sat32(acc_lo, acc_hi);
}

// Phase 1 for one row of 16-bit samples: diff + y_conv -> y_row (16 pixels at a
// time, int64 accum). Returns nonzero when any y_row value is nonzero.
static inline int y_conv_row_16_avx512(const uint16_t *const pp[5], const uint16_t *const cp[5],
                                       int32_t *y_row, unsigned w, unsigned bpc)
{
    unsigned j;
    __m512i nz_acc = _mm512_setzero_si512();
    for (j = 0; j + 16 <= w; j += 16) {
        __m512i result = y_conv16_16_avx512(pp, cp, j, bpc);
        _mm512_storeu_si512((__m512i *)(y_row + j), result);
        nz_acc = _mm512_or_si512(nz_acc, result);
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

    return _mm512_test_epi32_mask(nz_acc, nz_acc) != 0 || nz_tail;
}

uint64_t motion_score_pipeline_16_avx512(const uint8_t *prev_u8, ptrdiff_t prev_stride,
                                         const uint8_t *cur_u8, ptrdiff_t cur_stride,
                                         int32_t *y_row, unsigned w, unsigned h, unsigned bpc)
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
        if (!y_conv_row_16_avx512(pp, cp, y_row, w, bpc)) {
            continue;
        }

        // Phase 2: SIMD x_conv + abs + accumulate
        sad += x_conv_row_sad_avx512(y_row, w);
    }

    return sad;
}

// prev - cur of 32 uint8 samples, as 32 x int16.
static inline __m512i diff32_u8_avx512(const uint8_t *prev, const uint8_t *cur)
{
    return _mm512_sub_epi16(_mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)prev)),
                            _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)cur)));
}

// acc_lo / acc_hi += d * f, each int16 product widened to int32 (unpacklo /
// unpackhi order: the low and the high half of each 128-bit lane).
static inline void add_mul_widen_epi16(__m512i d, __m512i f, __m512i *acc_lo, __m512i *acc_hi)
{
    __m512i lo = _mm512_mullo_epi16(d, f);
    __m512i hi = _mm512_mulhi_epi16(d, f);
    *acc_lo = _mm512_add_epi32(*acc_lo, _mm512_unpacklo_epi16(lo, hi));
    *acc_hi = _mm512_add_epi32(*acc_hi, _mm512_unpackhi_epi16(lo, hi));
}

// Undo the per-128-bit-lane order of unpacklo / unpackhi and store 32 int32 to
// y_row. Returns the OR of the 32 values folded into 16 lanes.
static inline __m512i store32_unpacked_avx512(__m512i acc_lo, __m512i acc_hi, int32_t *y_row)
{
    __m256i lo_lo = _mm512_castsi512_si256(acc_lo);
    __m256i lo_hi = _mm512_extracti64x4_epi64(acc_lo, 1);
    __m256i hi_lo = _mm512_castsi512_si256(acc_hi);
    __m256i hi_hi = _mm512_extracti64x4_epi64(acc_hi, 1);

    __m256i cols_0_7 = _mm256_permute2x128_si256(lo_lo, hi_lo, 0x20);
    __m256i cols_8_15 = _mm256_permute2x128_si256(lo_lo, hi_lo, 0x31);
    __m256i cols_16_23 = _mm256_permute2x128_si256(lo_hi, hi_hi, 0x20);
    __m256i cols_24_31 = _mm256_permute2x128_si256(lo_hi, hi_hi, 0x31);

    _mm256_storeu_si256((__m256i *)y_row, cols_0_7);
    _mm256_storeu_si256((__m256i *)(y_row + 8), cols_8_15);
    _mm256_storeu_si256((__m256i *)(y_row + 16), cols_16_23);
    _mm256_storeu_si256((__m256i *)(y_row + 24), cols_24_31);

    return _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_or_si256(cols_0_7, cols_8_15)),
                              _mm256_or_si256(cols_16_23, cols_24_31), 1);
}

// y_conv of the frame difference for the 32 columns starting at j (8-bit
// samples, shift >>8), stored to y_row[j .. j+31]. Returns the OR of the stored
// values.
static inline __m512i y_conv32_8_avx512(const uint8_t *const p[5], const uint8_t *const c[5],
                                        unsigned j, int32_t *y_row)
{
    const __m512i f0 = _mm512_set1_epi16(3571);
    const __m512i f1 = _mm512_set1_epi16(16004);
    const __m512i f2 = _mm512_set1_epi16(26386);
    const __m512i round8 = _mm512_set1_epi32(1 << 7);

    __m512i acc_lo = _mm512_setzero_si512();
    __m512i acc_hi = _mm512_setzero_si512();
    add_mul_widen_epi16(diff32_u8_avx512(p[0] + j, c[0] + j), f0, &acc_lo, &acc_hi);
    add_mul_widen_epi16(diff32_u8_avx512(p[1] + j, c[1] + j), f1, &acc_lo, &acc_hi);
    add_mul_widen_epi16(diff32_u8_avx512(p[2] + j, c[2] + j), f2, &acc_lo, &acc_hi);
    add_mul_widen_epi16(diff32_u8_avx512(p[3] + j, c[3] + j), f1, &acc_lo, &acc_hi);
    add_mul_widen_epi16(diff32_u8_avx512(p[4] + j, c[4] + j), f0, &acc_lo, &acc_hi);

    acc_lo = _mm512_srai_epi32(_mm512_add_epi32(acc_lo, round8), 8);
    acc_hi = _mm512_srai_epi32(_mm512_add_epi32(acc_hi, round8), 8);

    return store32_unpacked_avx512(acc_lo, acc_hi, y_row + j);
}

// Phase 1 for one row of 8-bit samples: diff + y_conv -> y_row (32 columns at a
// time). Returns nonzero when any y_row value is nonzero.
static inline int y_conv_row_8_avx512(const uint8_t *const p[5], const uint8_t *const c[5],
                                      int32_t *y_row, unsigned w)
{
    unsigned j;
    __m512i nz_acc = _mm512_setzero_si512();
    for (j = 0; j + 32 <= w; j += 32) {
        nz_acc = _mm512_or_si512(nz_acc, y_conv32_8_avx512(p, c, j, y_row));
    }

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

    return _mm512_test_epi32_mask(nz_acc, nz_acc) != 0 || nz_tail;
}

uint64_t motion_score_pipeline_8_avx512(const uint8_t *prev, ptrdiff_t prev_stride,
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
        if (!y_conv_row_8_avx512(p, c, y_row, w)) {
            continue;
        }

        // Phase 2: SIMD x_conv + abs + accumulate
        sad += x_conv_row_sad_avx512(y_row, w);
    }

    return sad;
}

/* -----------------------------------------------------------------------
 * sad_avx512 — pixel-wise absolute difference sum between two pictures.
 *
 * Operates on the luma plane (data[0]) only.  Both pictures must have the
 * same dimensions and bit-depth.  Processes 32 uint16 samples per SIMD
 * iteration: |a - b| = max(a, b) - min(a, b) in unsigned 16-bit lanes, then
 * widening accumulation. A signed 16-bit difference wraps for 16-bit samples
 * that differ by more than 32767 (65535 - 0 gave 1);
 * T-SIMD-SAD-AVX512-INT16-DIFFERENCE-2026-10-05.
 * ----------------------------------------------------------------------- */
void sad_avx512(VmafPicture *pic_a, VmafPicture *pic_b, uint64_t *sad_out)
{
    assert(pic_a != NULL);
    assert(pic_b != NULL);
    assert(sad_out != NULL);
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
            /* |a[k]-b[k]| per uint16 lane, exact for every 16-bit sample */
            __m512i abs_diff = _mm512_sub_epi16(_mm512_max_epu16(va, vb), _mm512_min_epu16(va, vb));
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

// filter[] applied to five samples. The uint32_t operands keep the products out
// of signed int: uint16_t * uint16_t promotes to int in C, and at UINT16_MAX
// the product can exceed INT32_MAX (UBSan).
static inline uint32_t filter5_scalar(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                      uint32_t a4)
{
    return (uint32_t)filter[0] * a0 + (uint32_t)filter[1] * a1 + (uint32_t)filter[2] * a2 +
           (uint32_t)filter[3] * a3 + (uint32_t)filter[4] * a4;
}

// filter[] applied to five vectors of 16 samples held as uint32 lanes, then
// rounded and shifted right (logical).
static inline __m512i filter5_epu32_avx512(const __m512i v[5], __m512i round_v, unsigned shift)
{
    __m512i acc = _mm512_mullo_epi32(v[0], _mm512_set1_epi32(3571));
    acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v[1], _mm512_set1_epi32(16004)));
    acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v[2], _mm512_set1_epi32(26386)));
    acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v[3], _mm512_set1_epi32(16004)));
    acc = _mm512_add_epi32(acc, _mm512_mullo_epi32(v[4], _mm512_set1_epi32(3571)));
    return _mm512_srli_epi32(_mm512_add_epi32(acc, round_v), shift);
}

// One mirror-boundary row of the vertical filter on an 8-bit source.
static void y_conv_edge_row_8(const uint8_t *src, uint16_t *dst_row, unsigned width,
                              unsigned height, ptrdiff_t src_stride, unsigned i)
{
    const int radius = filter_width / 2;
    for (unsigned j = 0; j < width; j++) {
        uint32_t accum = 0;
        for (int k = 0; k < filter_width; k++) {
            int i_tap = (int)i - radius + k;
            if (i_tap < 0) {
                i_tap = -i_tap;
            } else if (i_tap >= (int)height) {
                i_tap = (int)height - (i_tap - (int)height + 2);
            }
            accum += (uint32_t)filter[k] * (uint32_t)src[(ptrdiff_t)i_tap * src_stride + j];
        }
        dst_row[j] = (uint16_t)((accum + 128u) >> 8u);
    }
}

// One interior row of the vertical filter on an 8-bit source: 32 uint8 pixels
// per iteration, then a scalar tail. s[k] is source row i - 2 + k.
static void y_conv_row_8_interior(const uint8_t *const s[5], uint16_t *dst_row, unsigned width)
{
    const __m512i round_v = _mm512_set1_epi32(128);
    unsigned j = 0;
    for (; j + 32 <= width; j += 32) {
        __m512i lo[5];
        __m512i hi[5];
        for (int k = 0; k < 5; k++) {
            /* Zero-extend 32 uint8 -> 32 uint16, then each half -> 16 uint32 */
            const __m512i v = _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(s[k] + j)));
            lo[k] = _mm512_cvtepu16_epi32(_mm512_castsi512_si256(v));
            hi[k] = _mm512_cvtepu16_epi32(_mm512_extracti64x4_epi64(v, 1));
        }
        /* Narrow int32 -> uint16 and store */
        _mm256_storeu_si256((__m256i *)(dst_row + j),
                            _mm512_cvtepi32_epi16(filter5_epu32_avx512(lo, round_v, 8u)));
        _mm256_storeu_si256((__m256i *)(dst_row + j + 16),
                            _mm512_cvtepi32_epi16(filter5_epu32_avx512(hi, round_v, 8u)));
    }

    for (; j < width; j++) {
        const uint32_t accum = filter5_scalar(s[0][j], s[1][j], s[2][j], s[3][j], s[4][j]);
        dst_row[j] = (uint16_t)((accum + 128u) >> 8u);
    }
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

    /* Top edge rows — scalar mirror boundary */
    for (unsigned i = 0; i < top_edge; i++) {
        y_conv_edge_row_8(src, dst + i * dst_stride, width, height, src_stride, i);
    }

    /* Interior rows — AVX-512 vectorised */
    for (unsigned i = top_edge; i < bottom_edge; i++) {
        const uint8_t *s[5];
        for (int k = 0; k < 5; k++) {
            s[k] = src + ((ptrdiff_t)i - 2 + k) * src_stride;
        }
        y_conv_row_8_interior(s, dst + i * dst_stride, width);
    }

    /* Bottom edge rows — scalar mirror boundary */
    for (unsigned i = bottom_edge; i < height; i++) {
        y_conv_edge_row_8(src, dst + i * dst_stride, width, height, src_stride, i);
    }
}

// One interior row of the vertical filter on a 16-bit source: 16 uint16 pixels
// per iteration, then a scalar tail. s[k] is source row i - 2 + k.
static void y_conv_row_16_interior(const uint16_t *const s[5], uint16_t *dst_row, unsigned width,
                                   unsigned inp_size_bits)
{
    const unsigned add_before_shift = 1u << (inp_size_bits - 1u);
    const __m512i round_v = _mm512_set1_epi32((int32_t)add_before_shift);
    unsigned j = 0;
    for (; j + 16 <= width; j += 16) {
        __m512i v[5];
        for (int k = 0; k < 5; k++) {
            /* Zero-extend 16 uint16 -> 16 uint32 */
            v[k] = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(s[k] + j)));
        }
        /* Narrow int32 -> uint16 and store */
        _mm256_storeu_si256((__m256i *)(dst_row + j),
                            _mm512_cvtepi32_epi16(filter5_epu32_avx512(v, round_v, inp_size_bits)));
    }

    for (; j < width; j++) {
        const uint32_t accum = filter5_scalar(s[0][j], s[1][j], s[2][j], s[3][j], s[4][j]);
        dst_row[j] = (uint16_t)((accum + add_before_shift) >> inp_size_bits);
    }
}

// One mirror-boundary row of the vertical filter on a 16-bit source.
static void y_conv_edge_row_16(const uint16_t *src, uint16_t *dst_row, unsigned width,
                               unsigned height, ptrdiff_t src_stride, unsigned i,
                               unsigned inp_size_bits)
{
    const unsigned add_before_shift = 1u << (inp_size_bits - 1u);
    for (unsigned j = 0; j < width; j++) {
        dst_row[j] = (uint16_t)((edge_16(false, src, (int)width, (int)height, (int)src_stride,
                                         (int)i, (int)j) +
                                 add_before_shift) >>
                                inp_size_bits);
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

    /* Top edge rows — scalar mirror boundary */
    for (unsigned i = 0; i < top_edge; i++) {
        y_conv_edge_row_16(src, dst + i * dst_stride, width, height, src_stride, i, inp_size_bits);
    }

    /* Interior rows — AVX-512 vectorised */
    for (unsigned i = top_edge; i < bottom_edge; i++) {
        const uint16_t *s[5];
        for (int k = 0; k < 5; k++) {
            s[k] = src + ((ptrdiff_t)i - 2 + k) * src_stride;
        }
        y_conv_row_16_interior(s, dst + i * dst_stride, width, inp_size_bits);
    }

    /* Bottom edge rows — scalar mirror boundary */
    for (unsigned i = bottom_edge; i < height; i++) {
        y_conv_edge_row_16(src, dst + i * dst_stride, width, height, src_stride, i, inp_size_bits);
    }
}

// Interior columns [left_edge, right_edge) of one row of the horizontal filter:
// 16 uint16 per iteration, then a scalar tail. src_row points at the first tap
// of column left_edge.
static void x_conv_row_16_interior(const uint16_t *src_row, uint16_t *dst_row, unsigned left_edge,
                                   unsigned right_edge)
{
    const unsigned shift_add_round = 32768u;
    const __m512i round_v = _mm512_set1_epi32((int32_t)shift_add_round);
    unsigned j = left_edge;
    for (; j + 16 <= right_edge; j += 16) {
        __m512i v[5];
        for (int k = 0; k < 5; k++) {
            v[k] = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(src_row + k)));
        }
        _mm256_storeu_si256((__m256i *)(dst_row + j),
                            _mm512_cvtepi32_epi16(filter5_epu32_avx512(v, round_v, 16u)));
        src_row += 16;
    }

    for (; j < right_edge; j++) {
        const uint32_t accum =
            filter5_scalar(src_row[0], src_row[1], src_row[2], src_row[3], src_row[4]);
        dst_row[j] = (uint16_t)((accum + shift_add_round) >> 16);
        src_row++;
    }
}

// Mirror-boundary columns [first, last) of one row of the horizontal filter.
static void x_conv_edge_cols_16(const uint16_t *src, uint16_t *dst_row, unsigned width,
                                unsigned height, ptrdiff_t src_stride, unsigned i, unsigned first,
                                unsigned last)
{
    for (unsigned j = first; j < last; j++) {
        dst_row[j] = (uint16_t)((edge_16(true, src, (int)width, (int)height, (int)src_stride,
                                         (int)i, (int)j) +
                                 32768u) >>
                                16);
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

    /* Left edge columns — scalar mirror boundary */
    for (unsigned i = 0; i < height; i++) {
        x_conv_edge_cols_16(src, dst + i * dst_stride, width, height, src_stride, i, 0, left_edge);
    }

    /* Interior columns; the first tap of column left_edge is src[i][left_edge - radius] */
    for (unsigned i = 0; i < height; i++) {
        x_conv_row_16_interior(src + (ptrdiff_t)i * src_stride + (left_edge - radius),
                               dst + i * dst_stride, left_edge, right_edge);
    }

    /* Right edge columns — scalar mirror boundary */
    for (unsigned i = 0; i < height; i++) {
        x_conv_edge_cols_16(src, dst + i * dst_stride, width, height, src_stride, i, right_edge,
                            width);
    }
}
