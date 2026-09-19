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

/* Each public kernel is a row loop over static helpers: a vertical pass into
 * the `buf->tmp` rows, the edge padding, then a horizontal pass. The helpers
 * issue the same intrinsics, lane for lane and in the same order, as the
 * macro-expanded kernels they replace, so every output is bit-identical to
 * them and to the scalar reference. Where the first filter tap seeds an
 * accumulator differently from the later taps (a plain product rather than a
 * multiply-accumulate), that tap keeps its own `*_init` helper. */

#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "feature/arm64/vif_neon.h"
#include "feature/common/macros.h"
#include "feature/integer_vif.h"
#include "libvmaf/vmaf_assert.h"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

static FORCE_INLINE void pad_top_and_bottom(const VifBuffer *buf, unsigned h, int fwidth)
{
    const unsigned fwidth_half = fwidth / 2;
    unsigned char *ref = buf->ref;
    unsigned char *dis = buf->dis;
    for (unsigned i = 1; i <= fwidth_half; ++i) {
        size_t offset = buf->stride * i;
        memcpy(ref - offset, ref + offset, buf->stride);
        memcpy(dis - offset, dis + offset, buf->stride);
        memcpy(ref + buf->stride * (h - 1) + buf->stride * i,
               ref + buf->stride * (h - 1) - buf->stride * i, buf->stride);
        memcpy(dis + buf->stride * (h - 1) + buf->stride * i,
               dis + buf->stride * (h - 1) - buf->stride * i, buf->stride);
    }
}

static FORCE_INLINE void decimate_and_pad(const VifBuffer *buf, unsigned w, unsigned h, int scale)
{
    uint16_t *ref = buf->ref;
    uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    const ptrdiff_t mu_stride = buf->stride_16 / sizeof(uint16_t);

    for (unsigned i = 0; i < h / 2; ++i) {
        for (unsigned j = 0; j < w / 2; ++j) {
            ref[i * stride + j] = buf->mu1[((ptrdiff_t)i * 2) * mu_stride + ((ptrdiff_t)j * 2)];
            dis[i * stride + j] = buf->mu2[((ptrdiff_t)i * 2) * mu_stride + ((ptrdiff_t)j * 2)];
        }
    }
    pad_top_and_bottom(buf, h / 2, vif_filter1d_width[scale]);
}

/* ------------------------------------------------------------------------ */
/* Lane containers. Every pair / quad keeps its columns in memory order,    */
/* `lo` before `hi`, so storing `lo` then `hi` writes consecutive columns.  */
/* ------------------------------------------------------------------------ */

/* 16 columns of u16. */
typedef struct VifU16x8Pair {
    uint16x8_t lo;
    uint16x8_t hi;
} VifU16x8Pair;

/* 8 columns of u32. */
typedef struct VifU32x4Pair {
    uint32x4_t lo;
    uint32x4_t hi;
} VifU32x4Pair;

/* 16 columns of u32. */
typedef struct VifU32x4Quad {
    VifU32x4Pair lo;
    VifU32x4Pair hi;
} VifU32x4Quad;

/* 4 columns of u64. */
typedef struct VifU64x2Pair {
    uint64x2_t lo;
    uint64x2_t hi;
} VifU64x2Pair;

/* 8 columns of u64. */
typedef struct VifU64x2Quad {
    VifU64x2Pair lo;
    VifU64x2Pair hi;
} VifU64x2Quad;

static FORCE_INLINE VifU32x4Pair vif_widen_u16(uint16x8_t v)
{
    const VifU32x4Pair wide = {vmovl_u16(vget_low_u16(v)), vmovl_high_u16(v)};
    return wide;
}

static FORCE_INLINE VifU32x4Pair vif_mul_u32(VifU32x4Pair a, VifU32x4Pair b)
{
    const VifU32x4Pair product = {vmulq_u32(a.lo, b.lo), vmulq_u32(a.hi, b.hi)};
    return product;
}

static FORCE_INLINE void vif_mla_u32(VifU32x4Pair *acc, VifU32x4Pair a, VifU32x4Pair b)
{
    acc->lo = vmlaq_u32(acc->lo, a.lo, b.lo);
    acc->hi = vmlaq_u32(acc->hi, a.hi, b.hi);
}

static FORCE_INLINE VifU32x4Pair vif_mull_n_u16(uint16x8_t v, uint16_t coeff)
{
    const VifU32x4Pair product = {vmull_n_u16(vget_low_u16(v), coeff), vmull_high_n_u16(v, coeff)};
    return product;
}

static FORCE_INLINE void vif_mlal_n_u16(VifU32x4Pair *acc, uint16x8_t v, uint16_t coeff)
{
    acc->lo = vmlal_n_u16(acc->lo, vget_low_u16(v), coeff);
    acc->hi = vmlal_high_n_u16(acc->hi, v, coeff);
}

static FORCE_INLINE VifU32x4Quad vif_mull16_n_u16(VifU16x8Pair v, uint16_t coeff)
{
    const VifU32x4Quad product = {vif_mull_n_u16(v.lo, coeff), vif_mull_n_u16(v.hi, coeff)};
    return product;
}

static FORCE_INLINE void vif_mlal16_n_u16(VifU32x4Quad *acc, VifU16x8Pair v, uint16_t coeff)
{
    vif_mlal_n_u16(&acc->lo, v.lo, coeff);
    vif_mlal_n_u16(&acc->hi, v.hi, coeff);
}

static FORCE_INLINE VifU64x2Quad vif_splat_u64(uint64x2_t v)
{
    const VifU64x2Quad splat = {{v, v}, {v, v}};
    return splat;
}

static FORCE_INLINE void vif_mlal_n_u32(VifU64x2Quad *acc, VifU32x4Pair v, uint16_t coeff)
{
    acc->lo.lo = vmlal_n_u32(acc->lo.lo, vget_low_u32(v.lo), coeff);
    acc->lo.hi = vmlal_high_n_u32(acc->lo.hi, v.lo, coeff);
    acc->hi.lo = vmlal_n_u32(acc->hi.lo, vget_low_u32(v.hi), coeff);
    acc->hi.hi = vmlal_high_n_u32(acc->hi.hi, v.hi, coeff);
}

