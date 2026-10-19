/**
 *
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

#include "feature/arm64/adm_neon.h"
#include "feature/integer_adm.h"
#include "feature/integer_adm_kernels.h"

#include <arm_neon.h>

/* Four-tap multiply-accumulate of eight int16 lanes, widened to two int32x4
 * halves: acc = init + f[0] * v[0] + f[1] * v[1] + f[2] * v[2] + f[3] * v[3].
 * The vertical pass feeds it four source rows, the horizontal pass the two
 * de-interleaved halves of two vld2q loads. Integer arithmetic throughout, so
 * the association order cannot perturb the result. */
typedef struct AdmNeonAccum {
    int32x4_t lo;
    int32x4_t hi;
} AdmNeonAccum;

static inline AdmNeonAccum adm_neon_macc4(int32x4_t init, const int16x8_t v[4], int16x4_t filter)
{
    AdmNeonAccum acc;
    acc.lo = vmlal_lane_s16(init, vget_low_s16(v[0]), filter, 0);
    acc.hi = vmlal_high_lane_s16(init, v[0], filter, 0);
    acc.lo = vmlal_lane_s16(acc.lo, vget_low_s16(v[1]), filter, 1);
    acc.hi = vmlal_high_lane_s16(acc.hi, v[1], filter, 1);
    acc.lo = vmlal_lane_s16(acc.lo, vget_low_s16(v[2]), filter, 2);
    acc.hi = vmlal_high_lane_s16(acc.hi, v[2], filter, 2);
    acc.lo = vmlal_lane_s16(acc.lo, vget_low_s16(v[3]), filter, 3);
    acc.hi = vmlal_high_lane_s16(acc.hi, v[3], filter, 3);
    return acc;
}

/* Shift both halves (a negative `shift` is a right shift), narrow them back to
 * int16 by keeping the low half of every lane, and store eight samples. */
static inline void adm_neon_store_shifted(int16_t *out, AdmNeonAccum acc, int32x4_t shift)
{
    const int16x8_t narrowed = vuzp1q_s16(vreinterpretq_s16_s32(vshlq_s32(acc.lo, shift)),
                                          vreinterpretq_s16_s32(vshlq_s32(acc.hi, shift)));
    vst1q_s16(out, narrowed);
}

enum {
    ADM_DWT2_8_SHIFT_VP = 8,
    ADM_DWT2_8_ADD_SHIFT_VP = 128,
    ADM_DWT2_8_SHIFT_HP = 16,
    ADM_DWT2_8_ADD_SHIFT_HP = 32768,
};

/* One vertical-pass column through the scalar arithmetic, for the columns the
 * 16-wide loop cannot reach. */
static void adm_dwt2_8_neon_vpass_column(const uint8_t *const rows[4], int j, int16_t *tmplo,
                                         int16_t *tmphi)
{
    int32_t accum_lo = 0;
    int32_t accum_hi = 0;

    for (int tap = 0; tap < 4; tap++) {
        const int32_t sample = (int32_t)(uint16_t)rows[tap][j];
        accum_lo += (int32_t)dwt2_db2_coeffs_lo[tap] * sample;
        accum_hi += (int32_t)dwt2_db2_coeffs_hi[tap] * sample;
    }
    accum_lo -= (int32_t)dwt2_db2_coeffs_lo_sum * ADM_DWT2_8_ADD_SHIFT_VP;
    accum_hi -= (int32_t)dwt2_db2_coeffs_hi_sum * ADM_DWT2_8_ADD_SHIFT_VP;
    tmplo[j] = (int16_t)((accum_lo + ADM_DWT2_8_ADD_SHIFT_VP) >> ADM_DWT2_8_SHIFT_VP);
    tmphi[j] = (int16_t)((accum_hi + ADM_DWT2_8_ADD_SHIFT_VP) >> ADM_DWT2_8_SHIFT_VP);
}

/* Vertical pass of one output row into tmplo/tmphi (w samples each). */
static void adm_dwt2_8_neon_vpass_row(const uint8_t *const rows[4], int w, int16_t *tmplo,
                                      int16_t *tmphi)
{
    const int16x4_t filter_lo_vec = vld1_s16(dwt2_db2_coeffs_lo);
    const int16x4_t filter_hi_vec = vld1_s16(dwt2_db2_coeffs_hi);
    const int32x4_t normalize_vec_vp_lo = vdupq_n_s32(
        (-1 * (int32_t)dwt2_db2_coeffs_lo_sum * ADM_DWT2_8_ADD_SHIFT_VP) + ADM_DWT2_8_ADD_SHIFT_VP);
    const int32x4_t normalize_vec_vp_hi = vdupq_n_s32(
        (-1 * (int32_t)dwt2_db2_coeffs_hi_sum * ADM_DWT2_8_ADD_SHIFT_VP) + ADM_DWT2_8_ADD_SHIFT_VP);
    const int32x4_t shift_vp_vec = vdupq_n_s32(-ADM_DWT2_8_SHIFT_VP);

    for (int j = 0; j < w - 15; j += 16) {
        int16x8_t s_16_l[4];
        int16x8_t s_16_h[4];

        for (int tap = 0; tap < 4; tap++) {
            const uint8x16_t u_8 = vld1q_u8(rows[tap] + j);
            s_16_l[tap] = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(u_8)));
            s_16_h[tap] = vreinterpretq_s16_u16(vmovl_high_u8(u_8));
        }

        adm_neon_store_shifted(
            tmplo + j, adm_neon_macc4(normalize_vec_vp_lo, s_16_l, filter_lo_vec), shift_vp_vec);
        adm_neon_store_shifted(tmplo + j + 8,
                               adm_neon_macc4(normalize_vec_vp_lo, s_16_h, filter_lo_vec),
                               shift_vp_vec);
        adm_neon_store_shifted(
            tmphi + j, adm_neon_macc4(normalize_vec_vp_hi, s_16_l, filter_hi_vec), shift_vp_vec);
        adm_neon_store_shifted(tmphi + j + 8,
                               adm_neon_macc4(normalize_vec_vp_hi, s_16_h, filter_hi_vec),
                               shift_vp_vec);
    }

    /* Scalar tail for the columns the 16-wide vertical loop cannot reach.
     *
     * The dispatcher in integer_adm.c admits this kernel on `!(w % 8)`, but
     * the loop above advances 16 at a time and stops at `w - 15`, so for a
     * width congruent to 8 mod 16 the final 8 columns of tmplo/tmphi were
     * never written. The horizontal pass then read whatever the previous
     * row had left there, producing garbage in the last output columns —
     * silently, because every Netflix golden fixture is 1280, 1920 or 576
     * pixels wide and all three are multiples of 16. */
    for (int j = (w / 16) * 16; j < w; ++j) {
        adm_dwt2_8_neon_vpass_column(rows, j, tmplo, tmphi);
    }
}

