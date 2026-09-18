/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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
#include <arm_neon.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libvmaf/picture.h"
#include "cambi_neon.h"
#include "cambi.h"
#include "cambi_c_values_frame.h"

/* Histogram range updates for the c-values driver, in plain C on purpose: GCC
 * and Clang vectorize these loops into the same eight-lane NEON adds (plus a
 * four-lane step and a scalar tail) that a hand-written kernel spells out, and
 * the measured instruction count of the whole driver was no lower with
 * intrinsics (Research-2065). Same modular uint16 arithmetic as
 * increment_range / decrement_range in cambi.c. */
static void cambi_increment_range_c(uint16_t *arr, int left, int right)
{
    for (int i = left; i < right; i++) {
        arr[i]++;
    }
}

static void cambi_decrement_range_c(uint16_t *arr, int left, int right)
{
    for (int i = left; i < right; i++) {
        arr[i]--;
    }
}

/* 1 where the pixel equals its right neighbour and the pixel below, else 0;
 * vceqq gives 0xFFFF, the shift turns that into 1. */
static inline uint16x8_t zero_derivative_neon(const uint16_t *px, const uint16_t *below)
{
    const uint16x8_t v = vld1q_u16(px);
    const uint16x8_t eq =
        vandq_u16(vceqq_u16(v, vld1q_u16(px + 1)), vceqq_u16(v, vld1q_u16(below)));
    return vshrq_n_u16(eq, 15);
}

void get_derivative_data_for_row_neon(const uint16_t *image_data, uint16_t *derivative_buffer,
                                      int width, int height, int row, int stride)
{
    const uint16_t *px = &image_data[(ptrdiff_t)row * stride];
    /* The last row compares with itself vertically, which is always equal:
     * the scalar's `row == height - 1 ||` short cut. */
    const uint16_t *below = (row == height - 1) ? px : &px[stride];
    int col = 0;
    /* Reads px[col + 8]: the vector loop stops one column early, and the last
     * column (no right neighbour) is the scalar's `col == width - 1` case. */
    for (; col + 8 < width; col += 8) {
        vst1q_u16(&derivative_buffer[col], zero_derivative_neon(&px[col], &below[col]));
    }
    for (; col < width; col++) {
        const bool horizontal = (col == width - 1) || (px[col] == px[col + 1]);
        derivative_buffer[col] = (uint16_t)(horizontal && (px[col] == below[col]));
    }
}

/* Row-invariant inputs of the per-pixel c-value, bundled so the helper below
 * keeps a short parameter list. */
typedef struct {
    const uint16_t *histograms;
    const uint16_t *tvi_thresholds;
    const int *diff_weights;
    const int *all_diffs;
    const float *reciprocal_lut;
    int width;
    uint16_t num_diffs;
    uint16_t vlt_luma;
    uint16_t v_band_base;
    uint16_t v_band_size;
} CambiCValueRowNeon;

/* The scalar per-pixel c-value, verbatim in operation order (ADR-0452). */
static inline float c_value_pixel_neon(const CambiCValueRowNeon *r, uint16_t mask_value,
                                       uint16_t pixel, int col)
{
    if (!mask_value)
        return 0.0f;
    const uint16_t value = (uint16_t)(pixel + r->num_diffs);
    const int compact_v_signed = (int)pixel - (int)r->v_band_base;
    if ((unsigned)compact_v_signed >= r->v_band_size)
        return 0.0f;
    const ptrdiff_t width = r->width;
    const uint16_t p_0 = r->histograms[(ptrdiff_t)compact_v_signed * width + col];
    float c_v = 0.0f;
    for (int d = 0; d < r->num_diffs; d++) {
        const int diff_up = r->all_diffs[r->num_diffs + d + 1];
        if ((value > r->tvi_thresholds[d]) || ((value + diff_up) <= r->vlt_luma))
            continue;
        const int idx1 = compact_v_signed + diff_up;
        const int idx2 = compact_v_signed + r->all_diffs[r->num_diffs - d - 1];
        const uint16_t p_1 = r->histograms[(ptrdiff_t)idx1 * width + col];
        const uint16_t p_2 = (idx2 >= 0) ? r->histograms[(ptrdiff_t)idx2 * width + col] : 0;
        const uint16_t p_max = (p_1 > p_2) ? p_1 : p_2;
        const float val =
            (float)(r->diff_weights[d] * p_0 * p_max) * r->reciprocal_lut[p_max + p_0];
        if (val > c_v)
            c_v = val;
    }
    return c_v;
}