static FORCE_INLINE void vif_mlal_u32(VifU64x2Quad *acc, VifU32x4Pair a, VifU32x4Pair b)
{
    acc->lo.lo = vmlal_u32(acc->lo.lo, vget_low_u32(a.lo), vget_low_u32(b.lo));
    acc->lo.hi = vmlal_high_u32(acc->lo.hi, a.lo, b.lo);
    acc->hi.lo = vmlal_u32(acc->hi.lo, vget_low_u32(a.hi), vget_low_u32(b.hi));
    acc->hi.hi = vmlal_high_u32(acc->hi.hi, a.hi, b.hi);
}

/* Shift each u64 lane right by -`shift` and keep its low 32 bits. */
static FORCE_INLINE uint32x4_t vif_narrow_u64(VifU64x2Pair v, int64x2_t shift)
{
    return vuzp1q_u32(vreinterpretq_u32_u64(vshlq_u64(v.lo, shift)),
                      vreinterpretq_u32_u64(vshlq_u64(v.hi, shift)));
}

/* Shift each u32 lane right by -`shift` and keep its low 16 bits. */
static FORCE_INLINE uint16x8_t vif_narrow_u32(VifU32x4Pair v, int32x4_t shift)
{
    return vuzp1q_u16(vreinterpretq_u16_u32(vshlq_u32(v.lo, shift)),
                      vreinterpretq_u16_u32(vshlq_u32(v.hi, shift)));
}

static FORCE_INLINE void vif_store_u32(VifU32x4Pair v, uint32_t *dst)
{
    vst1q_u32(dst, v.lo);
    vst1q_u32(dst + 4, v.hi);
}

static FORCE_INLINE void vif_store16_u32(VifU32x4Quad v, uint32_t *dst)
{
    vif_store_u32(v.lo, dst);
    vif_store_u32(v.hi, dst + 8);
}

/* dst = (v + round) >> -shift per u32 lane. */
static FORCE_INLINE void vif_store_round_shift_u32(VifU32x4Pair v, uint32x4_t round,
                                                   int32x4_t shift, uint32_t *dst)
{
    vst1q_u32(dst, vshlq_u32(vaddq_u32(v.lo, round), shift));
    vst1q_u32(dst + 4, vshlq_u32(vaddq_u32(v.hi, round), shift));
}

/* dst = v >> -shift per u32 lane, for accumulators seeded with their rounding bias. */
static FORCE_INLINE void vif_store16_shift_u32(VifU32x4Quad v, int32x4_t shift, uint32_t *dst)
{
    vst1q_u32(dst, vshlq_u32(v.lo.lo, shift));
    vst1q_u32(dst + 4, vshlq_u32(v.lo.hi, shift));
    vst1q_u32(dst + 8, vshlq_u32(v.hi.lo, shift));
    vst1q_u32(dst + 12, vshlq_u32(v.hi.hi, shift));
}

static FORCE_INLINE void vif_store_narrow_u64(VifU64x2Quad v, int64x2_t shift, uint32_t *dst)
{
    vst1q_u32(dst, vif_narrow_u64(v.lo, shift));
    vst1q_u32(dst + 4, vif_narrow_u64(v.hi, shift));
}

/* Filter taps and fixed-point rounding of one 16-bit vertical pass. At scale 0
 * the means drop `bpc` bits and the second moments `2 * (bpc - 8)`; every
 * coarser scale filters 16-bit data and drops 16 bits from both. */
typedef struct VifFilterPlan {
    unsigned fwidth;
    const uint16_t *vif_filt;
    int32_t add_shift_round_VP;
    int32_t shift_VP;
    int32_t add_shift_round_VP_sq;
    int32_t shift_VP_sq;
} VifFilterPlan;

static FORCE_INLINE VifFilterPlan vif_filter_plan(int filter, int bpc, int scale)
{
    VifFilterPlan p;
    p.fwidth = vif_filter1d_width[filter];
    p.vif_filt = vif_filter1d_table[filter];
    if (scale == 0) {
        p.add_shift_round_VP = 1 << (bpc - 1);
        p.shift_VP = bpc;
        p.shift_VP_sq = (bpc - 8) * 2;
        p.add_shift_round_VP_sq = (bpc == 8) ? 0 : 1 << (p.shift_VP_sq - 1);
    } else {
        p.add_shift_round_VP = 32768;
        p.shift_VP = 16;
        p.add_shift_round_VP_sq = 32768;
        p.shift_VP_sq = 16;
    }
    return p;
}

/* ------------------------------------------------------------------------ */
/* vif_subsample_rd_*: 16 columns per block. The vertical pass fills        */
/* tmp.ref_convol / tmp.dis_convol, the horizontal pass writes mu1 / mu2.   */
/* ------------------------------------------------------------------------ */

static FORCE_INLINE VifU16x8Pair vif_load16_u8(const uint8_t *src)
{
    const VifU16x8Pair v = {vmovl_u8(vld1_u8(src)), vmovl_u8(vld1_u8(src + 8))};
    return v;
}

static FORCE_INLINE VifU16x8Pair vif_load16_u16(const uint16_t *src)
{
    const VifU16x8Pair v = {vld1q_u16(src), vld1q_u16(src + 8)};
    return v;
}

/* One plane, 16 columns, 8-bit input: (sum + 128) >> 8. */
static FORCE_INLINE void vif_subsample8_vertical16(const uint8_t *src, ptrdiff_t stride,
                                                   uint32_t *dst)
{
    const unsigned int fwidth = vif_filter1d_width[1];
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];
    const uint32x4_t offset_vec_v = vdupq_n_u32(128);
    VifU32x4Quad accum = {{offset_vec_v, offset_vec_v}, {offset_vec_v, offset_vec_v}};

    for (unsigned fi = 0; fi < fwidth; ++fi)
        vif_mlal16_n_u16(&accum, vif_load16_u8(src + (ptrdiff_t)fi * stride), vif_filt_s1[fi]);
    vif_store16_shift_u32(accum, vdupq_n_s32(-8), dst);
}