/* One horizontal output column of all four subbands, read through the ind_x
 * mirror table exactly like the scalar adm_dwt2_8(). Used for the mirrored
 * j == 0 column and for the tail the 8-wide loop leaves over. */
static void adm_dwt2_8_neon_hpass_column(const int16_t *tmplo, const int16_t *tmphi,
                                         int *const ind_x[4], const adm_dwt_band_t *dst,
                                         int row_offset, int j)
{
    int32_t accum_a = ADM_DWT2_8_ADD_SHIFT_HP;
    int32_t accum_v = ADM_DWT2_8_ADD_SHIFT_HP;
    int32_t accum_h = ADM_DWT2_8_ADD_SHIFT_HP;
    int32_t accum_d = ADM_DWT2_8_ADD_SHIFT_HP;

    for (int tap = 0; tap < 4; tap++) {
        const int column = ind_x[tap][j];
        const int16_t s_lo = tmplo[column];
        const int16_t s_hi = tmphi[column];
        accum_a += (int32_t)dwt2_db2_coeffs_lo[tap] * s_lo;
        accum_v += (int32_t)dwt2_db2_coeffs_hi[tap] * s_lo;
        accum_h += (int32_t)dwt2_db2_coeffs_lo[tap] * s_hi;
        accum_d += (int32_t)dwt2_db2_coeffs_hi[tap] * s_hi;
    }

    dst->band_a[row_offset + j] = (int16_t)(accum_a >> ADM_DWT2_8_SHIFT_HP);
    dst->band_v[row_offset + j] = (int16_t)(accum_v >> ADM_DWT2_8_SHIFT_HP);
    dst->band_h[row_offset + j] = (int16_t)(accum_h >> ADM_DWT2_8_SHIFT_HP);
    dst->band_d[row_offset + j] = (int16_t)(accum_d >> ADM_DWT2_8_SHIFT_HP);
}

/* Horizontal pass of one output row. The 8-wide loop writes columns j..j+7
 * and reads taps up to 2 * (j + 7) + 2 without consulting ind_x, so its last
 * column must stay at or below half_w - 2 (the last column whose taps need no
 * mirror) and it must never store past half_w - 1. Column 0 and everything
 * from half_w_mod8 on go through ind_x, which applies the mirror. */
static void adm_dwt2_8_neon_hpass_row(const int16_t *tmplo, const int16_t *tmphi,
                                      int *const ind_x[4], const adm_dwt_band_t *dst,
                                      int row_offset, int half_w)
{
    const int half_w_mod8 = half_w >= 2 ? half_w - 1 - ((half_w - 2) % 8) : 1;
    const int16x4_t filter_lo_vec = vld1_s16(dwt2_db2_coeffs_lo);
    const int16x4_t filter_hi_vec = vld1_s16(dwt2_db2_coeffs_hi);
    const int32x4_t add_shift_hp_vec = vdupq_n_s32(ADM_DWT2_8_ADD_SHIFT_HP);
    const int32x4_t shift_hp_vec = vdupq_n_s32(-ADM_DWT2_8_SHIFT_HP);

    /* j = 0 is a special case: src_ind_x[k][0] is the mirrored {1, 0, 1, 2}
     * rather than {-1, 0, 1, 2}. */
    adm_dwt2_8_neon_hpass_column(tmplo, tmphi, ind_x, dst, row_offset, 0);

    /* The kernel only runs for even w (the dispatcher requires !(w % 8)), so
     * between column 1 and half_w_mod8 the taps of column j are simply
     * 2j - 1, 2j, 2j + 1 and 2j + 2 and ind_x can be ignored. */
    for (int j = 1; j < half_w_mod8; j += 8) {
        const int16_t *p_low = tmplo + ((ptrdiff_t)2 * j);
        const int16_t *p_high = tmphi + ((ptrdiff_t)2 * j);
        const int16x8x2_t low_s0s1 = vld2q_s16(p_low - 1);
        const int16x8x2_t low_s2s3 = vld2q_s16(p_low + 1);
        const int16x8x2_t high_s0s1 = vld2q_s16(p_high - 1);
        const int16x8x2_t high_s2s3 = vld2q_s16(p_high + 1);
        /* De-interleaved: val[0] of each load holds the odd-indexed samples
         * (taps 0 and 2), val[1] the even-indexed ones (taps 1 and 3). */
        const int16x8_t low_taps[4] = {low_s0s1.val[0], low_s0s1.val[1], low_s2s3.val[0],
                                       low_s2s3.val[1]};
        const int16x8_t high_taps[4] = {high_s0s1.val[0], high_s0s1.val[1], high_s2s3.val[0],
                                        high_s2s3.val[1]};
        const ptrdiff_t out = (ptrdiff_t)row_offset + j;

        adm_neon_store_shifted(dst->band_a + out,
                               adm_neon_macc4(add_shift_hp_vec, low_taps, filter_lo_vec),
                               shift_hp_vec);
        adm_neon_store_shifted(dst->band_v + out,
                               adm_neon_macc4(add_shift_hp_vec, low_taps, filter_hi_vec),
                               shift_hp_vec);
        adm_neon_store_shifted(dst->band_h + out,
                               adm_neon_macc4(add_shift_hp_vec, high_taps, filter_lo_vec),
                               shift_hp_vec);
        adm_neon_store_shifted(dst->band_d + out,
                               adm_neon_macc4(add_shift_hp_vec, high_taps, filter_hi_vec),
                               shift_hp_vec);
    }

    /* Scalar tail through ind_x: the columns the 8-wide loop must not reach,
     * including the last one, whose taps mirror back into range. Same guarded
     * bound as the x86 DWT2 kernels (Netflix/vmaf ea012e387). */
    for (int j = half_w_mod8; j < half_w; ++j) {
        adm_dwt2_8_neon_hpass_column(tmplo, tmphi, ind_x, dst, row_offset, j);
    }
}

void adm_dwt2_8_neon(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w, int h,
                     int src_stride, int dst_stride)
{
    int **ind_y = buf->ind_y;
    int *const *ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;
    const int half_w = (w + 1) / 2;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        const uint8_t *const rows[4] = {
            src + ((ptrdiff_t)ind_y[0][i] * src_stride),
            src + ((ptrdiff_t)ind_y[1][i] * src_stride),
            src + ((ptrdiff_t)ind_y[2][i] * src_stride),
            src + ((ptrdiff_t)ind_y[3][i] * src_stride),
        };
        adm_dwt2_8_neon_vpass_row(rows, w, tmplo, tmphi);
        adm_dwt2_8_neon_hpass_row(tmplo, tmphi, ind_x, dst, i * dst_stride, half_w);
    }
}