/*
 * calculate_c_values_row_neon — NEON-assisted port of calculate_c_values_row.
 *
 * CAMBI uses scatter/gather on the histogram array (non-contiguous uint16
 * reads indexed by per-pixel luma values).  NEON provides no gather
 * instruction, so the inner loop body is scalar.  The NEON contribution is:
 *
 *   1. A vectorised zero-mask scan: load 8 mask values at a time with
 *      vld1q_u16 and test with vmaxvq_u16.  If the max is zero the entire
 *      8-pixel block is skipped without entering the per-pixel c_value loop.
 *      On typical content where large flat regions have mask == 0 this
 *      eliminates the majority of inner-loop iterations.
 *
 *   2. The per-active-pixel inner loop is the scalar reference verbatim,
 *      which guarantees bit-identical output.  (No float reduction tree,
 *      no lane-widening accumulation — ADR-0452 / ADR-0138/0139 contract.)
 *
 * A pure-scalar loop over the same logic exists in cambi.c as
 * calculate_c_values_row; this function must remain numerically identical
 * to that scalar path for every active pixel.
 */
void calculate_c_values_row_neon(float *c_values, const uint16_t *histograms, const uint16_t *image,
                                 const uint16_t *mask, int row, int width, ptrdiff_t stride,
                                 const uint16_t num_diffs, const uint16_t *tvi_thresholds,
                                 uint16_t vlt_luma, const int *diff_weights, const int *all_diffs,
                                 const float *reciprocal_lut)
{
    int v_lo_signed_sc = (int)vlt_luma - 3 * (int)num_diffs + 1;
    uint16_t v_band_base = v_lo_signed_sc > 0 ? (uint16_t)v_lo_signed_sc : 0;
    uint16_t v_band_size = tvi_thresholds[num_diffs - 1] + 1 - v_band_base;
    const CambiCValueRowNeon r = {
        .histograms = histograms,
        .tvi_thresholds = tvi_thresholds,
        .diff_weights = diff_weights,
        .all_diffs = all_diffs,
        .reciprocal_lut = reciprocal_lut,
        .width = width,
        .num_diffs = num_diffs,
        .vlt_luma = vlt_luma,
        .v_band_base = v_band_base,
        .v_band_size = v_band_size,
    };

    const uint16_t *image_row = &image[row * stride];
    const uint16_t *mask_row = &mask[row * stride];
    float *c_row = &c_values[(ptrdiff_t)row * width];

    int col = 0;
    /* Fast-skip 8 columns at a time when all masks are zero. */
    for (; col + 8 <= width; col += 8) {
        uint16x8_t mv = vld1q_u16(&mask_row[col]);
        if (vmaxvq_u16(mv) == 0) {
            /* All 8 lanes masked out — write zeros and move on. */
            float32x4_t z = vdupq_n_f32(0.0f);
            vst1q_f32(&c_row[col], z);
            vst1q_f32(&c_row[col + 4], z);
            continue;
        }
        /* At least one active lane: process each pixel individually via the
         * scalar reference to guarantee bit-identical output. */
        for (int k = col; k < col + 8; k++) {
            c_row[k] = c_value_pixel_neon(&r, mask_row[k], image_row[k], k);
        }
    }

    /* Scalar tail for remaining columns. */
    for (; col < width; col++) {
        c_row[col] = c_value_pixel_neon(&r, mask_row[col], image_row[col], col);
    }
}

/*
 * Spatial-mask row kernels: NEON twins of compute_dp_row_avx2 /
 * compute_mask_row_avx2 (adapted from upstream Netflix/vmaf 86da14d03).
 * Integer-only and bit-exact against the scalar compute_dp_row /
 * compute_mask_row in cambi.c for every input.
 */