/* One plane, 16 columns, 16-bit input: (sum + round) >> shift from the plan. */
static FORCE_INLINE void vif_subsample16_vertical16(const uint16_t *src, ptrdiff_t stride,
                                                    const VifFilterPlan *p, uint32_t *dst)
{
    const uint32x4_t add_shift_round_VP_vec = vdupq_n_u32(p->add_shift_round_VP);
    VifU32x4Quad accum = {{add_shift_round_VP_vec, add_shift_round_VP_vec},
                          {add_shift_round_VP_vec, add_shift_round_VP_vec}};

    for (unsigned fi = 0; fi < p->fwidth; ++fi)
        vif_mlal16_n_u16(&accum, vif_load16_u16(src + (ptrdiff_t)fi * stride), p->vif_filt[fi]);
    vif_store16_shift_u32(accum, vdupq_n_s32(-p->shift_VP), dst);
}

static FORCE_INLINE void vif_subsample8_vertical_tail(const VifBuffer *buf, unsigned i, unsigned j)
{
    const unsigned int fwidth = vif_filter1d_width[1];
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];
    const uint8_t *ref = (const uint8_t *)buf->ref;
    const uint8_t *dis = (const uint8_t *)buf->dis;
    uint32_t accum_ref = 128;
    uint32_t accum_dis = 128;

    for (unsigned fi = 0; fi < fwidth; ++fi) {
        const int ii = i - fwidth / 2;
        const int ii_check = ii + fi;
        const uint16_t fcoeff = vif_filt_s1[fi];
        accum_ref += fcoeff * (uint32_t)ref[ii_check * buf->stride + j];
        accum_dis += fcoeff * (uint32_t)dis[ii_check * buf->stride + j];
    }
    buf->tmp.ref_convol[j] = accum_ref >> 8;
    buf->tmp.dis_convol[j] = accum_dis >> 8;
}

static FORCE_INLINE void vif_subsample16_vertical_tail(const VifBuffer *buf, unsigned i, unsigned j,
                                                       const VifFilterPlan *p)
{
    const ptrdiff_t stride_v = buf->stride / sizeof(uint16_t);
    const uint16_t *ref = (const uint16_t *)buf->ref;
    const uint16_t *dis = (const uint16_t *)buf->dis;
    uint32_t accum_ref = 0;
    uint32_t accum_dis = 0;

    for (unsigned fi = 0; fi < p->fwidth; ++fi) {
        const int ii = i - p->fwidth / 2;
        const int ii_check = ii + fi;
        const uint16_t fcoeff = p->vif_filt[fi];
        accum_ref += fcoeff * ((uint32_t)ref[ii_check * stride_v + j]);
        accum_dis += fcoeff * ((uint32_t)dis[ii_check * stride_v + j]);
    }
    buf->tmp.ref_convol[j] = (uint16_t)((accum_ref + p->add_shift_round_VP) >> p->shift_VP);
    buf->tmp.dis_convol[j] = (uint16_t)((accum_dis + p->add_shift_round_VP) >> p->shift_VP);
}

static FORCE_INLINE void vif_subsample8_vertical_row(const VifBuffer *buf, unsigned w, unsigned i)
{
    const unsigned int uiw15 = (w > 15 ? w - 15 : 0);
    const unsigned int fwidth = vif_filter1d_width[1];
    const int ii = i - fwidth / 2;
    const uint8_t *p_ref = (const uint8_t *)buf->ref + ii * buf->stride;
    const uint8_t *p_dis = (const uint8_t *)buf->dis + ii * buf->stride;

    unsigned int j = 0;
    for (; j < uiw15; j += 16) {
        vif_subsample8_vertical16(p_ref + j, buf->stride, buf->tmp.ref_convol + j);
        vif_subsample8_vertical16(p_dis + j, buf->stride, buf->tmp.dis_convol + j);
    }
    for (; j < w; ++j)
        vif_subsample8_vertical_tail(buf, i, j);
}

static FORCE_INLINE void vif_subsample16_vertical_row(const VifBuffer *buf, unsigned w, unsigned i,
                                                      const VifFilterPlan *p)
{
    const unsigned int uiw15 = (w > 15 ? w - 15 : 0);
    const ptrdiff_t stride_v = buf->stride / sizeof(uint16_t);
    const int ii = i - p->fwidth / 2;
    const uint16_t *p_ref = (const uint16_t *)buf->ref + ii * stride_v;
    const uint16_t *p_dis = (const uint16_t *)buf->dis + ii * stride_v;

    unsigned int j = 0;
    for (; j < uiw15; j += 16) {
        vif_subsample16_vertical16(p_ref + j, stride_v, p, buf->tmp.ref_convol + j);
        vif_subsample16_vertical16(p_dis + j, stride_v, p, buf->tmp.dis_convol + j);
    }
    for (; j < w; ++j)
        vif_subsample16_vertical_tail(buf, i, j, p);
}

/* One plane, 16 columns: (sum + 32768) >> 16 over a u32 convolution row,
 * narrowed to u16. `src` points `fwidth / 2` columns left of the block. */
static FORCE_INLINE void vif_subsample_horizontal16(const uint32_t *src, int filter, uint16_t *dst)
{
    const unsigned int fwidth = vif_filter1d_width[filter];
    const uint16_t *vif_filt = vif_filter1d_table[filter];
    const uint32x4_t offset_vec_h = vdupq_n_u32(32768);
    const int32x4_t shift_vec_h = vdupq_n_s32(-16);
    VifU32x4Quad accum = {{offset_vec_h, offset_vec_h}, {offset_vec_h, offset_vec_h}};

    for (unsigned fj = 0; fj < fwidth; ++fj) {
        const uint32_t *tap = src + fj;
        accum.lo.lo = vmlaq_n_u32(accum.lo.lo, vld1q_u32(tap), vif_filt[fj]);
        accum.lo.hi = vmlaq_n_u32(accum.lo.hi, vld1q_u32(tap + 4), vif_filt[fj]);
        accum.hi.lo = vmlaq_n_u32(accum.hi.lo, vld1q_u32(tap + 8), vif_filt[fj]);
        accum.hi.hi = vmlaq_n_u32(accum.hi.hi, vld1q_u32(tap + 12), vif_filt[fj]);
    }
    vst1q_u16(dst, vif_narrow_u32(accum.lo, shift_vec_h));
    vst1q_u16(dst + 8, vif_narrow_u32(accum.hi, shift_vec_h));
}