/*
 * Scale-zero decouple, four columns at a time (Netflix/vmaf 9e48141b, Dan
 * Trapp). The scalar reference is adm_decouple_cols() in
 * feature/integer_adm_kernels.h; this kernel returns its samples bit for bit.
 *
 * It takes the vector path for an integral enhancement gain limit only. The
 * scalar kernel stores MIN(rst * gain, t) as the double product truncated
 * toward zero (ADR-1413); for an integral gain that product is the int32
 * product (rst is within 2^15 and the limit within 100), so the vector lanes
 * form it in int32. A fractional limit takes the scalar kernel, which is the
 * definition of the truncated product.
 */

/* Dot product of two (h, v) pairs, rounded to float as the scalar angle test
 * rounds its int64 sums. The sums stay below 2^31, so the int64 -> double ->
 * float conversion rounds once. */
static inline float32x4_t adm_neon_dot_s16(int16x4_t ah, int16x4_t av, int16x4_t bh, int16x4_t bv)
{
    const int32x4_t h = vmull_s16(ah, bh);
    const int32x4_t v = vmull_s16(av, bv);
    const int64x2_t lo = vaddl_s32(vget_low_s32(h), vget_low_s32(v));
    const int64x2_t hi = vaddl_s32(vget_high_s32(h), vget_high_s32(v));
    return vcombine_f32(vcvt_f32_f64(vcvtq_f64_s64(lo)), vcvt_f32_f64(vcvtq_f64_s64(hi)));
}

/* The one-degree angle test of adm_angle_flag_fp64() on two lanes, in double:
 * dot >= 0 and dot^2 >= (cos^2 * |o|^2) * |t|^2, the products in that order. */
static inline uint32x2_t adm_neon_angle_f64(float32x2_t dot, float32x2_t omag, float32x2_t tmag,
                                            double cos_sq)
{
    const float64x2_t d = vmulq_n_f64(vcvt_f64_f32(dot), 1.0 / 4096.0);
    const float64x2_t o = vmulq_n_f64(vcvt_f64_f32(omag), 1.0 / 4096.0);
    const float64x2_t t = vmulq_n_f64(vcvt_f64_f32(tmag), 1.0 / 4096.0);
    const uint64x2_t non_negative = vcgeq_f64(d, vdupq_n_f64(0.0));
    const uint64x2_t inside = vcgeq_f64(vmulq_f64(d, d), vmulq_f64(vmulq_n_f64(o, cos_sq), t));
    return vmovn_u64(vandq_u64(non_negative, inside));
}

/* The angle flag of four columns: all lanes clear at gain 1, where the Q15
 * reconstruction already lies between zero and the distorted sample. */
static inline uint32x4_t adm_neon_angle4(const int16x4_t o[3], const int16x4_t t[3], double gain,
                                         double cos_sq)
{
    if (gain == 1.0) {
        return vdupq_n_u32(0);
    }
    const float32x4_t dot = adm_neon_dot_s16(o[0], o[1], t[0], t[1]);
    const float32x4_t omag = adm_neon_dot_s16(o[0], o[1], o[0], o[1]);
    const float32x4_t tmag = adm_neon_dot_s16(t[0], t[1], t[0], t[1]);
    return vcombine_u32(
        adm_neon_angle_f64(vget_low_f32(dot), vget_low_f32(omag), vget_low_f32(tmag), cos_sq),
        adm_neon_angle_f64(vget_high_f32(dot), vget_high_f32(omag), vget_high_f32(tmag), cos_sq));
}

/* adm_decouple_band() on four columns of one band. `ref` is the reference
 * band at the first column: its samples index the reciprocal table. */
static inline int16x4_t adm_neon_decouple_band(const int16_t *ref, int16x4_t o, int16x4_t t,
                                               uint32x4_t angle, int gain, const int32_t *lookup)
{
    const int32_t div[4] = {lookup[ref[0] + 32768], lookup[ref[1] + 32768], lookup[ref[2] + 32768],
                            lookup[ref[3] + 32768]};
    const int32x4_t recip = vld1q_s32(div);
    const int32x4_t dis = vmovl_s16(t);
    const int32x4_t orig = vmovl_s16(o);
    const int32x4_t zero = vdupq_n_s32(0);
    const int32x4_t ratio =
        vcombine_s32(vrshrn_n_s64(vmull_s32(vget_low_s32(recip), vget_low_s32(dis)), 15),
                     vrshrn_n_s64(vmull_s32(vget_high_s32(recip), vget_high_s32(dis)), 15));
    const int32x4_t k = vbslq_s32(vceqq_s32(orig, zero), vdupq_n_s32(32768),
                                  vmaxq_s32(zero, vminq_s32(ratio, vdupq_n_s32(32768))));
    const int32x4_t rst = vrshrq_n_s32(vmulq_s32(k, orig), 15);
    if (!vmaxvq_u32(angle)) {
        return vmovn_s32(rst);
    }
    const int32x4_t scaled = vmulq_n_s32(rst, gain);
    const uint32x4_t active = vandq_u32(angle, vcgtq_s32(k, zero));
    const int32x4_t negative =
        vbslq_s32(vandq_u32(active, vcltq_s32(orig, zero)), vmaxq_s32(scaled, dis), rst);
    return vmovn_s32(
        vbslq_s32(vandq_u32(active, vcgtq_s32(orig, zero)), vminq_s32(scaled, dis), negative));
}

/* Decouple the four columns starting at `off` and store both outputs. */
static inline void adm_neon_decouple4(AdmBuffer *buf, int off, int gain, double cos_sq,
                                      const int32_t *lookup)
{
    const int16_t *const ref[3] = {buf->ref_dwt2.band_h + off, buf->ref_dwt2.band_v + off,
                                   buf->ref_dwt2.band_d + off};
    const int16_t *const dis[3] = {buf->dis_dwt2.band_h + off, buf->dis_dwt2.band_v + off,
                                   buf->dis_dwt2.band_d + off};
    int16_t *const rst[3] = {buf->decouple_r.band_h + off, buf->decouple_r.band_v + off,
                             buf->decouple_r.band_d + off};
    int16_t *const add[3] = {buf->decouple_a.band_h + off, buf->decouple_a.band_v + off,
                             buf->decouple_a.band_d + off};
    int16x4_t o[3];
    int16x4_t t[3];

    for (int b = 0; b < 3; ++b) {
        o[b] = vld1_s16(ref[b]);
        t[b] = vld1_s16(dis[b]);
    }
    const uint32x4_t angle = adm_neon_angle4(o, t, (double)gain, cos_sq);
    for (int b = 0; b < 3; ++b) {
        const int16x4_t r = adm_neon_decouple_band(ref[b], o[b], t[b], angle, gain, lookup);
        vst1_s16(rst[b], r);
        vst1_s16(add[b], vsub_s16(t[b], r));
    }
}

