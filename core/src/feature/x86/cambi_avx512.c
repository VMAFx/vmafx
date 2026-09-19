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

#include <assert.h>
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "libvmaf/picture.h"
#include "cambi_avx512.h"
#include "cambi.h"
#include "cambi_c_values_frame.h"

/* Lane mask for the first n (0 .. 32) of 32 uint16 lanes. */
static inline __mmask32 first_lanes32(int n)
{
    return (__mmask32)(0xFFFFFFFFull >> (unsigned)(32 - n));
}

/* arr[left, right) += delta (modulo 2^16, like the scalar ++ / --). The last
 * partial block is a masked load / store, so no element outside the range is
 * read or written, and a window narrower than one vector (every window below
 * 1080p) costs one masked block instead of a scalar loop. */
static FORCE_INLINE void cambi_add_range_avx512(uint16_t *arr, int left, int right, __m512i delta)
{
    int col = left;
    for (; col + 32 <= right; col += 32) {
        const __m512i v = _mm512_loadu_si512((const void *)&arr[col]);
        _mm512_storeu_si512((void *)&arr[col], _mm512_add_epi16(v, delta));
    }
    const int rest = right - col;
    if (rest > 0) {
        const __mmask32 k = first_lanes32(rest);
        const __m512i v = _mm512_maskz_loadu_epi16(k, &arr[col]);
        _mm512_mask_storeu_epi16(&arr[col], k, _mm512_add_epi16(v, delta));
    }
}

void cambi_increment_range_avx512(uint16_t *arr, int left, int right)
{
    cambi_add_range_avx512(arr, left, right, _mm512_set1_epi16(1));
}

void cambi_decrement_range_avx512(uint16_t *arr, int left, int right)
{
    /* Adding 0xFFFF is subtracting 1 modulo 2^16. */
    cambi_add_range_avx512(arr, left, right, _mm512_set1_epi16(-1));
}

/* 1 where px[col] equals its right neighbour and the pixel below, for the
 * lanes in k; loads are masked, so nothing outside k is read. */
static inline __m512i zero_derivative_avx512(const uint16_t *px, const uint16_t *below, __mmask32 k)
{
    const __m512i v = _mm512_maskz_loadu_epi16(k, px);
    const __mmask32 eq = _mm512_mask_cmpeq_epi16_mask(k, v, _mm512_maskz_loadu_epi16(k, px + 1)) &
                         _mm512_cmpeq_epi16_mask(v, _mm512_maskz_loadu_epi16(k, below));
    return _mm512_maskz_mov_epi16(eq, _mm512_set1_epi16(1));
}

void get_derivative_data_for_row_avx512(const uint16_t *image_data, uint16_t *derivative_buffer,
                                        int width, int height, int row, int stride)
{
    const uint16_t *px = &image_data[(ptrdiff_t)row * stride];
    /* The last row compares with itself vertically, which is always equal:
     * the scalar's `row == height - 1 ||` short cut. */
    const uint16_t *below = (row == height - 1) ? px : &px[stride];
    int col = 0;
    /* Reads px[col + 32]; the last column (no right neighbour) is the scalar's
     * `col == width - 1` case and is written after the loop. */
    for (; col + 32 < width; col += 32) {
        _mm512_storeu_si512((void *)&derivative_buffer[col],
                            zero_derivative_avx512(&px[col], &below[col], 0xFFFFFFFFu));
    }
    const int rest = width - 1 - col; /* 0 .. 31 columns left before the last */
    if (rest > 0) {
        const __mmask32 k = first_lanes32(rest);
        _mm512_mask_storeu_epi16(&derivative_buffer[col], k,
                                 zero_derivative_avx512(&px[col], &below[col], k));
    }
    derivative_buffer[width - 1] = (uint16_t)(px[width - 1] == below[width - 1]);
}