static FORCE_INLINE void vif_subsample_horizontal_tail(const VifBuffer *buf, unsigned j, int filter,
                                                       uint16_t *mu1, uint16_t *mu2)
{
    const unsigned int fwidth = vif_filter1d_width[filter];
    const uint16_t *vif_filt = vif_filter1d_table[filter];
    uint32_t accum_ref = 32768;
    uint32_t accum_dis = 32768;

    for (unsigned fj = 0; fj < fwidth; ++fj) {
        const int jj = j - fwidth / 2;
        const int jj_check = jj + fj;
        const uint16_t fcoeff = vif_filt[fj];
        accum_ref += fcoeff * buf->tmp.ref_convol[jj_check];
        accum_dis += fcoeff * buf->tmp.dis_convol[jj_check];
    }
    mu1[j] = (uint16_t)(accum_ref >> 16);
    mu2[j] = (uint16_t)(accum_dis >> 16);
}

static FORCE_INLINE void vif_subsample_horizontal_row(const VifBuffer *buf, unsigned w, unsigned i,
                                                      int filter)
{
    const unsigned int uiw15 = (w > 15 ? w - 15 : 0);
    const unsigned int fwidth = vif_filter1d_width[filter];
    const ptrdiff_t dst_stride = buf->stride_16 / sizeof(uint16_t);
    uint16_t *mu1 = buf->mu1 + (ptrdiff_t)i * dst_stride;
    uint16_t *mu2 = buf->mu2 + (ptrdiff_t)i * dst_stride;
    const uint32_t *p_ref_conv = buf->tmp.ref_convol - (fwidth / 2);
    const uint32_t *p_dis_conv = buf->tmp.dis_convol - (fwidth / 2);

    unsigned int j = 0;
    for (; j < uiw15; j += 16) {
        vif_subsample_horizontal16(p_ref_conv + j, filter, mu1 + j);
        vif_subsample_horizontal16(p_dis_conv + j, filter, mu2 + j);
    }
    for (; j < w; ++j)
        vif_subsample_horizontal_tail(buf, j, filter, mu1, mu2);
}

void vif_subsample_rd_8_neon(const VifBuffer *buf, unsigned int w, unsigned int h)
{
    VMAF_ASSERT_DEBUG(buf->ref != NULL);
    VMAF_ASSERT_DEBUG(buf->dis != NULL);
    VMAF_ASSERT_DEBUG(w > 0 && h > 0);
    const unsigned int fwidth = vif_filter1d_width[1];

    for (unsigned int i = 0; i < h; ++i) {
        vif_subsample8_vertical_row(buf, w, i);
        PADDING_SQ_DATA_2(buf, w, fwidth / 2);
        vif_subsample_horizontal_row(buf, w, i, 1);
    }
    decimate_and_pad(buf, w, h, 0);
}

void vif_subsample_rd_16_neon(const VifBuffer *buf, unsigned int w, unsigned int h, int scale,
                              int bpc)
{
    VMAF_ASSERT_DEBUG(buf->ref != NULL);
    VMAF_ASSERT_DEBUG(buf->dis != NULL);
    VMAF_ASSERT_DEBUG(w > 0 && h > 0);
    VMAF_ASSERT_DEBUG(scale >= 0 && scale < 3);
    VMAF_ASSERT_DEBUG(bpc >= 8 && bpc <= 16);
    const VifFilterPlan plan = vif_filter_plan(scale + 1, bpc, scale);

    for (unsigned i = 0; i < h; ++i) {
        vif_subsample16_vertical_row(buf, w, i, &plan);
        PADDING_SQ_DATA_2(buf, w, plan.fwidth / 2);
        vif_subsample_horizontal_row(buf, w, i, scale + 1);
    }
    decimate_and_pad(buf, w, h, scale);
}

/* ------------------------------------------------------------------------ */
/* vif_statistic_*: the vertical pass fills tmp.mu1 / tmp.mu2 (means) and   */
/* tmp.ref / tmp.dis / tmp.ref_dis (second moments); the horizontal pass    */
/* filters them 8 columns at a time and accumulates num / den per pixel.    */
/* ------------------------------------------------------------------------ */

static FORCE_INLINE int64_t vif_num_log(const VifPublicState *s, int32_t sigma1_sq,
                                        int32_t sigma2_sq, int32_t sigma12)
{
    static const int32_t sigma_nsq = 65536 << 1;
    /**
    * In floating-point numerator = log2((1.0f + (g * g * sigma1_sq)/(sv_sq + sigma_nsq))
    *
    * In Fixed-point the above is converted to
    * numerator = log2((sv_sq + sigma_nsq)+(g * g * sigma1_sq))- log2(sv_sq + sigma_nsq)
    */

    const double eps = 65536 * 1.0e-10;
    double g = sigma12 / (sigma1_sq + eps); // this epsilon can go away
    int32_t sv_sq = sigma2_sq - g * sigma12;

    sv_sq = (uint32_t)(MAX(sv_sq, 0));

    g = MIN(g, s->vif_enhn_gain_limit);

    uint32_t numer1 = (sv_sq + sigma_nsq);
    int64_t numer1_tmp = (int64_t)((g * g * sigma1_sq)) + numer1; //numerator
    return log2_64(s->log2_table, numer1_tmp) - log2_64(s->log2_table, numer1);
}

static FORCE_INLINE void vif_accumulate_pixel(const VifPublicState *s, VifResiduals *totals,
                                              int32_t sigma1_sq, int32_t sigma2_sq, int32_t sigma12)
{
    static const int32_t sigma_nsq = 65536 << 1;
    if (sigma1_sq >= sigma_nsq) {
        /**
        * log values are taken from the look-up table generated by
        * log_generate() function which is called in integer_combo_threadfunc
        * den_val in float is log2(1 + sigma1_sq/2)
        * here it is converted to equivalent of log2(2+sigma1_sq) - log2(2) i.e log2(2*65536+sigma1_sq) - 17
        * multiplied by 2048 as log_value = log2(i)*2048 i=16384 to 65535 generated using log_value
        * x because best 16 bits are taken
        */
        totals->accum_den_log += log2_32(s->log2_table, sigma_nsq + sigma1_sq) - 2048 * 17;

        if (sigma12 > 0 && sigma2_sq > 0) {
            // num_val = log2f(1.0f + (g * g * sigma1_sq) / (sv_sq + sigma_nsq));
            totals->accum_num_log += vif_num_log(s, sigma1_sq, sigma2_sq, sigma12);
        }
    } else {
        totals->accum_num_non_log += sigma2_sq;
        totals->accum_den_non_log++;
    }
}