void adm_decouple_neon(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                       int32_t *adm_div_lookup)
{
    const float cos_1deg_sq = adm_cos_1deg_sq();
    const AdmBorder b = adm_border_filt(w, h);
    const int width = b.right - b.left;
    const int gain = (int)adm_enhn_gain_limit;

    if (width < 4 || adm_enhn_gain_limit != (double)gain) {
        for (int i = b.top; i < b.bottom; ++i) {
            adm_decouple_cols(buf, i, stride, b.left, b.right, adm_enhn_gain_limit, adm_div_lookup,
                              cos_1deg_sq);
        }
        return;
    }
    for (int i = b.top; i < b.bottom; ++i) {
        /* Every group of four starts inside the region; the last one is moved
         * back to end on the last column, which recomputes up to three columns
         * from unchanged inputs. */
        for (int j = b.left; j < b.right; j += 4) {
            const int j0 = (j > b.right - 4) ? b.right - 4 : j;
            adm_neon_decouple4(buf, (i * stride) + j0, gain, (double)cos_1deg_sq, adm_div_lookup);
        }
    }
}

/*
 * Contrast masking, four columns at a time (Netflix/vmaf 8bc5a5c6a for scale
 * 0, b41d2340a for scales 1 to 3, Dan Trapp). The scalar references are
 * adm_cm_accum_px() and i4_adm_cm_accum_px() in feature/integer_adm_kernels.h;
 * these kernels return their row sums bit for bit, and adm_cm_rows() /
 * i4_adm_cm_rows() fold each row once (ADR-1167).
 *
 * The fork's scalar arithmetic is not upstream's, so neither is the vector
 * one: the scale-0 centre tap stays int32 and the excess is formed in int64
 * and clamped to [0, INT32_MAX] (ADR-1402), where upstream narrows the tap
 * to int16 and subtracts the threshold in int32. Every other step is the
 * scalar's integer arithmetic lane by lane: products widened to int64, the
 * square narrowed to int32 by truncation (the scalar's (int32_t) cast), the
 * cube added with its rounding term and shifted arithmetically. A scale-0
 * row is summed in uint64 lanes (adm_cm_fold_s0()); a scale 1-3 row in int64
 * lanes. Edge rows and columns, rows narrower than one block and the columns
 * left over after the last block run the scalar sample.
 */

/* The 3x3 neighbourhood of the filtered band without its centre, plus the
 * centre tap computed by `tap`, for four columns: the per-band sum of
 * adm_cm_thresh() / i4_adm_cm_thresh(). */
static inline int32x4_t adm_neon_cm_ring_s16(const int16_t *flt, ptrdiff_t stride)
{
    int32x4_t sum = vmovl_s16(vld1_s16(flt - stride - 1));
    sum = vaddw_s16(sum, vld1_s16(flt - stride));
    sum = vaddw_s16(sum, vld1_s16(flt - stride + 1));
    sum = vaddw_s16(sum, vld1_s16(flt - 1));
    sum = vaddw_s16(sum, vld1_s16(flt + 1));
    sum = vaddw_s16(sum, vld1_s16(flt + stride - 1));
    sum = vaddw_s16(sum, vld1_s16(flt + stride));
    return vaddw_s16(sum, vld1_s16(flt + stride + 1));
}

/* adm_cm_thresh() of the four interior columns from `j` of row `i`. The centre
 * tap ((ONE_BY_15 * |a|) + 2048) >> 12 reaches 69904 and stays int32. */
static inline int32x4_t adm_neon_cm_thresh(const AdmCmCtx *c, int i, int j)
{
    const ptrdiff_t off = ((ptrdiff_t)i * c->csf_a_stride) + j;
    int32x4_t thr = vdupq_n_s32(0);

    for (int theta = 0; theta < 3; ++theta) {
        const int32x4_t mag = vabsq_s32(vmovl_s16(vld1_s16(c->angles[theta] + off)));
        const int32x4_t tap =
            vshrq_n_s32(vaddq_s32(vmulq_n_s32(mag, ONE_BY_15), vdupq_n_s32(2048)), 12);
        thr = vaddq_s32(
            thr, vaddq_s32(adm_neon_cm_ring_s16(c->flt_angles[theta] + off, c->csf_a_stride), tap));
    }
    return thr;
}

/* The rounded cube ((v^2 + add_sq) >> shift_sq narrowed to int32) * v, plus
 * add_cub, shifted right arithmetically by shift_cub: two lanes of
 * adm_cm_accum_round() / i4_adm_cm_accum_round() after the excess. */
static inline int64x2_t adm_neon_cm_cube(int32x2_t v, const AdmCmBand *p)
{
    const int64x2_t sq = vshlq_s64(vaddq_s64(vmull_s32(v, v), vdupq_n_s64(p->add_shift_sq)),
                                   vdupq_n_s64(-(int64_t)p->shift_sq));
    const int64x2_t cub =
        vaddq_s64(vmull_s32(vmovn_s64(sq), v), vdupq_n_s64((int64_t)p->add_shift_cub));
    return vshlq_s64(cub, vdupq_n_s64(-(int64_t)p->shift_cub));
}

/* adm_cm_excess_s0() on two lanes: clamp(|x| - thr * 2^shift_sub, 0,
 * INT32_MAX) in int64. */
static inline int32x2_t adm_neon_cm_excess_s0(int32x2_t x, int32x2_t thr, int32_t shift_sub)
{
    const int64x2_t magnitude = vabsq_s64(vmovl_s32(x));
    const int64x2_t excess =
        vsubq_s64(magnitude, vshlq_s64(vmovl_s32(thr), vdupq_n_s64(shift_sub)));
    const int64x2_t zero = vdupq_n_s64(0);
    const int64x2_t max = vdupq_n_s64(INT32_MAX);
    const int64x2_t floored = vbslq_s64(vcltq_s64(excess, zero), zero, excess);
    return vmovn_s64(vbslq_s64(vcgtq_s64(floored, max), max, floored));
}

/* One band of four scale-0 columns: x = band * i_rfactor (int32, as the
 * scalar's int16 * uint16 product), then the excess and the cube, added to
 * the band's two uint64 lanes. */