/*
 * calculate_c_values_row_avx512 — 16-lane wide port of calculate_c_values_row_avx2.
 *
 * CAMBI is an integer pipeline: all histogram counts are uint16 and the
 * per-lane arithmetic is integer until the final float multiply by
 * reciprocal_lut.  No float reduction trees exist, so AVX-512 output is
 * bit-identical to the scalar reference when run on the same histogram state.
 * (ADR-0452 / SIMD-bitexact contract per ADR-0138/0139.)
 *
 * Design notes vs the AVX2 sibling:
 *  - 16-lane i32 gather (scale=2 → vpgatherdps with 4-byte gather on 2-byte
 *    elements is not directly supported; use _mm512_i32gather_epi32 scale=2
 *    + mask with lo16, matching the AVX2 pattern).
 *  - AVX-512 mask registers replace the AVX2 testz+continue shortcut: we use
 *    _mm512_mask_storeu_ps to conditionally write lanes; an all-zero __mmask16
 *    causes no writes without the testz branch.
 *  - The inner predicate loop uses __mmask16 throughout to avoid the
 *    _mm512_testz fallback.
 *  - Gather: _mm512_i32gather_epi32 with scale=2 (uint16 elements at byte
 *    offsets index*2). The AVX-512 version carries no imm8 scale restriction;
 *    scale=2 is legal and saves the shift-then-add offset arithmetic.
 */
static inline __m512 accumulate_c_value_chunk_avx512(
    __m512i value_v, __m512i compact_v, __m512i col_v, __m512i p0, __mmask16 mask_active,
    const uint16_t *histograms, int width, const uint16_t num_diffs, const uint16_t *tvi_thresholds,
    const int *diff_weights, const int *all_diffs, const float *reciprocal_lut, __m512i width_v,
    __m512i vlt_luma_v, __m512i band_max_v, __m512i zero, __m512i lo16_mask)
{
    (void)width;
    __m512 c_value = _mm512_setzero_ps();

    for (int d = 0; d < num_diffs; d++) {
        int delta_plus = all_diffs[num_diffs + d + 1];
        int delta_minus = all_diffs[num_diffs - d - 1];
        int weight = diff_weights[d];
        int tvi_thresh = tvi_thresholds[d];

        /* pred_a: value <= tvi_thresh — compare gives mask of lanes satisfying. */
        __mmask16 pred_a = _mm512_cmple_epi32_mask(value_v, _mm512_set1_epi32(tvi_thresh));

        /* pred_b: (value + delta_plus) > vlt_luma. */
        __m512i value_plus = _mm512_add_epi32(value_v, _mm512_set1_epi32(delta_plus));
        __mmask16 pred_b = _mm512_cmpgt_epi32_mask(value_plus, vlt_luma_v);

        __mmask16 predicate = pred_a & pred_b & mask_active;
        if (predicate == 0) {
            continue;
        }

        /* compact_plus clamped for safe gather; compact_minus with OOB tracking. */
        __m512i compact_plus_raw = _mm512_add_epi32(compact_v, _mm512_set1_epi32(delta_plus));
        __m512i compact_plus = _mm512_min_epi32(compact_plus_raw, band_max_v);

        __m512i compact_minus_raw = _mm512_add_epi32(compact_v, _mm512_set1_epi32(delta_minus));
        __mmask16 p2_inbounds = _mm512_cmpgt_epi32_mask(compact_minus_raw, _mm512_set1_epi32(-1));
        __m512i compact_minus = _mm512_max_epi32(compact_minus_raw, zero);

        /* p1 / p2 gathers. */
        __m512i p1_idx = _mm512_add_epi32(_mm512_mullo_epi32(compact_plus, width_v), col_v);
        __m512i p1 =
            _mm512_and_si512(_mm512_i32gather_epi32(p1_idx, (const int *)histograms, 2), lo16_mask);

        __m512i p2_idx = _mm512_add_epi32(_mm512_mullo_epi32(compact_minus, width_v), col_v);
        __m512i p2 =
            _mm512_and_si512(_mm512_i32gather_epi32(p2_idx, (const int *)histograms, 2), lo16_mask);
        /* Zero OOB lanes in p2. */
        p2 = _mm512_maskz_mov_epi32(p2_inbounds, p2);

        __m512i p_max = _mm512_max_epu32(p1, p2);
        __m512i denom = _mm512_add_epi32(p_max, p0);

        /* num = weight * p0 * p_max; all values bounded by uint16 so i32 mul is safe. */
        __m512i num_int =
            _mm512_mullo_epi32(_mm512_set1_epi32(weight), _mm512_mullo_epi32(p0, p_max));
        __m512 num_f = _mm512_cvtepi32_ps(num_int);

        /* rcp = reciprocal_lut[denom]; LUT is hot in L1. */
        __m512 rcp = _mm512_i32gather_ps(denom, reciprocal_lut, 4);

        __m512 val = _mm512_mul_ps(num_f, rcp);
        /* Mask off lanes where predicate is false. */
        val = _mm512_maskz_mov_ps(predicate, val);
        c_value = _mm512_max_ps(c_value, val);
    }
    return c_value;
}