typedef struct VifStatHorizontal {
    VifU32x4Pair mu1;
    VifU32x4Pair mu2;
    VifU64x2Quad ref;
    VifU64x2Quad dis;
    VifU64x2Quad ref_dis;
} VifStatHorizontal;

static FORCE_INLINE VifU32x4Pair vif_load8_u32(const uint32_t *src)
{
    const VifU32x4Pair v = {vld1q_u32(src), vld1q_u32(src + 4)};
    return v;
}

static FORCE_INLINE VifU32x4Pair vif_mul8_n_u32(const uint32_t *src, uint16_t coeff)
{
    const VifU32x4Pair v = vif_load8_u32(src);
    const VifU32x4Pair product = {vmulq_n_u32(v.lo, coeff), vmulq_n_u32(v.hi, coeff)};
    return product;
}

static FORCE_INLINE void vif_mla8_n_u32(VifU32x4Pair *acc, const uint32_t *src, uint16_t coeff)
{
    const VifU32x4Pair v = vif_load8_u32(src);
    acc->lo = vmlaq_n_u32(acc->lo, v.lo, coeff);
    acc->hi = vmlaq_n_u32(acc->hi, v.hi, coeff);
}

/* First tap: the means start as plain products, the second moments as the
 * 32768 rounding bias plus the product. `k` is the tap's column offset. */
static FORCE_INLINE void vif_stat_horizontal_init(VifStatHorizontal *a, const VifBuffer *buf,
                                                  ptrdiff_t k, uint16_t coeff)
{
    a->mu1 = vif_mul8_n_u32(buf->tmp.mu1 + k, coeff);
    a->mu2 = vif_mul8_n_u32(buf->tmp.mu2 + k, coeff);
    a->ref = vif_splat_u64(vdupq_n_u64(32768));
    a->dis = a->ref;
    a->ref_dis = a->ref;
    vif_mlal_n_u32(&a->ref, vif_load8_u32(buf->tmp.ref + k), coeff);
    vif_mlal_n_u32(&a->dis, vif_load8_u32(buf->tmp.dis + k), coeff);
    vif_mlal_n_u32(&a->ref_dis, vif_load8_u32(buf->tmp.ref_dis + k), coeff);
}

static FORCE_INLINE void vif_stat_horizontal_tap(VifStatHorizontal *a, const VifBuffer *buf,
                                                 ptrdiff_t k, uint16_t coeff)
{
    vif_mla8_n_u32(&a->mu1, buf->tmp.mu1 + k, coeff);
    vif_mla8_n_u32(&a->mu2, buf->tmp.mu2 + k, coeff);
    vif_mlal_n_u32(&a->ref, vif_load8_u32(buf->tmp.ref + k), coeff);
    vif_mlal_n_u32(&a->dis, vif_load8_u32(buf->tmp.dis + k), coeff);
    vif_mlal_n_u32(&a->ref_dis, vif_load8_u32(buf->tmp.ref_dis + k), coeff);
}

/* (a * b + 2^31) >> 32 per lane: the rounded product of two filtered means. */
static FORCE_INLINE uint32x4_t vif_mean_product(uint32x4_t a, uint32x4_t b)
{
    const uint64x2_t round = vdupq_n_u64(2147483648);
    const VifU64x2Pair product = {vmlal_u32(round, vget_low_u32(a), vget_low_u32(b)),
                                  vmlal_high_u32(round, a, b)};
    return vif_narrow_u64(product, vdupq_n_s64(-32));
}

/* (moment >> 16) - mean_a * mean_b per lane, reinterpreted as signed. */
static FORCE_INLINE int32x4_t vif_sigma(VifU64x2Pair moment, uint32x4_t mean_a, uint32x4_t mean_b)
{
    const uint32x4_t filtered = vif_narrow_u64(moment, vdupq_n_s64(-16));
    return vreinterpretq_s32_u32(vsubq_u32(filtered, vif_mean_product(mean_a, mean_b)));
}

static FORCE_INLINE void vif_stat_horizontal_sigmas(const VifStatHorizontal *a, int32_t *xx,
                                                    int32_t *yy, int32_t *xy)
{
    const int32x4_t zero = vdupq_n_s32(0);

    vst1q_s32(xx, vif_sigma(a->ref.lo, a->mu1.lo, a->mu1.lo));
    vst1q_s32(xx + 4, vif_sigma(a->ref.hi, a->mu1.hi, a->mu1.hi));

    /* The scalar kernels clamp with `sigma2_sq = MAX(sigma2_sq, 0)` before
     * the branch, and the non-log arm then accumulates the clamped value into
     * `accum_num_non_log`.  Without the clamp a negative sigma2_sq — routine
     * on near-flat content, where the fixed-point rounding of mu2 outruns the
     * filtered dis^2 — is summed verbatim and drives num the wrong way.
     * `vif_statistic_8_avx2` (`_mm256_max_epi32`) clamps here too. */
    vst1q_s32(yy, vmaxq_s32(zero, vif_sigma(a->dis.lo, a->mu2.lo, a->mu2.lo)));
    vst1q_s32(yy + 4, vmaxq_s32(zero, vif_sigma(a->dis.hi, a->mu2.hi, a->mu2.hi)));

    vst1q_s32(xy, vif_sigma(a->ref_dis.lo, a->mu1.lo, a->mu2.lo));
    vst1q_s32(xy + 4, vif_sigma(a->ref_dis.hi, a->mu1.hi, a->mu2.hi));
}

/* 8 output columns starting at `j`, filtered with vif_filter1d_table[filter]. */
static FORCE_INLINE void vif_stat_horizontal8(const VifPublicState *s, const VifBuffer *buf,
                                              unsigned j, int filter, VifResiduals *totals)
{
    const unsigned int fwidth = vif_filter1d_width[filter];
    const uint16_t *vif_filt = vif_filter1d_table[filter];
    const ptrdiff_t k = (ptrdiff_t)j - (ptrdiff_t)(fwidth / 2);
    int32_t xx[8];
    int32_t yy[8];
    int32_t xy[8];
    VifStatHorizontal a;

    vif_stat_horizontal_init(&a, buf, k, vif_filt[0]);
    for (unsigned fj = 1; fj < fwidth; ++fj)
        vif_stat_horizontal_tap(&a, buf, k + fj, vif_filt[fj]);
    vif_stat_horizontal_sigmas(&a, xx, yy, xy);

    for (unsigned int b = 0; b < 8; b++)
        vif_accumulate_pixel(s, totals, xx[b], yy[b], xy[b]);
}