static inline uint64x2_t adm_neon_cm_band_s0(uint64x2_t acc, const int16_t *src, uint16_t rfactor,
                                             int32x4_t thr, const AdmCmBand *p)
{
    const int32x4_t x = vmulq_n_s32(vmovl_s16(vld1_s16(src)), (int32_t)rfactor);
    const int32x2_t lo = adm_neon_cm_excess_s0(vget_low_s32(x), vget_low_s32(thr), p->shift_sub);
    const int32x2_t hi = adm_neon_cm_excess_s0(vget_high_s32(x), vget_high_s32(thr), p->shift_sub);
    acc = vaddq_u64(acc, vreinterpretq_u64_s64(adm_neon_cm_cube(lo, p)));
    return vaddq_u64(acc, vreinterpretq_u64_s64(adm_neon_cm_cube(hi, p)));
}

/* adm_cm_row() of an interior row: four columns at a time, the scalar
 * sample for the edge columns, for rows narrower than one block and for the
 * columns after the last block. The row is summed in uint64, as the scalar
 * row is (modulo 2^64, so the lanes may be added in any order). */
static void adm_neon_cm_row(const AdmCmCtx *c, int i, const AdmCmBounds *bd, uint64_t inner[3])
{
    if (bd->left_edge || bd->right_edge || bd->end_col - bd->start_col < 4) {
        adm_cm_row(c, i, bd, inner);
        return;
    }
    const int16_t *const src[3] = {c->src->band_h, c->src->band_v, c->src->band_d};
    uint64x2_t acc[3] = {vdupq_n_u64(0), vdupq_n_u64(0), vdupq_n_u64(0)};
    int j = bd->start_col;

    for (; j + 4 <= bd->end_col; j += 4) {
        const int32x4_t thr = adm_neon_cm_thresh(c, i, j);
        const ptrdiff_t idx = ((ptrdiff_t)i * c->src_stride) + j;
        for (int b = 0; b < 3; ++b) {
            acc[b] = adm_neon_cm_band_s0(acc[b], src[b] + idx, c->i_rfactor[b], thr, &c->band[b]);
        }
    }
    uint64_t row[3] = {vaddvq_u64(acc[0]), vaddvq_u64(acc[1]), vaddvq_u64(acc[2])};
    for (; j < bd->end_col; ++j) {
        adm_cm_accum_px(c, i, j, row);
    }
    for (int b = 0; b < 3; ++b) {
        inner[b] += row[b];
    }
}

float adm_cm_neon(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride,
                  double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                  double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                  double adm_p_norm, bool measure_aim)
{
    AdmCmCtx c;
    adm_cm_ctx_init(&c, buf, w, h, src_stride, csf_a_stride, adm_norm_view_dist,
                    adm_ref_display_height, adm_csf_mode, adm_csf_scale, adm_csf_diag_scale,
                    measure_aim);
    const AdmCmBounds bd = adm_cm_bounds(w, h);

    uint64_t accum[3] = {0, 0, 0};
    adm_cm_rows(&c, &bd, adm_neon_cm_row, accum);
    return adm_cm_result(&c, &bd, accum, adm_noise_weight, adm_p_norm);
}

/* The 32-bit twin of adm_neon_cm_ring_s16(): int32 adds, as the scalar's. */
static inline int32x4_t adm_neon_cm_ring_s32(const int32_t *flt, ptrdiff_t stride)
{
    int32x4_t sum = vld1q_s32(flt - stride - 1);
    sum = vaddq_s32(sum, vld1q_s32(flt - stride));
    sum = vaddq_s32(sum, vld1q_s32(flt - stride + 1));
    sum = vaddq_s32(sum, vld1q_s32(flt - 1));
    sum = vaddq_s32(sum, vld1q_s32(flt + 1));
    sum = vaddq_s32(sum, vld1q_s32(flt + stride - 1));
    sum = vaddq_s32(sum, vld1q_s32(flt + stride));
    return vaddq_s32(sum, vld1q_s32(flt + stride + 1));
}

/* (int32)(((int64)a * m + add) >> shift) on four lanes; `m` fits int32. */
static inline int32x4_t adm_neon_mul_round_s32(int32x4_t a, int32_t m, int64_t add, uint32_t shift)
{
    const int64x2_t add_v = vdupq_n_s64(add);
    const int64x2_t shift_v = vdupq_n_s64(-(int64_t)shift);
    const int64x2_t lo = vshlq_s64(vaddq_s64(vmull_n_s32(vget_low_s32(a), m), add_v), shift_v);
    const int64x2_t hi = vshlq_s64(vaddq_s64(vmull_n_s32(vget_high_s32(a), m), add_v), shift_v);
    return vcombine_s32(vmovn_s64(lo), vmovn_s64(hi));
}

/* i4_adm_cm_thresh() of the four interior columns from `j` of row `i`. The
 * centre tap keeps ADR-0155's negative rounding term (add_bef_shift_flt). */
static inline int32x4_t adm_neon_i4_cm_thresh(const I4AdmCmCtx *c, int i, int j)
{
    const ptrdiff_t off = ((ptrdiff_t)i * c->csf_a_stride) + j;
    int32x4_t thr = vdupq_n_s32(0);

    for (int theta = 0; theta < 3; ++theta) {
        const int32x4_t mag = vabsq_s32(vld1q_s32(c->angles[theta] + off));
        const int32x4_t tap =
            adm_neon_mul_round_s32(mag, I4_ONE_BY_15, c->add_bef_shift_flt, c->shift_flt);
        thr = vaddq_s32(
            thr, vaddq_s32(adm_neon_cm_ring_s32(c->flt_angles[theta] + off, c->csf_a_stride), tap));
    }
    return thr;
}

/* One band of four scale 1-3 columns: x = i4_adm_cm_scale(), the excess
 * max(|x| - (thr >> shift_sub), 0) in int32 and the cube, added to the band's
 * two int64 lanes. */
static inline int64x2_t adm_neon_i4_cm_band(int64x2_t acc, const I4AdmCmCtx *c, const int32_t *src,
                                            uint32_t rfactor, int32x4_t thr)
{
    const int32x4_t x = adm_neon_mul_round_s32(vld1q_s32(src), (int32_t)rfactor,
                                               c->add_bef_shift_dst, c->shift_dst);
    const int32x4_t limit = vshlq_s32(thr, vdupq_n_s32(-c->band.shift_sub));
    const int32x4_t v = vmaxq_s32(vsubq_s32(vabsq_s32(x), limit), vdupq_n_s32(0));
    acc = vaddq_s64(acc, adm_neon_cm_cube(vget_low_s32(v), &c->band));
    return vaddq_s64(acc, adm_neon_cm_cube(vget_high_s32(v), &c->band));
}