/* Inclusive prefix sum of the four uint32 lanes (modular, like the scalar).
 * vextq_u32 against zero shifts the vector up by k lanes. */
static inline uint32x4_t inclusive_prefix_u32_neon(uint32x4_t x)
{
    const uint32x4_t zero = vdupq_n_u32(0);
    x = vaddq_u32(x, vextq_u32(zero, x, 3));
    x = vaddq_u32(x, vextq_u32(zero, x, 2));
    return x;
}

void compute_dp_row_neon(uint32_t *dp_curr, const uint32_t *dp_prev, const uint16_t *deriv,
                         int width, int pad_size, bool deriv_valid)
{
    const int dp_offset = pad_size + 1;
    const int actual_width = deriv_valid ? width : 0;
    uint32x4_t carry = vdupq_n_u32(0);
    int j = 0;
    for (; j + 8 <= actual_width; j += 8) {
        const uint16x8_t d = vld1q_u16(&deriv[j]);
        /* Eight-lane prefix: the high half also gets the low half's total. */
        const uint32x4_t lo = inclusive_prefix_u32_neon(vmovl_u16(vget_low_u16(d)));
        const uint32x4_t hi =
            vaddq_u32(inclusive_prefix_u32_neon(vmovl_high_u16(d)), vdupq_laneq_u32(lo, 3));
        uint32_t *out = &dp_curr[dp_offset + j];
        const uint32_t *prev = &dp_prev[dp_offset + j];
        vst1q_u32(out, vaddq_u32(vld1q_u32(prev), vaddq_u32(lo, carry)));
        vst1q_u32(out + 4, vaddq_u32(vld1q_u32(prev + 4), vaddq_u32(hi, carry)));
        /* Only this add is loop-carried; the block total does not wait on carry. */
        carry = vaddq_u32(carry, vdupq_laneq_u32(hi, 3));
    }
    uint32_t prefix = vgetq_lane_u32(carry, 0);
    for (; j < actual_width; j++) {
        prefix += deriv[j];
        dp_curr[dp_offset + j] = dp_prev[dp_offset + j] + prefix;
    }
    const int n = width + pad_size;
    for (; j < n; j++) {
        dp_curr[dp_offset + j] = dp_prev[dp_offset + j] + prefix;
    }
}

/* Four-lane box sum: dp_bottom[j + delta] + dp_top[j] - dp_bottom[j] - dp_top[j + delta]. */
static inline uint32x4_t box_sum_u32_neon(const uint32_t *dp_bottom, const uint32_t *dp_top, int j,
                                          int delta)
{
    const uint32x4_t bd = vld1q_u32(&dp_bottom[j + delta]);
    const uint32x4_t t = vld1q_u32(&dp_top[j]);
    const uint32x4_t b = vld1q_u32(&dp_bottom[j]);
    const uint32x4_t td = vld1q_u32(&dp_top[j + delta]);
    return vsubq_u32(vaddq_u32(bd, t), vaddq_u32(b, td));
}

/* Not dispatched: GCC and Clang already auto-vectorize the scalar
 * compute_mask_row into this instruction sequence (cmhi + uzp1, eight columns
 * per iteration), so it would not reduce the op count. It is kept as the NEON
 * twin and stays under the parity test. */
void compute_mask_row_neon(uint16_t *mask_row, const uint32_t *dp_bottom, const uint32_t *dp_top,
                           int width, int pad_size, uint32_t mask_index)
{
    const int delta = 2 * pad_size + 1;
    const uint32x4_t midx = vdupq_n_u32(mask_index);
    const uint16x8_t one = vdupq_n_u16(1);
    int j = 0;
    for (; j + 8 <= width; j += 8) {
        /* vcgtq_u32 is the unsigned compare the scalar performs. */
        const uint32x4_t gt_lo = vcgtq_u32(box_sum_u32_neon(dp_bottom, dp_top, j, delta), midx);
        const uint32x4_t gt_hi = vcgtq_u32(box_sum_u32_neon(dp_bottom, dp_top, j + 4, delta), midx);
        const uint16x8_t gt = vcombine_u16(vmovn_u32(gt_lo), vmovn_u32(gt_hi));
        vst1q_u16(&mask_row[j], vandq_u16(gt, one));
    }
    for (; j < width; j++) {
        const uint32_t result = dp_bottom[j + delta] + dp_top[j] - dp_bottom[j] - dp_top[j + delta];
        mask_row[j] = (uint16_t)(result > mask_index);
    }
}