static inline float calculate_c_value_pixel_scalar_avx512(
    uint16_t img_val, int col, int width, const uint16_t *histograms, const uint16_t num_diffs,
    const uint16_t *tvi_thresholds, uint16_t vlt_luma, const int *diff_weights,
    const int *all_diffs, const float *reciprocal_lut, uint16_t v_band_base, uint16_t v_band_size)
{
    int compact_v_signed = (int)img_val - (int)v_band_base;
    if ((unsigned)compact_v_signed >= v_band_size) {
        return 0.0f;
    }

    uint16_t value = (uint16_t)(img_val + num_diffs);
    uint16_t compact_v_sc = (uint16_t)compact_v_signed;
    uint16_t p_0 = histograms[(ptrdiff_t)compact_v_sc * width + col];
    float c_v = 0.0f;

    for (int d = 0; d < num_diffs; d++) {
        if ((value <= tvi_thresholds[d]) && ((value + all_diffs[num_diffs + d + 1]) > vlt_luma)) {
            int idx1 = compact_v_signed + all_diffs[num_diffs + d + 1];
            int idx2 = compact_v_signed + all_diffs[num_diffs - d - 1];
            uint16_t p_1 = histograms[(ptrdiff_t)idx1 * width + col];
            uint16_t p_2 = (idx2 >= 0) ? histograms[(ptrdiff_t)idx2 * width + col] : 0;
            uint16_t p_max = (p_1 > p_2) ? p_1 : p_2;
            float val = (float)(diff_weights[d] * p_0 * p_max) * reciprocal_lut[p_max + p_0];
            if (val > c_v) {
                c_v = val;
            }
        }
    }
    return c_v;
}

static void calculate_c_values_row_scalar_tail_avx512(
    float *c_row, const uint16_t *histograms, const uint16_t *image_row, const uint16_t *mask_row,
    int col_start, int width, const uint16_t num_diffs, const uint16_t *tvi_thresholds,
    uint16_t vlt_luma, const int *diff_weights, const int *all_diffs, const float *reciprocal_lut,
    uint16_t v_band_base, uint16_t v_band_size)
{
    for (int col = col_start; col < width; col++) {
        c_row[col] = mask_row[col] ? calculate_c_value_pixel_scalar_avx512(
                                         image_row[col], col, width, histograms, num_diffs,
                                         tvi_thresholds, vlt_luma, diff_weights, all_diffs,
                                         reciprocal_lut, v_band_base, v_band_size) :
                                     0.0f;
    }
}