/* The horizontal loop steps 8 columns and stops at `w - 7`, but `integer_vif.c`
 * installs these kernels for *every* width — there is no `w % 8` admission
 * guard. Without this tail the last `w % 8` columns of each row never reach
 * num/den at all (and for `w <= 7` the row is dropped entirely). Both x86 8-bit
 * kernels close the same gap the same way. */
static FORCE_INLINE void vif_add_line_residuals(const VifPublicState *s, VifResiduals *totals,
                                                unsigned from, unsigned w, int scale)
{
    if (from == w)
        return;
    const VifResiduals residuals = vif_compute_line_residuals(s, from, w, scale);
    totals->accum_num_log += residuals.accum_num_log;
    totals->accum_den_log += residuals.accum_den_log;
    totals->accum_num_non_log += residuals.accum_num_non_log;
    totals->accum_den_non_log += residuals.accum_den_non_log;
}

static FORCE_INLINE void vif_store_num_den(const VifResiduals *totals, float *num, float *den)
{
    num[0] = totals->accum_num_log / 2048.0 +
             (totals->accum_den_non_log - ((totals->accum_num_non_log) / 16384.0) / (65025.0));
    den[0] = totals->accum_den_log / 2048.0 + totals->accum_den_non_log;
}

/* 16 pixels widened to u16, and their squares (u8 x u8 -> u16). */
typedef struct VifPixels8 {
    VifU16x8Pair px;
    VifU16x8Pair sq;
} VifPixels8;

static FORCE_INLINE VifPixels8 vif_load16_u8_sq(const uint8_t *src)
{
    const uint8x8_t lo = vld1_u8(src);
    const uint8x8_t hi = vld1_u8(src + 8);
    const VifPixels8 p = {{vmovl_u8(lo), vmovl_u8(hi)}, {vmull_u8(lo, lo), vmull_u8(hi, hi)}};
    return p;
}

typedef struct VifStat8Vertical {
    VifU32x4Quad mu1;
    VifU32x4Quad mu2;
    VifU32x4Quad ref_sq;
    VifU32x4Quad dis_sq;
    VifU32x4Quad ref_dis;
} VifStat8Vertical;

/* Cross moment in u32 lanes: f_dis * ref, where f_dis already holds dis * coeff. */
static FORCE_INLINE VifU32x4Quad vif_mul16_u16(VifU32x4Quad f_dis, VifU16x8Pair ref)
{
    const VifU32x4Quad product = {vif_mul_u32(f_dis.lo, vif_widen_u16(ref.lo)),
                                  vif_mul_u32(f_dis.hi, vif_widen_u16(ref.hi))};
    return product;
}

static FORCE_INLINE void vif_mla16_u16(VifU32x4Quad *acc, VifU32x4Quad f_dis, VifU16x8Pair ref)
{
    vif_mla_u32(&acc->lo, f_dis.lo, vif_widen_u16(ref.lo));
    vif_mla_u32(&acc->hi, f_dis.hi, vif_widen_u16(ref.hi));
}

/* First tap: plain products; the cross moment reuses the dis product. */
static FORCE_INLINE void vif_stat8_vertical_init(VifStat8Vertical *a, const uint8_t *ref,
                                                 const uint8_t *dis, uint16_t coeff)
{
    const VifPixels8 r = vif_load16_u8_sq(ref);
    const VifPixels8 d = vif_load16_u8_sq(dis);

    a->mu1 = vif_mull16_n_u16(r.px, coeff);
    a->mu2 = vif_mull16_n_u16(d.px, coeff);
    a->ref_sq = vif_mull16_n_u16(r.sq, coeff);
    a->dis_sq = vif_mull16_n_u16(d.sq, coeff);
    a->ref_dis = vif_mul16_u16(a->mu2, r.px);
}

static FORCE_INLINE void vif_stat8_vertical_tap(VifStat8Vertical *a, const uint8_t *ref,
                                                const uint8_t *dis, uint16_t coeff)
{
    const VifPixels8 r = vif_load16_u8_sq(ref);
    const VifPixels8 d = vif_load16_u8_sq(dis);
    const VifU32x4Quad f_dis = vif_mull16_n_u16(d.px, coeff);

    vif_mlal16_n_u16(&a->mu1, r.px, coeff);
    vif_mlal16_n_u16(&a->mu2, d.px, coeff);
    vif_mlal16_n_u16(&a->ref_sq, r.sq, coeff);
    vif_mlal16_n_u16(&a->dis_sq, d.sq, coeff);
    vif_mla16_u16(&a->ref_dis, f_dis, r.px);
}

/* 16 columns starting at `j`; `ref` / `dis` point at column `j` of the top tap row. */
static FORCE_INLINE void vif_stat8_vertical16(const VifBuffer *buf, const uint8_t *ref,
                                              const uint8_t *dis, unsigned j)
{
    const unsigned int fwidth = vif_filter1d_width[0];
    const uint16_t *vif_filt_s0 = vif_filter1d_table[0];
    const uint32x4_t offset_vec_v = vdupq_n_u32(128);
    const int32x4_t shift_vec_v = vdupq_n_s32(-8);
    VifStat8Vertical a;

    vif_stat8_vertical_init(&a, ref, dis, vif_filt_s0[0]);
    for (unsigned int fi = 1; fi < fwidth; ++fi) {
        const ptrdiff_t offset = (ptrdiff_t)fi * buf->stride;
        vif_stat8_vertical_tap(&a, ref + offset, dis + offset, vif_filt_s0[fi]);
    }
    vif_store_round_shift_u32(a.mu1.lo, offset_vec_v, shift_vec_v, buf->tmp.mu1 + j);
    vif_store_round_shift_u32(a.mu1.hi, offset_vec_v, shift_vec_v, buf->tmp.mu1 + j + 8);
    vif_store_round_shift_u32(a.mu2.lo, offset_vec_v, shift_vec_v, buf->tmp.mu2 + j);
    vif_store_round_shift_u32(a.mu2.hi, offset_vec_v, shift_vec_v, buf->tmp.mu2 + j + 8);
    vif_store16_u32(a.ref_sq, buf->tmp.ref + j);
    vif_store16_u32(a.dis_sq, buf->tmp.dis + j);
    vif_store16_u32(a.ref_dis, buf->tmp.ref_dis + j);
}