/*
 * Preprocessing and per-scale kernels: NEON twins of decimate_avx2,
 * anti_dithering_filter_avx2 and filter_mode_avx2. All three are integer-only
 * and bit-exact against the scalar decimate / anti_dithering_filter /
 * filter_mode in cambi.c for every uint16 input.
 */

/* dst[i, j] = src[2i, 2j] in place: ld2 de-interleaves sixteen elements and the
 * even half is the output. Reads stay ahead of writes within a row and across
 * rows, as in the scalar loop; the vector loop reads up to src[2 * width - 1],
 * the same bound as decimate_avx2. */
static void decimate_row_neon(const uint16_t *src, uint16_t *dst, unsigned width)
{
    unsigned j = 0;
    for (; j + 8 <= width; j += 8) {
        vst1q_u16(&dst[j], vld2q_u16(&src[(size_t)2 * j]).val[0]);
    }
    for (; j < width; j++) {
        dst[j] = src[(size_t)2 * j];
    }
}

void decimate_neon(VmafPicture *image, unsigned width, unsigned height)
{
    assert(image->data[0]);
    assert(width > 0u && height > 0u);
    uint16_t *data = image->data[0];
    const ptrdiff_t stride = image->stride[0] >> 1;
    for (unsigned i = 0; i < height; i++) {
        decimate_row_neon(&data[(ptrdiff_t)2 * (ptrdiff_t)i * stride], &data[(ptrdiff_t)i * stride],
                          width);
    }
}

/* floor((a + b + c + d) / 4) for eight columns of a 2x2 window: widening
 * adds, so the sum is exact for any uint16 input, and the narrowing shift
 * cannot truncate because the quotient is at most 65535. */
static inline uint16x8_t box_average_2x2_neon(const uint16_t *row0, const uint16_t *row1)
{
    const uint16x8_t a = vld1q_u16(row0);
    const uint16x8_t b = vld1q_u16(row0 + 1);
    const uint16x8_t c = vld1q_u16(row1);
    const uint16x8_t d = vld1q_u16(row1 + 1);
    const uint32x4_t lo = vaddq_u32(vaddl_u16(vget_low_u16(a), vget_low_u16(b)),
                                    vaddl_u16(vget_low_u16(c), vget_low_u16(d)));
    const uint32x4_t hi = vaddq_u32(vaddl_high_u16(a, b), vaddl_high_u16(c, d));
    return vshrn_high_n_u32(vshrn_n_u32(lo, 2), hi, 2);
}

void anti_dithering_filter_neon(VmafPicture *pic, unsigned width, unsigned height)
{
    assert(pic->data[0]);
    assert(width > 0u && height > 0u);
    uint16_t *data = pic->data[0];
    const ptrdiff_t stride = pic->stride[0] >> 1;

    for (unsigned i = 0; i + 1 < height; i++) {
        uint16_t *row0 = &data[(ptrdiff_t)i * stride];
        const uint16_t *row1 = &data[(ptrdiff_t)(i + 1) * stride];
        unsigned j = 0;
        /* j + 8 < width keeps the row0[j + 8] / row1[j + 8] loads inside the row;
         * row0[j + 8] is read before the next block overwrites it. */
        for (; j + 8 < width; j += 8) {
            vst1q_u16(&row0[j], box_average_2x2_neon(&row0[j], &row1[j]));
        }
        for (; j + 1 < width; j++) {
            row0[j] = (uint16_t)((row0[j] + row0[j + 1] + row1[j] + row1[j + 1]) >> 2);
        }
        row0[width - 1] = (uint16_t)((row0[width - 1] + row1[width - 1]) >> 1);
    }
    /* Last row: floor((a + b) / 2) with its right neighbour; vhadd is exactly
     * that, without overflow. */
    uint16_t *last_row = &data[(ptrdiff_t)(height - 1) * stride];
    unsigned j = 0;
    for (; j + 8 < width; j += 8) {
        vst1q_u16(&last_row[j], vhaddq_u16(vld1q_u16(&last_row[j]), vld1q_u16(&last_row[j + 1])));
    }
    for (; j + 1 < width; j++) {
        last_row[j] = (uint16_t)((last_row[j] + last_row[j + 1]) >> 1);
    }
}