static inline int process_c_values_row_chunks_avx512(
    float *c_row, const uint16_t *image_row, const uint16_t *mask_row, const uint16_t *histograms,
    int width, const uint16_t num_diffs, const uint16_t *tvi_thresholds, const int *diff_weights,
    const int *all_diffs, const float *reciprocal_lut, uint16_t v_band_base, uint16_t v_band_size,
    __m512i vlt_luma_v)
{
    const __m512i col_base = _mm512_set_epi32(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    const __m512i width_v = _mm512_set1_epi32(width);
    const __m512i num_diffs_v = _mm512_set1_epi32(num_diffs);
    const __m512i lo16_mask = _mm512_set1_epi32(0xFFFF);
    const __m512i band_offset_v = _mm512_set1_epi32((int)num_diffs + (int)v_band_base);
    const __m512i band_max_v = _mm512_set1_epi32((int)v_band_size - 1);
    const __m512i zero = _mm512_setzero_si512();

    int col = 0;
    for (; col + 16 < width; col += 16) {
        /* Load 16 mask values, promote to i32. */
        __m256i mask16 = _mm256_loadu_si256((const __m256i *)&mask_row[col]);
        __m512i mask32 = _mm512_cvtepu16_epi32(mask16);
        __mmask16 mask_active = _mm512_cmpneq_epi32_mask(mask32, zero);

        /* Skip chunk if no lane is active: mask store of zeros is a no-op. */
        if (mask_active == 0) {
            _mm512_mask_storeu_ps(&c_row[col], (__mmask16)0xFFFF, _mm512_setzero_ps());
            continue;
        }

        /* value = image[col + lane] + num_diffs (adjusted space). */
        __m256i img16 = _mm256_loadu_si256((const __m256i *)&image_row[col]);
        __m512i value_v = _mm512_add_epi32(_mm512_cvtepu16_epi32(img16), num_diffs_v);

        /* compact_v = value_v - band_offset, clamped to [0, band_max]. */
        __m512i compact_v = _mm512_sub_epi32(value_v, band_offset_v);
        compact_v = _mm512_max_epi32(compact_v, zero);
        compact_v = _mm512_min_epi32(compact_v, band_max_v);

        __m512i col_v = _mm512_add_epi32(_mm512_set1_epi32(col), col_base);

        /* p_0 gather: histograms[compact_v * width + col + lane]. */
        __m512i p0_idx = _mm512_add_epi32(_mm512_mullo_epi32(compact_v, width_v), col_v);
        __m512i p0 =
            _mm512_and_si512(_mm512_i32gather_epi32(p0_idx, (const int *)histograms, 2), lo16_mask);

        __m512 c_value = accumulate_c_value_chunk_avx512(
            value_v, compact_v, col_v, p0, mask_active, histograms, width, num_diffs,
            tvi_thresholds, diff_weights, all_diffs, reciprocal_lut, width_v, vlt_luma_v,
            band_max_v, zero, lo16_mask);

        /* Apply active-lane mask and store 16 floats. */
        c_value = _mm512_maskz_mov_ps(mask_active, c_value);
        _mm512_storeu_ps(&c_row[col], c_value);
    }
    return col;
}

void calculate_c_values_row_avx512(float *c_values, const uint16_t *histograms,
                                   const uint16_t *image, const uint16_t *mask, int row, int width,
                                   ptrdiff_t stride, const uint16_t num_diffs,
                                   const uint16_t *tvi_thresholds, uint16_t vlt_luma,
                                   const int *diff_weights, const int *all_diffs,
                                   const float *reciprocal_lut)
{
    int v_lo_signed_sc = (int)vlt_luma - 3 * (int)num_diffs + 1;
    uint16_t v_band_base = v_lo_signed_sc > 0 ? (uint16_t)v_lo_signed_sc : 0;
    uint16_t v_band_size = tvi_thresholds[num_diffs - 1] + 1 - v_band_base;

    const uint16_t *image_row = &image[row * stride];
    const uint16_t *mask_row = &mask[row * stride];
    float *c_row = &c_values[(ptrdiff_t)row * width];
    const __m512i vlt_luma_v = _mm512_set1_epi32(vlt_luma);

    int col = process_c_values_row_chunks_avx512(
        c_row, image_row, mask_row, histograms, width, num_diffs, tvi_thresholds, diff_weights,
        all_diffs, reciprocal_lut, v_band_base, v_band_size, vlt_luma_v);

    calculate_c_values_row_scalar_tail_avx512(c_row, histograms, image_row, mask_row, col, width,
                                              num_diffs, tvi_thresholds, vlt_luma, diff_weights,
                                              all_diffs, reciprocal_lut, v_band_base, v_band_size);
}

/*
 * Spatial-mask row kernels: 16-lane twins of compute_dp_row_avx2 /
 * compute_mask_row_avx2 (adapted from upstream Netflix/vmaf 86da14d03).
 * Integer-only and bit-exact against the scalar compute_dp_row /
 * compute_mask_row in cambi.c for every input.
 */

/* Inclusive prefix sum of the sixteen uint32 lanes (modular, like the
 * scalar). valignd against zero shifts the vector up by k lanes. */
static inline __m512i inclusive_prefix_epi32_avx512(__m512i x)
{
    const __m512i zero = _mm512_setzero_si512();
    x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 15));
    x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 14));
    x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 12));
    x = _mm512_add_epi32(x, _mm512_alignr_epi32(x, zero, 8));
    return x;
}

void compute_dp_row_avx512(uint32_t *dp_curr, const uint32_t *dp_prev, const uint16_t *deriv,
                           int width, int pad_size, bool deriv_valid)
{
    const int dp_offset = pad_size + 1;
    const int actual_width = deriv_valid ? width : 0;
    const __m512i last_lane = _mm512_set1_epi32(15);
    __m512i carry = _mm512_setzero_si512();
    int j = 0;
    for (; j + 16 <= actual_width; j += 16) {
        const __m512i d = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)&deriv[j]));
        const __m512i scan = inclusive_prefix_epi32_avx512(d);
        const __m512i prev = _mm512_loadu_si512((const void *)&dp_prev[dp_offset + j]);
        _mm512_storeu_si512((void *)&dp_curr[dp_offset + j],
                            _mm512_add_epi32(prev, _mm512_add_epi32(scan, carry)));
        /* Only this add is loop-carried; the block total does not wait on carry. */
        carry = _mm512_add_epi32(carry, _mm512_permutexvar_epi32(last_lane, scan));
    }
    uint32_t prefix = (uint32_t)_mm_cvtsi128_si32(_mm512_castsi512_si128(carry));
    for (; j < actual_width; j++) {
        prefix += deriv[j];
        dp_curr[dp_offset + j] = dp_prev[dp_offset + j] + prefix;
    }
    const int n = width + pad_size;
    for (; j < n; j++) {
        dp_curr[dp_offset + j] = dp_prev[dp_offset + j] + prefix;
    }
}