static FORCE_INLINE void vif_stat8_vertical_tail(const VifBuffer *buf, unsigned i, unsigned j)
{
    const unsigned int fwidth = vif_filter1d_width[0];
    const uint16_t *vif_filt_s0 = vif_filter1d_table[0];
    const uint8_t *ref = (const uint8_t *)buf->ref;
    const uint8_t *dis = (const uint8_t *)buf->dis;
    uint32_t accum_mu1 = 128;
    uint32_t accum_mu2 = 128;
    uint32_t accum_ref = 0;
    uint32_t accum_dis = 0;
    uint32_t accum_ref_dis = 0;

    for (unsigned fi = 0; fi < fwidth; ++fi) {
        const int ii = i - fwidth / 2;
        const int ii_check = ii + fi;
        const uint16_t fcoeff = vif_filt_s0[fi];
        const uint16_t imgcoeff_ref = ref[ii_check * buf->stride + j];
        const uint16_t imgcoeff_dis = dis[ii_check * buf->stride + j];
        const uint32_t img_coeff_ref = fcoeff * (uint32_t)imgcoeff_ref;
        const uint32_t img_coeff_dis = fcoeff * (uint32_t)imgcoeff_dis;
        accum_mu1 += img_coeff_ref;
        accum_mu2 += img_coeff_dis;
        accum_ref += img_coeff_ref * (uint32_t)imgcoeff_ref;
        accum_dis += img_coeff_dis * (uint32_t)imgcoeff_dis;
        accum_ref_dis += img_coeff_ref * (uint32_t)imgcoeff_dis;
    }
    buf->tmp.mu1[j] = accum_mu1 >> 8;
    buf->tmp.mu2[j] = accum_mu2 >> 8;
    buf->tmp.ref[j] = accum_ref;
    buf->tmp.dis[j] = accum_dis;
    buf->tmp.ref_dis[j] = accum_ref_dis;
}

static FORCE_INLINE void vif_stat8_vertical_row(const VifBuffer *buf, unsigned w, unsigned i)
{
    const unsigned int uiw15 = (w > 15 ? w - 15 : 0);
    const unsigned int fwidth = vif_filter1d_width[0];
    const int ii = i - fwidth / 2;
    const uint8_t *p_ref = (const uint8_t *)buf->ref + ii * buf->stride;
    const uint8_t *p_dis = (const uint8_t *)buf->dis + ii * buf->stride;

    unsigned int j = 0;
    for (; j < uiw15; j += 16)
        vif_stat8_vertical16(buf, p_ref + j, p_dis + j, j);
    for (; j < w; ++j)
        vif_stat8_vertical_tail(buf, i, j);
}

/* Research-2045: integer_vif.c assigns this function to VifState callbacks
 * taking VifPublicState *, shared with the scalar and other ISA implementations. */
// cppcheck-suppress constParameterPointer
void vif_statistic_8_neon(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h)
{
    VMAF_ASSERT_DEBUG(s != NULL);
    VMAF_ASSERT_DEBUG(num != NULL && den != NULL);
    VMAF_ASSERT_DEBUG(w > 0 && h > 0);
    const unsigned int uiw7 = (w > 7 ? w - 7 : 0);
    const unsigned int fwidth = vif_filter1d_width[0];
    VifBuffer buf = s->buf;
    VifResiduals totals = {0};

    for (unsigned i = 0; i < h; ++i) {
        vif_stat8_vertical_row(&buf, w, i);
        PADDING_SQ_DATA(&buf, w, fwidth / 2);

        unsigned int j = 0;
        for (; j < uiw7; j += 8)
            vif_stat_horizontal8(s, &buf, j, 0, &totals);
        vif_add_line_residuals(s, &totals, j, w, 0);
    }
    vif_store_num_den(&totals, num, den);
}

/* 8 pixels: raw u16, widened to u32, and squared in u32. */
typedef struct VifPixels16 {
    uint16x8_t px;
    VifU32x4Pair wide;
    VifU32x4Pair sq;
} VifPixels16;

static FORCE_INLINE VifPixels16 vif_load8_u16_sq(const uint16_t *src)
{
    VifPixels16 p;
    p.px = vld1q_u16(src);
    p.wide = vif_widen_u16(p.px);
    p.sq = vif_mul_u32(p.wide, p.wide);
    return p;
}

typedef struct VifStat16Vertical {
    VifU32x4Pair mu1;
    VifU32x4Pair mu2;
    VifU64x2Quad ref_sq;
    VifU64x2Quad dis_sq;
    VifU64x2Quad ref_dis;
} VifStat16Vertical;

/* First tap: plain products for the means; the second moments start from the
 * plan's rounding bias, and the cross moment reuses the dis product. */
static FORCE_INLINE void vif_stat16_vertical_init(VifStat16Vertical *a, const uint16_t *ref,
                                                  const uint16_t *dis, const VifFilterPlan *p)
{
    const VifPixels16 r = vif_load8_u16_sq(ref);
    const VifPixels16 d = vif_load8_u16_sq(dis);
    const uint16_t coeff = p->vif_filt[0];

    a->mu1 = vif_mull_n_u16(r.px, coeff);
    a->mu2 = vif_mull_n_u16(d.px, coeff);
    a->ref_sq = vif_splat_u64(vdupq_n_u64(p->add_shift_round_VP_sq));
    a->dis_sq = a->ref_sq;
    a->ref_dis = a->ref_sq;
    vif_mlal_n_u32(&a->ref_sq, r.sq, coeff);
    vif_mlal_n_u32(&a->dis_sq, d.sq, coeff);
    vif_mlal_u32(&a->ref_dis, a->mu2, r.wide);
}