/* The row factor rides in a signed lane of vmull_n_s32(): a factor above
 * INT32_MAX takes the scalar rows. */
static inline bool adm_neon_i4_rfactor_fits(const I4AdmCmCtx *c)
{
    return c->rfactor[0] <= INT32_MAX && c->rfactor[1] <= INT32_MAX && c->rfactor[2] <= INT32_MAX;
}

/* i4_adm_cm_row() of an interior row; see adm_neon_cm_row(). */
static void adm_neon_i4_cm_row(const I4AdmCmCtx *c, int i, const AdmCmBounds *bd, int64_t inner[3])
{
    if (bd->left_edge || bd->right_edge || bd->end_col - bd->start_col < 4 ||
        !adm_neon_i4_rfactor_fits(c)) {
        i4_adm_cm_row(c, i, bd, inner);
        return;
    }
    const int32_t *const src[3] = {c->src->band_h, c->src->band_v, c->src->band_d};
    int64x2_t acc[3] = {vdupq_n_s64(0), vdupq_n_s64(0), vdupq_n_s64(0)};
    int j = bd->start_col;

    for (; j + 4 <= bd->end_col; j += 4) {
        const int32x4_t thr = adm_neon_i4_cm_thresh(c, i, j);
        const ptrdiff_t idx = ((ptrdiff_t)i * c->src_stride) + j;
        for (int b = 0; b < 3; ++b) {
            acc[b] = adm_neon_i4_cm_band(acc[b], c, src[b] + idx, c->rfactor[b], thr);
        }
    }
    int64_t row[3] = {vaddvq_s64(acc[0]), vaddvq_s64(acc[1]), vaddvq_s64(acc[2])};
    for (; j < bd->end_col; ++j) {
        i4_adm_cm_accum_px(c, i, j, row);
    }
    for (int b = 0; b < 3; ++b) {
        inner[b] += row[b];
    }
}

float i4_adm_cm_neon(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride, int scale,
                     double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                     double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                     double adm_p_norm, bool measure_aim)
{
    I4AdmCmCtx c;
    i4_adm_cm_ctx_init(&c, buf, w, h, src_stride, csf_a_stride, scale, adm_norm_view_dist,
                       adm_ref_display_height, adm_csf_mode, adm_csf_scale, adm_csf_diag_scale,
                       measure_aim);
    const AdmCmBounds bd = adm_cm_bounds(w, h);

    int64_t accum[3] = {0, 0, 0};
    i4_adm_cm_rows(&c, &bd, adm_neon_i4_cm_row, accum);
    return i4_adm_cm_result(&c, &bd, accum, adm_noise_weight, adm_p_norm);
}

/*
 * Daubechies-2 DWT of scales 1 to 3, four columns at a time (Netflix/vmaf
 * b41d2340a). The scalar reference is adm_dwt2_s123_combined() in
 * integer_adm.c: i4_dwt2_tap4() accumulates the four taps in int64, adds the
 * scale's rounding term and shifts arithmetically, and the int32 result keeps
 * the low 32 bits. The vector lanes do the same. The vertical pass runs over
 * every column; the horizontal pass takes the four outputs from `j` with
 * de-interleaving loads when their taps need no mirror (j >= 1 and
 * 2 * j + 8 < w: ind_x[k][j] = 2j - 1 + k, dwt2_src_indices_1d()), and the
 * scalar i4_dwt2_hpass_bands() through ind_x otherwise.
 */

/* i4_dwt2_tap4() on four lanes. */
static inline int32x4_t adm_neon_i4_dwt_taps(const int32x4_t s[4], const int16_t *filter,
                                             int32_t add, int16_t shift)
{
    const int64x2_t add_v = vdupq_n_s64(add);
    const int64x2_t shift_v = vdupq_n_s64(-(int64_t)shift);
    int64x2_t lo = vmull_n_s32(vget_low_s32(s[0]), filter[0]);
    int64x2_t hi = vmull_n_s32(vget_high_s32(s[0]), filter[0]);

    for (int k = 1; k < 4; ++k) {
        lo = vmlal_n_s32(lo, vget_low_s32(s[k]), filter[k]);
        hi = vmlal_n_s32(hi, vget_high_s32(s[k]), filter[k]);
    }
    return vcombine_s32(vmovn_s64(vshlq_s64(vaddq_s64(lo, add_v), shift_v)),
                        vmovn_s64(vshlq_s64(vaddq_s64(hi, add_v), shift_v)));
}

/* Vertical pass of one plane's output row into tmplo / tmphi (w each). */
static void adm_neon_i4_dwt_vpass(const int32_t *const rows[4], int w, int32_t *tmplo,
                                  int32_t *tmphi, const I4Dwt2Round *r)
{
    int j = 0;

    for (; j + 4 <= w; j += 4) {
        const int32x4_t s[4] = {vld1q_s32(rows[0] + j), vld1q_s32(rows[1] + j),
                                vld1q_s32(rows[2] + j), vld1q_s32(rows[3] + j)};
        vst1q_s32(tmplo + j, adm_neon_i4_dwt_taps(s, dwt2_db2_coeffs_lo, r->add_vp, r->shift_vp));
        vst1q_s32(tmphi + j, adm_neon_i4_dwt_taps(s, dwt2_db2_coeffs_hi, r->add_vp, r->shift_vp));
    }
    for (; j < w; ++j) {
        tmplo[j] = i4_dwt2_tap4(dwt2_db2_coeffs_lo, rows[0][j], rows[1][j], rows[2][j], rows[3][j],
                                r->add_vp, r->shift_vp);
        tmphi[j] = i4_dwt2_tap4(dwt2_db2_coeffs_hi, rows[0][j], rows[1][j], rows[2][j], rows[3][j],
                                r->add_vp, r->shift_vp);
    }
}

/* The four outputs from `j` of one plane, taps 2j - 1 .. 2j + 2 read by two
 * de-interleaving loads per row buffer. */