void compute_mask_row_avx512(uint16_t *mask_row, const uint32_t *dp_bottom, const uint32_t *dp_top,
                             int width, int pad_size, uint32_t mask_index)
{
    const int delta = 2 * pad_size + 1;
    const __m512i midx = _mm512_set1_epi32((int32_t)mask_index);
    const __m256i one = _mm256_set1_epi16(1);
    int j = 0;
    for (; j + 16 <= width; j += 16) {
        const __m512i bd = _mm512_loadu_si512((const void *)&dp_bottom[j + delta]);
        const __m512i t = _mm512_loadu_si512((const void *)&dp_top[j]);
        const __m512i b = _mm512_loadu_si512((const void *)&dp_bottom[j]);
        const __m512i td = _mm512_loadu_si512((const void *)&dp_top[j + delta]);
        const __m512i result = _mm512_sub_epi32(_mm512_add_epi32(bd, t), _mm512_add_epi32(b, td));
        /* AVX-512F has the unsigned compare the scalar performs. */
        const __mmask16 gt = _mm512_cmpgt_epu32_mask(result, midx);
        _mm256_storeu_si256((__m256i *)&mask_row[j], _mm256_maskz_mov_epi16(gt, one));
    }
    for (; j < width; j++) {
        const uint32_t result = dp_bottom[j + delta] + dp_top[j] - dp_bottom[j] - dp_top[j + delta];
        mask_row[j] = (uint16_t)(result > mask_index);
    }
}

/*
 * Preprocessing and per-scale kernels: 32-lane twins of decimate_avx2,
 * anti_dithering_filter_avx2 and filter_mode_avx2. All three are integer-only
 * and bit-exact against the scalar decimate / anti_dithering_filter /
 * filter_mode in cambi.c for every uint16 input. Row tails are one masked
 * block instead of up to 31 scalar steps: CAMBI's smaller scales are only a
 * few vectors wide, so the tail is a large share of each row.
 */

/* vpermt2w selector: the even elements of the 64-element pair (lo, hi). */
static const uint16_t k_even_lanes[32] = {0,  2,  4,  6,  8,  10, 12, 14, 16, 18, 20,
                                          22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42,
                                          44, 46, 48, 50, 52, 54, 56, 58, 60, 62};

/* dst[i, j] = src[2i, 2j] in place. Reads stay ahead of writes within a row and
 * across rows, as in the scalar loop. The full blocks read up to
 * src[2 * width - 1], the same bound as decimate_avx2; the masked tail reads
 * exactly the even elements the scalar reads. */
void decimate_avx512(VmafPicture *image, unsigned width, unsigned height)
{
    assert(image->data[0]);
    assert(width > 0u && height > 0u);
    uint16_t *data = image->data[0];
    const ptrdiff_t stride = image->stride[0] >> 1;
    const __m512i even = _mm512_loadu_si512((const void *)k_even_lanes);
    for (unsigned i = 0; i < height; i++) {
        const uint16_t *src = &data[(ptrdiff_t)2 * (ptrdiff_t)i * stride];
        uint16_t *dst = &data[(ptrdiff_t)i * stride];
        unsigned j = 0;
        for (; j + 32 <= width; j += 32) {
            const __m512i lo = _mm512_loadu_si512((const void *)&src[(size_t)2 * j]);
            const __m512i hi = _mm512_loadu_si512((const void *)&src[(size_t)2 * j + 32]);
            _mm512_storeu_si512((void *)&dst[j], _mm512_permutex2var_epi16(lo, even, hi));
        }
        const unsigned rest = width - j; /* 0 .. 31 outputs */
        if (rest > 0) {
            /* 2 * rest - 1 source elements, split over the two 32-lane loads. */
            const uint64_t src_lanes = (1ull << (2u * rest - 1u)) - 1u;
            const __m512i lo = _mm512_maskz_loadu_epi16((__mmask32)src_lanes, &src[(size_t)2 * j]);
            const __m512i hi =
                _mm512_maskz_loadu_epi16((__mmask32)(src_lanes >> 32), &src[(size_t)2 * j + 32]);
            _mm512_mask_storeu_epi16(&dst[j], first_lanes32((int)rest),
                                     _mm512_permutex2var_epi16(lo, even, hi));
        }
    }
}