/* The duplicate among (a, b, c) if any pair matches, otherwise the unsigned
 * minimum: the scalar mode3() in cambi.c. */
static inline uint16x8_t mode3_neon(uint16x8_t a, uint16x8_t b, uint16x8_t c)
{
    const uint16x8_t a_dup = vorrq_u16(vceqq_u16(a, b), vceqq_u16(a, c));
    const uint16x8_t min_abc = vminq_u16(vminq_u16(a, b), c);
    return vbslq_u16(a_dup, a, vbslq_u16(vceqq_u16(b, c), b, min_abc));
}

static inline uint16_t mode3_scalar_neon(uint16_t a, uint16_t b, uint16_t c)
{
    if (a == b || a == c)
        return a;
    if (b == c)
        return b;
    const uint16_t ab = a < b ? a : b;
    return ab < c ? ab : c;
}

/* Horizontal pass of one row into buf: mode3 of each interior pixel and its two
 * neighbours; the first and last columns are copied. */
static void filter_mode_row_neon(const uint16_t *row, uint16_t *buf, int width)
{
    buf[0] = row[0];
    int j = 1;
    /* Writes buf[j .. j + 7] and reads row[j + 8]: both need j + 8 <= width - 1
     * (the last mode3 column is width - 2). */
    for (; j + 8 < width; j += 8) {
        const uint16x8_t a = vld1q_u16(&row[j - 1]);
        const uint16x8_t b = vld1q_u16(&row[j]);
        const uint16x8_t c = vld1q_u16(&row[j + 1]);
        vst1q_u16(&buf[j], mode3_neon(a, b, c));
    }
    for (; j < width - 1; j++) {
        buf[j] = mode3_scalar_neon(row[j - 1], row[j], row[j + 1]);
    }
    buf[width - 1] = row[width - 1];
}

/* Vertical pass: out = mode3 of the three buffered rows, column by column. */
static void filter_mode_column_neon(const uint16_t *buffer, uint16_t *out, int width)
{
    const uint16_t *b0 = buffer;
    const uint16_t *b1 = &buffer[width];
    const uint16_t *b2 = &buffer[(ptrdiff_t)2 * width];
    int j = 0;
    for (; j + 8 <= width; j += 8) {
        vst1q_u16(&out[j], mode3_neon(vld1q_u16(&b0[j]), vld1q_u16(&b1[j]), vld1q_u16(&b2[j])));
    }
    for (; j < width; j++) {
        out[j] = mode3_scalar_neon(b0[j], b1[j], b2[j]);
    }
}

void filter_mode_neon(const VmafPicture *image, int width, int height, uint16_t *buffer)
{
    assert(image->data[0]);
    assert(width > 0 && height > 0 && buffer);
    uint16_t *data = image->data[0];
    const ptrdiff_t stride = image->stride[0] >> 1;
    int curr_line = 0;
    for (int i = 0; i < height; i++) {
        filter_mode_row_neon(&data[(ptrdiff_t)i * stride], &buffer[(ptrdiff_t)curr_line * width],
                             width);
        if (i > 1) {
            filter_mode_column_neon(buffer, &data[(ptrdiff_t)(i - 1) * stride], width);
        }
        curr_line = (curr_line + 1 == 3 ? 0 : curr_line + 1);
    }
}

/*
 * Frame-level c-values driver: the shared calculate_c_values walk
 * (cambi_c_values_frame.h) with the range updaters and row kernel above, and
 * column scans that test eight pixels per compare for "this column needs a
 * histogram update". Histogram updates are integer and commute per cell, and
 * the row kernel is the scalar per-pixel code, so the c-values equal the scalar
 * calculate_c_values output byte for byte.
 */