static inline void adm_neon_i4_dwt_hblock(const int32_t *tmplo, const int32_t *tmphi,
                                          const i4_adm_dwt_band_t *dst, int j, ptrdiff_t out,
                                          const I4Dwt2Round *r)
{
    const int32x4x2_t lo01 = vld2q_s32(tmplo + ((ptrdiff_t)2 * j) - 1);
    const int32x4x2_t lo23 = vld2q_s32(tmplo + ((ptrdiff_t)2 * j) + 1);
    const int32x4x2_t hi01 = vld2q_s32(tmphi + ((ptrdiff_t)2 * j) - 1);
    const int32x4x2_t hi23 = vld2q_s32(tmphi + ((ptrdiff_t)2 * j) + 1);
    const int32x4_t lo[4] = {lo01.val[0], lo01.val[1], lo23.val[0], lo23.val[1]};
    const int32x4_t hi[4] = {hi01.val[0], hi01.val[1], hi23.val[0], hi23.val[1]};

    vst1q_s32(dst->band_a + out,
              adm_neon_i4_dwt_taps(lo, dwt2_db2_coeffs_lo, r->add_hp, r->shift_hp));
    vst1q_s32(dst->band_v + out,
              adm_neon_i4_dwt_taps(lo, dwt2_db2_coeffs_hi, r->add_hp, r->shift_hp));
    vst1q_s32(dst->band_h + out,
              adm_neon_i4_dwt_taps(hi, dwt2_db2_coeffs_lo, r->add_hp, r->shift_hp));
    vst1q_s32(dst->band_d + out,
              adm_neon_i4_dwt_taps(hi, dwt2_db2_coeffs_hi, r->add_hp, r->shift_hp));
}

/* Horizontal pass of one plane's output row `i`. */
static void adm_neon_i4_dwt_hpass(const int32_t *tmplo, const int32_t *tmphi,
                                  const i4_adm_dwt_band_t *dst, int *const *ind_x, int i, int w,
                                  int dst_stride, const I4Dwt2Round *r)
{
    const int half_w = (w + 1) / 2;
    int j = 0;

    while (j < half_w) {
        const ptrdiff_t out = ((ptrdiff_t)i * dst_stride) + j;
        if (j > 0 && (2 * j) + 8 < w) {
            adm_neon_i4_dwt_hblock(tmplo, tmphi, dst, j, out, r);
            j += 4;
        } else {
            const int jx[4] = {ind_x[0][j], ind_x[1][j], ind_x[2][j], ind_x[3][j]};
            i4_dwt2_hpass_bands(tmplo, tmphi, dst, jx, out, r->add_hp, r->shift_hp);
            ++j;
        }
    }
}

void adm_dwt2_s123_combined_neon(const int32_t *i4_ref_scale, const int32_t *i4_curr_dis,
                                 AdmBuffer *buf, int w, int h, int ref_stride, int dis_stride,
                                 int dst_stride, int scale)
{
    const I4Dwt2Round r = i4_dwt2_round(scale);
    int *const *ind_y = buf->ind_y;
    /* The scalar layout of tmp_ref: tmplo_ref, tmphi_ref, tmplo_dis, tmphi_dis. */
    int32_t *tmp = buf->tmp_ref;
    int32_t *const tmplo_ref = tmp;
    int32_t *const tmphi_ref = tmp + w;
    int32_t *const tmplo_dis = tmp + ((ptrdiff_t)2 * w);
    int32_t *const tmphi_dis = tmp + ((ptrdiff_t)3 * w);

    for (int i = 0; i < (h + 1) / 2; ++i) {
        const int32_t *const ref_rows[4] = {
            i4_ref_scale + ((ptrdiff_t)ind_y[0][i] * ref_stride),
            i4_ref_scale + ((ptrdiff_t)ind_y[1][i] * ref_stride),
            i4_ref_scale + ((ptrdiff_t)ind_y[2][i] * ref_stride),
            i4_ref_scale + ((ptrdiff_t)ind_y[3][i] * ref_stride),
        };
        const int32_t *const dis_rows[4] = {
            i4_curr_dis + ((ptrdiff_t)ind_y[0][i] * dis_stride),
            i4_curr_dis + ((ptrdiff_t)ind_y[1][i] * dis_stride),
            i4_curr_dis + ((ptrdiff_t)ind_y[2][i] * dis_stride),
            i4_curr_dis + ((ptrdiff_t)ind_y[3][i] * dis_stride),
        };
        adm_neon_i4_dwt_vpass(ref_rows, w, tmplo_ref, tmphi_ref, &r);
        adm_neon_i4_dwt_vpass(dis_rows, w, tmplo_dis, tmphi_dis, &r);
        adm_neon_i4_dwt_hpass(tmplo_ref, tmphi_ref, &buf->i4_ref_dwt2, buf->ind_x, i, w, dst_stride,
                              &r);
        adm_neon_i4_dwt_hpass(tmplo_dis, tmphi_dis, &buf->i4_dis_dwt2, buf->ind_x, i, w, dst_stride,
                              &r);
    }
}

/*
 * Decouple of scales 1 to 3, four columns at a time (Netflix/vmaf b41d2340a).
 * The scalar reference is adm_decouple_s123_cols() in integer_adm_kernels.h.
 * The vector path serves an enhancement gain limit of 1, as upstream's does;
 * every other limit, and a region narrower than four columns, runs the
 * scalar kernel.
 *
 * At a limit of 1 the scalar stores MIN(rst, t) (rst_f > 0, which is k > 0
 * and o > 0) or MAX(rst, t) (k > 0, o < 0) where the angle flag is set, and
 * rst elsewhere. Only a sample whose rst overshoots t can change, so the
 * angle flag is evaluated for those columns alone, through the scalar
 * adm_angle_flag() on the int64 dot products, lane by lane.
 */

/* The reciprocal-table index and shift of get_best15_from32(): |o| itself
 * below 32768, else |o| rounded to its 15 most significant bits. */
static inline uint32x4_t adm_neon_s123_index(int32x4_t o, int32x4_t *shift)
{
    const uint32x4_t mag = vreinterpretq_u32_s32(vabsq_s32(o));
    *shift = vmaxq_s32(vsubq_s32(vdupq_n_s32(17), vreinterpretq_s32_u32(vclzq_u32(mag))),
                       vdupq_n_s32(0));
    return vrshlq_u32(mag, vnegq_s32(*shift));
}

/* tmp_k of adm_decouple_band_s123() on two lanes, clamped to [0, 32768]:
 * (lut * t * sign + 2^(14 + shift)) >> (15 + shift), in int64. */
static inline int32x2_t adm_neon_s123_k2(int32x2_t recip, int32x2_t t, int32x2_t shift)
{
    const int64x2_t prod = vmull_s32(recip, t);
    const int64x2_t total = vnegq_s64(vmovl_s32(vadd_s32(shift, vdup_n_s32(15))));
    int64x2_t k = vrshlq_s64(prod, total);
    k = vbslq_s64(vcltq_s64(k, vdupq_n_s64(0)), vdupq_n_s64(0), k);
    k = vbslq_s64(vcgtq_s64(k, vdupq_n_s64(32768)), vdupq_n_s64(32768), k);
    return vmovn_s64(k);
}