/* floor((a + b + c + d) / 4) for the lanes in k of a 2x2 window, exact for any
 * uint16 input without widening: with x = 4 * (x >> 2) + (x & 3) for each tap,
 * floor(sum / 4) = sum(x >> 2) + floor(sum(x & 3) / 4), and neither partial
 * sum can exceed 65535. */
static inline __m512i box_average_2x2_avx512(const uint16_t *row0, const uint16_t *row1,
                                             __mmask32 k)
{
    const __m512i three = _mm512_set1_epi16(3);
    const __m512i a = _mm512_maskz_loadu_epi16(k, row0);
    const __m512i b = _mm512_maskz_loadu_epi16(k, row0 + 1);
    const __m512i c = _mm512_maskz_loadu_epi16(k, row1);
    const __m512i d = _mm512_maskz_loadu_epi16(k, row1 + 1);
    const __m512i quarters =
        _mm512_add_epi16(_mm512_add_epi16(_mm512_srli_epi16(a, 2), _mm512_srli_epi16(b, 2)),
                         _mm512_add_epi16(_mm512_srli_epi16(c, 2), _mm512_srli_epi16(d, 2)));
    const __m512i remainders =
        _mm512_add_epi16(_mm512_add_epi16(_mm512_and_si512(a, three), _mm512_and_si512(b, three)),
                         _mm512_add_epi16(_mm512_and_si512(c, three), _mm512_and_si512(d, three)));
    return _mm512_add_epi16(quarters, _mm512_srli_epi16(remainders, 2));
}

/* floor((a + b) / 2) for the lanes in k of a horizontal pair, without
 * overflow: a + b = 2 * (a & b) + (a ^ b). */
static inline __m512i pair_average_avx512(const uint16_t *row, __mmask32 k)
{
    const __m512i a = _mm512_maskz_loadu_epi16(k, row);
    const __m512i b = _mm512_maskz_loadu_epi16(k, row + 1);
    return _mm512_add_epi16(_mm512_and_si512(a, b), _mm512_srli_epi16(_mm512_xor_si512(a, b), 1));
}

/* One row of the 2x2 average in place; the last column averages vertically.
 * Every block reads row0[j + 32] before a later block overwrites it. */
static void anti_dithering_row_avx512(uint16_t *row0, const uint16_t *row1, unsigned width)
{
    unsigned j = 0;
    for (; j + 32 < width; j += 32) {
        _mm512_storeu_si512((void *)&row0[j],
                            box_average_2x2_avx512(&row0[j], &row1[j], 0xFFFFFFFFu));
    }
    const int rest = (int)(width - 1 - j); /* 0 .. 31 columns before the last */
    if (rest > 0) {
        const __mmask32 k = first_lanes32(rest);
        _mm512_mask_storeu_epi16(&row0[j], k, box_average_2x2_avx512(&row0[j], &row1[j], k));
    }
    row0[width - 1] = (uint16_t)((row0[width - 1] + row1[width - 1]) >> 1);
}

/* The last row averages each pixel with its right neighbour, in place. */
static void anti_dithering_last_row_avx512(uint16_t *row, unsigned width)
{
    unsigned j = 0;
    for (; j + 32 < width; j += 32) {
        _mm512_storeu_si512((void *)&row[j], pair_average_avx512(&row[j], 0xFFFFFFFFu));
    }
    const int rest = (int)(width - 1 - j);
    if (rest > 0) {
        const __mmask32 k = first_lanes32(rest);
        _mm512_mask_storeu_epi16(&row[j], k, pair_average_avx512(&row[j], k));
    }
}

void anti_dithering_filter_avx512(VmafPicture *pic, unsigned width, unsigned height)
{
    assert(pic->data[0]);
    assert(width > 0u && height > 0u);
    uint16_t *data = pic->data[0];
    const ptrdiff_t stride = pic->stride[0] >> 1;
    for (unsigned i = 0; i + 1 < height; i++) {
        anti_dithering_row_avx512(&data[(ptrdiff_t)i * stride], &data[(ptrdiff_t)(i + 1) * stride],
                                  width);
    }
    anti_dithering_last_row_avx512(&data[(ptrdiff_t)(height - 1) * stride], width);
}