static FORCE_INLINE void vif_stat16_vertical_tap(VifStat16Vertical *a, const uint16_t *ref,
                                                 const uint16_t *dis, uint16_t coeff)
{
    const VifPixels16 r = vif_load8_u16_sq(ref);
    const VifPixels16 d = vif_load8_u16_sq(dis);
    const VifU32x4Pair f_dis = vif_mull_n_u16(d.px, coeff);

    vif_mlal_n_u16(&a->mu1, r.px, coeff);
    vif_mlal_n_u16(&a->mu2, d.px, coeff);
    vif_mlal_n_u32(&a->ref_sq, r.sq, coeff);
    vif_mlal_n_u32(&a->dis_sq, d.sq, coeff);
    vif_mlal_u32(&a->ref_dis, f_dis, r.wide);
}

/* 8 columns starting at `j`; `ref` / `dis` point at column `j` of the top tap row. */
static FORCE_INLINE void vif_stat16_vertical8(const VifBuffer *buf, const uint16_t *ref,
                                              const uint16_t *dis, unsigned j,
                                              const VifFilterPlan *p)
{
    const ptrdiff_t stride_16 = buf->stride / sizeof(uint16_t);
    const uint32x4_t add_shift_round_VP_vec = vdupq_n_u32(p->add_shift_round_VP);
    const int32x4_t shift_VP_vec = vdupq_n_s32(-p->shift_VP);
    const int64x2_t shift_VP_sq_vec = vdupq_n_s64(-p->shift_VP_sq);
    VifStat16Vertical a;

    vif_stat16_vertical_init(&a, ref, dis, p);
    for (unsigned fi = 1; fi < p->fwidth; ++fi) {
        const ptrdiff_t offset = (ptrdiff_t)fi * stride_16;
        vif_stat16_vertical_tap(&a, ref + offset, dis + offset, p->vif_filt[fi]);
    }
    vif_store_round_shift_u32(a.mu1, add_shift_round_VP_vec, shift_VP_vec, buf->tmp.mu1 + j);
    vif_store_round_shift_u32(a.mu2, add_shift_round_VP_vec, shift_VP_vec, buf->tmp.mu2 + j);
    vif_store_narrow_u64(a.ref_sq, shift_VP_sq_vec, buf->tmp.ref + j);
    vif_store_narrow_u64(a.dis_sq, shift_VP_sq_vec, buf->tmp.dis + j);
    vif_store_narrow_u64(a.ref_dis, shift_VP_sq_vec, buf->tmp.ref_dis + j);
}

static FORCE_INLINE void vif_stat16_vertical_tail(const VifBuffer *buf, unsigned i, unsigned j,
                                                  const VifFilterPlan *p)
{
    const int32_t add_shift_round_VP = p->add_shift_round_VP;
    const int32_t add_shift_round_VP_sq = p->add_shift_round_VP_sq;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    const uint16_t *ref = (const uint16_t *)buf->ref;
    const uint16_t *dis = (const uint16_t *)buf->dis;
    uint32_t accum_mu1 = add_shift_round_VP;
    uint32_t accum_mu2 = add_shift_round_VP;
    uint64_t accum_ref = add_shift_round_VP_sq;
    uint64_t accum_dis = add_shift_round_VP_sq;
    uint64_t accum_ref_dis = add_shift_round_VP_sq;

    for (unsigned fi = 0; fi < p->fwidth; ++fi) {
        const int ii = i - p->fwidth / 2;
        const int ii_check = ii + fi;
        const uint16_t fcoeff = p->vif_filt[fi];
        const uint16_t imgcoeff_ref = ref[ii_check * stride + j];
        const uint16_t imgcoeff_dis = dis[ii_check * stride + j];
        const uint32_t img_coeff_ref = fcoeff * (uint32_t)imgcoeff_ref;
        const uint32_t img_coeff_dis = fcoeff * (uint32_t)imgcoeff_dis;
        accum_mu1 += img_coeff_ref;
        accum_mu2 += img_coeff_dis;
        accum_ref += img_coeff_ref * (uint64_t)imgcoeff_ref;
        accum_dis += img_coeff_dis * (uint64_t)imgcoeff_dis;
        accum_ref_dis += img_coeff_ref * (uint64_t)imgcoeff_dis;
    }
    buf->tmp.mu1[j] = (uint16_t)(accum_mu1 >> p->shift_VP);
    buf->tmp.mu2[j] = (uint16_t)(accum_mu2 >> p->shift_VP);
    buf->tmp.ref[j] = (uint32_t)(accum_ref >> p->shift_VP_sq);
    buf->tmp.dis[j] = (uint32_t)(accum_dis >> p->shift_VP_sq);
    buf->tmp.ref_dis[j] = (uint32_t)(accum_ref_dis >> p->shift_VP_sq);
}

static FORCE_INLINE void vif_stat16_vertical_row(const VifBuffer *buf, unsigned w, unsigned i,
                                                 const VifFilterPlan *p)
{
    const unsigned int uiw7 = (w > 7 ? w - 7 : 0);
    const ptrdiff_t stride_16 = buf->stride / sizeof(uint16_t);
    const int ii = i - p->fwidth / 2;
    const uint16_t *p_ref = (const uint16_t *)buf->ref + ii * stride_16;
    const uint16_t *p_dis = (const uint16_t *)buf->dis + ii * stride_16;

    unsigned int j = 0;
    for (; j < uiw7; j += 8)
        vif_stat16_vertical8(buf, p_ref + j, p_dis + j, j, p);
    for (; j < w; ++j)
        vif_stat16_vertical_tail(buf, i, j, p);
}

/* Research-2045: integer_vif.c assigns this function to VifState callbacks
 * taking VifPublicState *, shared with the scalar and other ISA implementations. */
// cppcheck-suppress constParameterPointer
void vif_statistic_16_neon(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h,
                           int bpc, int scale)
{
    const unsigned int uiw7 = (w > 7 ? w - 7 : 0);
    const VifFilterPlan plan = vif_filter_plan(scale, bpc, scale);
    VifBuffer buf = s->buf;
    VifResiduals totals = {0};

    for (unsigned i = 0; i < h; ++i) {
        vif_stat16_vertical_row(&buf, w, i, &plan);
        PADDING_SQ_DATA(&buf, w, plan.fwidth / 2);

        unsigned int j = 0;
        for (; j < uiw7; j += 8)
            vif_stat_horizontal8(s, &buf, j, scale, &totals);
        vif_add_line_residuals(s, &totals, j, w, scale);
    }
    vif_store_num_den(&totals, num, den);
}