/* rst = (k * o + 16384) >> 15 of one band, and in `*overshoot` the lanes the
 * limit can change: k > 0 and rst past t on o's side. */
static inline int32x4_t adm_neon_s123_band(int32x4_t o, int32x4_t t, const int32_t *lookup,
                                           uint32x4_t *overshoot)
{
    int32x4_t shift;
    const uint32x4_t index = adm_neon_s123_index(o, &shift);
    const int32_t div[4] = {
        lookup[vgetq_lane_u32(index, 0) + 32768], lookup[vgetq_lane_u32(index, 1) + 32768],
        lookup[vgetq_lane_u32(index, 2) + 32768], lookup[vgetq_lane_u32(index, 3) + 32768]};
    const int32x4_t zero = vdupq_n_s32(0);
    int32x4_t recip = vld1q_s32(div);
    recip = vbslq_s32(vcltq_s32(o, zero), vnegq_s32(recip), recip);
    int32x4_t k = vcombine_s32(
        adm_neon_s123_k2(vget_low_s32(recip), vget_low_s32(t), vget_low_s32(shift)),
        adm_neon_s123_k2(vget_high_s32(recip), vget_high_s32(t), vget_high_s32(shift)));
    k = vbslq_s32(vceqq_s32(o, zero), vdupq_n_s32(32768), k);
    const int32x4_t rst =
        vcombine_s32(vrshrn_n_s64(vmull_s32(vget_low_s32(k), vget_low_s32(o)), 15),
                     vrshrn_n_s64(vmull_s32(vget_high_s32(k), vget_high_s32(o)), 15));
    const uint32x4_t over_pos = vandq_u32(vcgtq_s32(o, zero), vcgtq_s32(rst, t));
    const uint32x4_t over_neg = vandq_u32(vcltq_s32(o, zero), vcltq_s32(rst, t));
    *overshoot = vandq_u32(vcgtq_s32(k, zero), vorrq_u32(over_pos, over_neg));
    return rst;
}

/* The angle flag of every lane with an overshooting band, from the scalar
 * adm_angle_flag(); 0 elsewhere. */
static inline uint32x4_t adm_neon_s123_angle(const int32_t o[2][4], const int32_t t[2][4],
                                             uint32x4_t any, float cos_1deg_sq)
{
    uint32_t need[4];
    uint32_t flag[4];

    vst1q_u32(need, any);
    for (int l = 0; l < 4; ++l) {
        flag[l] = 0;
        if (need[l] == 0) {
            continue;
        }
        const int64_t dot = ((int64_t)o[0][l] * t[0][l]) + ((int64_t)o[1][l] * t[1][l]);
        const int64_t omag = ((int64_t)o[0][l] * o[0][l]) + ((int64_t)o[1][l] * o[1][l]);
        const int64_t tmag = ((int64_t)t[0][l] * t[0][l]) + ((int64_t)t[1][l] * t[1][l]);
        flag[l] = adm_angle_flag(dot, omag, tmag, cos_1deg_sq) ? UINT32_MAX : 0;
    }
    return vld1q_u32(flag);
}

/* Decouple the four columns at `off` at a limit of 1 and store both outputs. */
static void adm_neon_decouple_s123_4(AdmBuffer *buf, ptrdiff_t off, const int32_t *lookup,
                                     float cos_1deg_sq)
{
    const int32_t *const ref[3] = {buf->i4_ref_dwt2.band_h + off, buf->i4_ref_dwt2.band_v + off,
                                   buf->i4_ref_dwt2.band_d + off};
    const int32_t *const dis[3] = {buf->i4_dis_dwt2.band_h + off, buf->i4_dis_dwt2.band_v + off,
                                   buf->i4_dis_dwt2.band_d + off};
    int32x4_t o[3];
    int32x4_t t[3];
    int32x4_t rst[3];
    uint32x4_t over[3];

    for (int b = 0; b < 3; ++b) {
        o[b] = vld1q_s32(ref[b]);
        t[b] = vld1q_s32(dis[b]);
        rst[b] = adm_neon_s123_band(o[b], t[b], lookup, &over[b]);
    }
    const uint32x4_t any = vorrq_u32(vorrq_u32(over[0], over[1]), over[2]);
    if (vmaxvq_u32(any) != 0) {
        /* The h and v bands of both pictures feed the angle test. */
        int32_t ov[2][4];
        int32_t tv[2][4];
        for (int b = 0; b < 2; ++b) {
            vst1q_s32(ov[b], o[b]);
            vst1q_s32(tv[b], t[b]);
        }
        const uint32x4_t angle = adm_neon_s123_angle((const int32_t (*)[4])ov,
                                                     (const int32_t (*)[4])tv, any, cos_1deg_sq);
        for (int b = 0; b < 3; ++b) {
            rst[b] = vbslq_s32(vandq_u32(angle, over[b]), t[b], rst[b]);
        }
    }
    int32_t *const out_r[3] = {buf->i4_decouple_r.band_h + off, buf->i4_decouple_r.band_v + off,
                               buf->i4_decouple_r.band_d + off};
    int32_t *const out_a[3] = {buf->i4_decouple_a.band_h + off, buf->i4_decouple_a.band_v + off,
                               buf->i4_decouple_a.band_d + off};
    for (int b = 0; b < 3; ++b) {
        vst1q_s32(out_r[b], rst[b]);
        vst1q_s32(out_a[b], vsubq_s32(t[b], rst[b]));
    }
}

void adm_decouple_s123_neon(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                            int32_t *adm_div_lookup)
{
    const float cos_1deg_sq = adm_cos_1deg_sq();
    const AdmBorder b = adm_border_filt(w, h);

    if (b.right - b.left < 4 || adm_enhn_gain_limit != 1.0) {
        for (int i = b.top; i < b.bottom; ++i) {
            adm_decouple_s123_cols(buf, i, stride, b.left, b.right, adm_enhn_gain_limit,
                                   adm_div_lookup, cos_1deg_sq);
        }
        return;
    }
    for (int i = b.top; i < b.bottom; ++i) {
        /* As adm_decouple_neon(): the last group of four ends on the last
         * column and recomputes up to three columns from unchanged inputs. */
        for (int j = b.left; j < b.right; j += 4) {
            const int j0 = (j > b.right - 4) ? b.right - 4 : j;
            adm_neon_decouple_s123_4(buf, ((ptrdiff_t)i * stride) + j0, adm_div_lookup,
                                     cos_1deg_sq);
        }
    }
}