/* The duplicate among (a, b, c) if any pair matches, otherwise the unsigned
 * minimum: the scalar mode3() in cambi.c, one mask per predicate. */
static inline __m512i mode3_avx512(__m512i a, __m512i b, __m512i c)
{
    const __mmask32 a_dup = _mm512_cmpeq_epi16_mask(a, b) | _mm512_cmpeq_epi16_mask(a, c);
    const __mmask32 bc_eq = _mm512_cmpeq_epi16_mask(b, c);
    const __m512i min_abc = _mm512_min_epu16(_mm512_min_epu16(a, b), c);
    return _mm512_mask_blend_epi16(a_dup, _mm512_mask_blend_epi16(bc_eq, min_abc, b), a);
}

/* mode3 of (p[-1], p[0], p[1]) for the lanes in k. */
static inline __m512i mode3_horizontal_avx512(const uint16_t *p, __mmask32 k)
{
    return mode3_avx512(_mm512_maskz_loadu_epi16(k, p - 1), _mm512_maskz_loadu_epi16(k, p),
                        _mm512_maskz_loadu_epi16(k, p + 1));
}

/* Horizontal pass of one row into buf: mode3 of each interior pixel and its two
 * neighbours; the first and last columns are copied. */
static void filter_mode_row_avx512(const uint16_t *row, uint16_t *buf, int width)
{
    buf[0] = row[0];
    int j = 1;
    /* Writes buf[j .. j + 31] and reads row[j + 32]: both need j + 32 <= width - 1
     * (the last mode3 column is width - 2). */
    for (; j + 32 < width; j += 32) {
        _mm512_storeu_si512((void *)&buf[j], mode3_horizontal_avx512(&row[j], 0xFFFFFFFFu));
    }
    const int rest = width - 1 - j; /* 0 .. 31 mode3 columns left */
    if (rest > 0) {
        const __mmask32 k = first_lanes32(rest);
        _mm512_mask_storeu_epi16(&buf[j], k, mode3_horizontal_avx512(&row[j], k));
    }
    buf[width - 1] = row[width - 1];
}

/* Vertical pass: out = mode3 of the three buffered rows, column by column. */
static void filter_mode_column_avx512(const uint16_t *buffer, uint16_t *out, int width)
{
    const uint16_t *b0 = buffer;
    const uint16_t *b1 = &buffer[width];
    const uint16_t *b2 = &buffer[(ptrdiff_t)2 * width];
    int j = 0;
    for (; j + 32 <= width; j += 32) {
        const __m512i a = _mm512_loadu_si512((const void *)&b0[j]);
        const __m512i b = _mm512_loadu_si512((const void *)&b1[j]);
        const __m512i c = _mm512_loadu_si512((const void *)&b2[j]);
        _mm512_storeu_si512((void *)&out[j], mode3_avx512(a, b, c));
    }
    const int rest = width - j;
    if (rest > 0) {
        const __mmask32 k = first_lanes32(rest);
        const __m512i a = _mm512_maskz_loadu_epi16(k, &b0[j]);
        const __m512i b = _mm512_maskz_loadu_epi16(k, &b1[j]);
        const __m512i c = _mm512_maskz_loadu_epi16(k, &b2[j]);
        _mm512_mask_storeu_epi16(&out[j], k, mode3_avx512(a, b, c));
    }
}

void filter_mode_avx512(const VmafPicture *image, int width, int height, uint16_t *buffer)
{
    assert(image->data[0]);
    assert(width > 0 && height > 0 && buffer);
    uint16_t *data = image->data[0];
    const ptrdiff_t stride = image->stride[0] >> 1;
    int curr_line = 0;
    for (int i = 0; i < height; i++) {
        filter_mode_row_avx512(&data[(ptrdiff_t)i * stride], &buffer[(ptrdiff_t)curr_line * width],
                               width);
        if (i > 1) {
            filter_mode_column_avx512(buffer, &data[(ptrdiff_t)(i - 1) * stride], width);
        }
        curr_line = (curr_line + 1 == 3 ? 0 : curr_line + 1);
    }
}

/*
 * Frame-level c-values driver: the shared calculate_c_values walk
 * (cambi_c_values_frame.h) with the range updaters and row kernel above, and
 * column scans that test 32 pixels per compare for "this column needs a
 * histogram update". Histogram updates are integer and commute per cell, and
 * the row kernel matches calculate_c_values_row bit for bit, so the c-values
 * equal the scalar calculate_c_values output byte for byte.
 */