/* One bit per lane of an all-ones / all-zeros uint16x8 condition. */
static inline uint32_t lane_bits_neon(uint16x8_t cond)
{
    static const uint16_t weights[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    return vaddvq_u16(vandq_u16(cond, vld1q_u16(weights)));
}

/* Unmasked and in the scored band [base, base + size), eight lanes; *value
 * gets the pixels. */
static inline uint16x8_t in_band_neon(const CambiCValuesFrame *f, ptrdiff_t at, uint16x8_t base,
                                      uint16x8_t size, uint16x8_t *value)
{
    const uint16x8_t m = vld1q_u16(&f->mask[at]);
    const uint16x8_t v = vld1q_u16(&f->image[at]);
    *value = v;
    return vandq_u16(vtstq_u16(m, m), vcltq_u16(vsubq_u16(v, base), size));
}

/* One mask of up to 32 columns starting at `at`. */
static inline uint32_t scan_row_mask_neon(const CambiCValuesFrame *f, ptrdiff_t at, int n,
                                          uint16x8_t base, uint16x8_t size)
{
    uint32_t flags = 0;
    int b = 0;
    for (; b + 8 <= n; b += 8) {
        uint16x8_t v;
        flags |= lane_bits_neon(in_band_neon(f, at + b, base, size, &v)) << b;
    }
    for (; b < n; b++) {
        flags |= (uint32_t)cambi_column_in_band(f, at + b) << b;
    }
    return flags;
}

static void scan_row_neon(const CambiCValuesFrame *f, int row, int j0, int n, uint32_t *masks)
{
    const uint16x8_t base = vdupq_n_u16(f->v_band_base);
    const uint16x8_t size = vdupq_n_u16(f->v_band_size);
    const ptrdiff_t at = (ptrdiff_t)row * f->stride + j0;
    for (int b = 0; b < n; b += 32) {
        masks[b / 32] = scan_row_mask_neon(f, at + b, MIN(32, n - b), base, size);
    }
}

/* One slide mask of up to 32 columns. */
static inline uint32_t scan_slide_mask_neon(const CambiCValuesFrame *f, ptrdiff_t at_sub,
                                            ptrdiff_t at_add, int n, uint16x8_t base,
                                            uint16x8_t size)
{
    uint32_t flags = 0;
    int b = 0;
    for (; b + 8 <= n; b += 8) {
        uint16x8_t v_sub;
        uint16x8_t v_add;
        const uint16x8_t sub_in = in_band_neon(f, at_sub + b, base, size, &v_sub);
        const uint16x8_t add_in = in_band_neon(f, at_add + b, base, size, &v_add);
        const uint16x8_t cancel = vandq_u16(vandq_u16(sub_in, add_in), vceqq_u16(v_sub, v_add));
        flags |= lane_bits_neon(vbicq_u16(vorrq_u16(sub_in, add_in), cancel)) << b;
    }
    for (; b < n; b++) {
        flags |= (uint32_t)cambi_column_slide_needed(f, at_sub + b, at_add + b) << b;
    }
    return flags;
}

static void scan_slide_neon(const CambiCValuesFrame *f, int row_sub, int row_add, int j0, int n,
                            uint32_t *masks)
{
    const uint16x8_t base = vdupq_n_u16(f->v_band_base);
    const uint16x8_t size = vdupq_n_u16(f->v_band_size);
    const ptrdiff_t at_sub = (ptrdiff_t)row_sub * f->stride + j0;
    const ptrdiff_t at_add = (ptrdiff_t)row_add * f->stride + j0;
    for (int b = 0; b < n; b += 32) {
        masks[b / 32] = scan_slide_mask_neon(f, at_sub + b, at_add + b, MIN(32, n - b), base, size);
    }
}

void calculate_c_values_neon(VmafPicture *pic, const VmafPicture *mask_pic, float *c_values,
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
        .inc = cambi_increment_range_c,
        .dec = cambi_decrement_range_c,
        .row = calculate_c_values_row_neon,
        .scan_row = scan_row_neon,
        .scan_slide = scan_slide_neon,
    };
    cambi_calculate_c_values_frame(&f, k);
}
