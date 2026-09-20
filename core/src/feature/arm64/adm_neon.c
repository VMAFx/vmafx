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

#include "feature/arm64/adm_neon.h"
#include "feature/integer_adm.h"

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