/* The two scans stay out of line: each call builds its own broadcast
 * constants for a block of up to CAMBI_SCAN_BLOCK columns, so the frame walk
 * keeps no vector value live across its calls to the row kernel. Inlined,
 * Clang hoisted those broadcasts and spilled them around every call, and a
 * wide spill is what faults under the Win64 ABI (ADR-1254). cl.exe has no
 * __attribute__. */
#if defined(_MSC_VER)
#define CAMBI_SCAN_NOINLINE __declspec(noinline)
#else
#define CAMBI_SCAN_NOINLINE __attribute__((noinline))
#endif

/* Lanes in k whose pixel is unmasked and in the scored band [base, base +
 * size); *value gets the pixel values (zero outside k). */
static inline __mmask32 in_band_avx512(const CambiCValuesFrame *f, ptrdiff_t at, __mmask32 k,
                                       __m512i base, __m512i size, __m512i *value)
{
    const __m512i m = _mm512_maskz_loadu_epi16(k, &f->mask[at]);
    const __m512i v = _mm512_maskz_loadu_epi16(k, &f->image[at]);
    *value = v;
    return _mm512_mask_test_epi16_mask(k, m, m) &
           _mm512_cmplt_epu16_mask(_mm512_sub_epi16(v, base), size);
}

CAMBI_SCAN_NOINLINE static void scan_row_avx512(const CambiCValuesFrame *f, int row, int j0, int n,
                                                uint32_t *masks)
{
    const __m512i base = _mm512_set1_epi16((short)f->v_band_base);
    const __m512i size = _mm512_set1_epi16((short)f->v_band_size);
    const ptrdiff_t at = (ptrdiff_t)row * f->stride + j0;
    for (int b = 0; b < n; b += 32) {
        __m512i v;
        masks[b / 32] =
            (uint32_t)in_band_avx512(f, at + b, first_lanes32(MIN(32, n - b)), base, size, &v);
    }
}

CAMBI_SCAN_NOINLINE static void scan_slide_avx512(const CambiCValuesFrame *f, int row_sub,
                                                  int row_add, int j0, int n, uint32_t *masks)
{
    const __m512i base = _mm512_set1_epi16((short)f->v_band_base);
    const __m512i size = _mm512_set1_epi16((short)f->v_band_size);
    const ptrdiff_t at_sub = (ptrdiff_t)row_sub * f->stride + j0;
    const ptrdiff_t at_add = (ptrdiff_t)row_add * f->stride + j0;
    for (int b = 0; b < n; b += 32) {
        const __mmask32 k = first_lanes32(MIN(32, n - b));
        __m512i v_sub;
        __m512i v_add;
        const __mmask32 sub_in = in_band_avx512(f, at_sub + b, k, base, size, &v_sub);
        const __mmask32 add_in = in_band_avx512(f, at_add + b, k, base, size, &v_add);
        /* uh_slide skips a column whose two pixels are both in with one value. */
        const __mmask32 cancel = sub_in & add_in & _mm512_cmpeq_epi16_mask(v_sub, v_add);
        masks[b / 32] = (uint32_t)((sub_in | add_in) & ~cancel);
    }
}

void calculate_c_values_avx512(VmafPicture *pic, const VmafPicture *mask_pic, float *c_values,
                               uint16_t *histograms, uint16_t window_size, const uint16_t num_diffs,
                               const uint16_t *tvi_for_diff, uint16_t vlt_luma,
                               const int *diff_weights, const int *all_diffs, int width, int height)
{
    CambiCValuesFrame f = {
        .c_values = c_values,
        .histograms = histograms,
        .image = pic->data[0],
        .mask = mask_pic->data[0],
        .tvi_for_diff = tvi_for_diff,
        .diff_weights = diff_weights,
        .all_diffs = all_diffs,
        .stride = pic->stride[0] >> 1,
        .width = width,
        .height = height,
        .pad_size = (uint16_t)(window_size >> 1),
        .num_diffs = num_diffs,
        .vlt_luma = vlt_luma,
    };
    const CambiCValuesKernels k = {
        .inc = cambi_increment_range_avx512,
        .dec = cambi_decrement_range_avx512,
        .row = calculate_c_values_row_avx512,
        .scan_row = scan_row_avx512,
        .scan_slide = scan_slide_avx512,
    };
    cambi_calculate_c_values_frame(&f, k);
}
