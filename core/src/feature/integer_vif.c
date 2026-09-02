/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <errno.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "cpu.h"
#include "dict.h"
#include "common/macros.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"

#include "picture.h"
#include "integer_vif.h"

#if ARCH_X86
#include "x86/vif_avx2.h"
#if HAVE_AVX512
#include "x86/vif_avx512.h"
#endif
#elif ARCH_AARCH64
#include "arm64/vif_neon.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is an
 * upstream-mirror file whose Netflix source spells the null pointer constant
 * `NULL` (every upstream sync would re-conflict against a keyword rewrite) and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct VifState {
    VifPublicState public;
    bool debug;
    bool vif_skip_scale0;
    void (*subsample_rd_8)(const VifBuffer *buf, unsigned w, unsigned h);
    void (*subsample_rd_16)(const VifBuffer *buf, unsigned w, unsigned h, int scale, int bpc);
    void (*vif_statistic_8)(VifPublicState *s, float *num, float *den, unsigned w, unsigned h);
    void (*vif_statistic_16)(VifPublicState *s, float *num, float *den, unsigned w, unsigned h,
                             int bpc, int scale);
    VmafDictionary *feature_name_dict;
} VifState;

static const VmafOption options[] = {{
                                         .name = "debug",
                                         .help = "debug mode: enable additional output",
                                         .offset = offsetof(VifState, debug),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                     },
                                     {
                                         .name = "vif_enhn_gain_limit",
                                         .alias = "egl",
                                         .help = "enhancement gain imposed on vif, must be >= 1.0, "
                                                 "where 1.0 means the gain is completely disabled",
                                         .offset = offsetof(VifState, public.vif_enhn_gain_limit),
                                         .type = VMAF_OPT_TYPE_DOUBLE,
                                         .default_val.d = DEFAULT_VIF_ENHN_GAIN_LIMIT,
                                         .min = 1.0,
                                         .max = DEFAULT_VIF_ENHN_GAIN_LIMIT,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {
                                         .name = "vif_skip_scale0",
                                         .alias = "ssclz",
                                         .help = "when set, skip scale 0 calculations",
                                         .offset = offsetof(VifState, vif_skip_scale0),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {0}};

static FORCE_INLINE void pad_top_and_bottom(const VifBuffer *buf, unsigned h, int fwidth)
{
    const unsigned fwidth_half = fwidth / 2;
    unsigned char *ref = buf->ref;
    unsigned char *dis = buf->dis;
    for (unsigned i = 1; i <= fwidth_half; ++i) {
        size_t offset = (size_t)buf->stride * i;
        memcpy(ref - offset, ref + offset, (size_t)buf->stride);
        memcpy(dis - offset, dis + offset, (size_t)buf->stride);
        memcpy(ref + (size_t)buf->stride * (h - 1) + (size_t)buf->stride * i,
               ref + (size_t)buf->stride * (h - 1) - (size_t)buf->stride * i, (size_t)buf->stride);
        memcpy(dis + (size_t)buf->stride * (h - 1) + (size_t)buf->stride * i,
               dis + (size_t)buf->stride * (h - 1) - (size_t)buf->stride * i, (size_t)buf->stride);
    }
}

static FORCE_INLINE void decimate_and_pad(const VifBuffer *buf, unsigned w, unsigned h, int scale)
{
    uint16_t *ref = buf->ref;
    uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    const ptrdiff_t mu_stride = buf->stride_16 / sizeof(uint16_t);

    for (unsigned i = 0; i < h / 2; ++i) {
        const ptrdiff_t src_row = (ptrdiff_t)i * 2 * mu_stride;
        for (unsigned j = 0; j < w / 2; ++j) {
            const ptrdiff_t src_col = (ptrdiff_t)j * 2;
            ref[i * stride + j] = buf->mu1[src_row + src_col];
            dis[i * stride + j] = buf->mu2[src_row + src_col];
        }
    }
    pad_top_and_bottom(buf, h / 2, vif_filter1d_width[scale]);
}

static void subsample_rd_8(const VifBuffer *buf, unsigned w, unsigned h)
{
    const unsigned fwidth = vif_filter1d_width[1];
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];

    for (unsigned i = 0; i < h; ++i) {
        //VERTICAL
        for (unsigned j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (unsigned fi = 0; fi < fwidth; ++fi) {
                int ii = i - fwidth / 2;
                int ii_check = ii + fi;
                const uint16_t fcoeff = vif_filt_s1[fi];
                const uint8_t *ref = (const uint8_t *)buf->ref;
                const uint8_t *dis = (const uint8_t *)buf->dis;
                accum_ref += fcoeff * (uint32_t)ref[ii_check * buf->stride + j];
                accum_dis += fcoeff * (uint32_t)dis[ii_check * buf->stride + j];
            }
            buf->tmp.ref_convol[j] = (accum_ref + 128) >> 8;
            buf->tmp.dis_convol[j] = (accum_dis + 128) >> 8;
        }

        PADDING_SQ_DATA_2(buf, w, fwidth / 2);

        //HORIZONTAL
        for (unsigned j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (unsigned fj = 0; fj < fwidth; ++fj) {
                int jj = j - fwidth / 2;
                int jj_check = jj + fj;
                const uint16_t fcoeff = vif_filt_s1[fj];
                accum_ref += fcoeff * buf->tmp.ref_convol[jj_check];
                accum_dis += fcoeff * buf->tmp.dis_convol[jj_check];
            }
            const ptrdiff_t stride = buf->stride_16 / sizeof(uint16_t);
            buf->mu1[i * stride + j] = (uint16_t)((accum_ref + 32768) >> 16);
            buf->mu2[i * stride + j] = (uint16_t)((accum_dis + 32768) >> 16);
        }
    }
    decimate_and_pad(buf, w, h, 0);
}

static void subsample_rd_16(const VifBuffer *buf, unsigned w, unsigned h, int scale, int bpc)
{
    const unsigned fwidth = vif_filter1d_width[scale + 1];
    const uint16_t *vif_filt = vif_filter1d_table[scale + 1];
    int32_t add_shift_round_VP;
    int32_t shift_VP;

    if (scale == 0) {
        add_shift_round_VP = 1 << (bpc - 1);
        shift_VP = bpc;
    } else {
        add_shift_round_VP = 32768;
        shift_VP = 16;
    }

    for (unsigned i = 0; i < h; ++i) {
        //VERTICAL
        for (unsigned j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (unsigned fi = 0; fi < fwidth; ++fi) {
                int ii = i - fwidth / 2;
                int ii_check = ii + fi;
                const uint16_t fcoeff = vif_filt[fi];
                const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
                const uint16_t *ref = buf->ref;
                const uint16_t *dis = buf->dis;
                accum_ref += fcoeff * ((uint32_t)ref[ii_check * stride + j]);
                accum_dis += fcoeff * ((uint32_t)dis[ii_check * stride + j]);
            }
            buf->tmp.ref_convol[j] = (uint16_t)((accum_ref + add_shift_round_VP) >> shift_VP);
            buf->tmp.dis_convol[j] = (uint16_t)((accum_dis + add_shift_round_VP) >> shift_VP);
        }

        PADDING_SQ_DATA_2(buf, w, fwidth / 2);

        //HORIZONTAL
        for (unsigned j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (unsigned fj = 0; fj < fwidth; ++fj) {
                int jj = j - fwidth / 2;
                int jj_check = jj + fj;
                const uint16_t fcoeff = vif_filt[fj];
                accum_ref += fcoeff * ((uint32_t)buf->tmp.ref_convol[jj_check]);
                accum_dis += fcoeff * ((uint32_t)buf->tmp.dis_convol[jj_check]);
            }
            const ptrdiff_t stride = buf->stride_16 / sizeof(uint16_t);
            buf->mu1[i * stride + j] = (uint16_t)((accum_ref + 32768) >> 16);
            buf->mu2[i * stride + j] = (uint16_t)((accum_dis + 32768) >> 16);
        }
    }
    decimate_and_pad(buf, w, h, scale);
}

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

static inline void log_generate(uint16_t *log2_table)
{
    /*
     * ADR-0500 LUT shrink: the table now has VIF_LOG2_TABLE_SIZE (16384) entries.
     * Entry i corresponds to the original entry at index (VIF_LOG2_TABLE_OFFSET + i),
     * i.e. log2(32768 + i) * 2048.  The scalar accessors log2_32 / log2_64 mask the
     * normalised mantissa with 0x7FFF (= VIF_LOG2_TABLE_SIZE - 1) to recover i.  The
     * AVX-512 gather path applies the same mask via _mm256_and_si256 before the gather.
     * Bit-exactness is preserved: same uint16 values, same arithmetic (roundf() of a
     * float below 2^15 is the same integer as round() of its double promotion).
     */
    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i) {
        log2_table[i] = (uint16_t)roundf(log2f((float)(VIF_LOG2_TABLE_OFFSET + i)) * 2048);
    }
}

/*
 * The scalar statistic is shared, line by line, between vif_statistic_8,
 * vif_statistic_16 and vif_compute_line_residuals (the tail helper the AVX2 /
 * AVX-512 / NEON kernels call for the columns their block width does not cover).
 * The three helpers below hold the arithmetic once so the SIMD twins stay
 * bit-exact against a single scalar reference:
 *   vif_horizontal_pixel   — horizontal filter pass, five moments of one pixel
 *   vif_accumulate_pixel   — log-domain / non-log accumulation of that pixel
 *   vif_store_residuals    — final num / den from the four accumulators
 * The expressions are verbatim upstream Netflix arithmetic; do not reorder them.
 */

/* Five filtered moments of one pixel after the horizontal pass. */
typedef struct VifPixelMoments {
    uint32_t mu1;
    uint32_t mu2;
    uint32_t xx;
    uint32_t yy;
    uint32_t xy;
} VifPixelMoments;

static FORCE_INLINE VifPixelMoments vif_horizontal_pixel(const VifBuffer *buf,
                                                         const uint16_t *vif_filt, unsigned fwidth,
                                                         unsigned j)
{
    uint32_t accum_mu1 = 0;
    uint32_t accum_mu2 = 0;
    uint64_t accum_ref = 0;
    uint64_t accum_dis = 0;
    uint64_t accum_ref_dis = 0;
    for (unsigned fj = 0; fj < fwidth; ++fj) {
        int jj = j - fwidth / 2;
        int jj_check = jj + fj;
        const uint16_t fcoeff = vif_filt[fj];
        accum_mu1 += fcoeff * ((uint32_t)buf->tmp.mu1[jj_check]);
        accum_mu2 += fcoeff * ((uint32_t)buf->tmp.mu2[jj_check]);
        accum_ref += fcoeff * ((uint64_t)buf->tmp.ref[jj_check]);
        accum_dis += fcoeff * ((uint64_t)buf->tmp.dis[jj_check]);
        accum_ref_dis += fcoeff * ((uint64_t)buf->tmp.ref_dis[jj_check]);
    }

    const VifPixelMoments m = {
        .mu1 = accum_mu1,
        .mu2 = accum_mu2,
        .xx = (uint32_t)((accum_ref + 32768) >> 16),
        .yy = (uint32_t)((accum_dis + 32768) >> 16),
        .xy = (uint32_t)((accum_ref_dis + 32768) >> 16),
    };
    return m;
}

static FORCE_INLINE void vif_accumulate_pixel(VifResiduals *acc, const uint16_t *log2_table,
                                              double vif_enhn_gain_limit, VifPixelMoments m)
{
    static const int32_t sigma_nsq = 65536 << 1;

    uint32_t mu1_sq_val = (uint32_t)((((uint64_t)m.mu1 * m.mu1) + 2147483648) >> 32);
    uint32_t mu2_sq_val = (uint32_t)((((uint64_t)m.mu2 * m.mu2) + 2147483648) >> 32);
    uint32_t mu1_mu2_val = (uint32_t)((((uint64_t)m.mu1 * m.mu2) + 2147483648) >> 32);

    int32_t sigma1_sq = (int32_t)(m.xx - mu1_sq_val);
    int32_t sigma2_sq = (int32_t)(m.yy - mu2_sq_val);
    int32_t sigma12 = (int32_t)(m.xy - mu1_mu2_val);

    sigma2_sq = MAX(sigma2_sq, 0);
    if (sigma1_sq >= sigma_nsq) {
        /**
        * log values are taken from the look-up table generated by
        * log_generate() function which is called in integer_combo_threadfunc
        * den_val in float is log2(1 + sigma1_sq/2)
        * here it is converted to equivalent of log2(2+sigma1_sq) - log2(2) i.e log2(2*65536+sigma1_sq) - 17
        * multiplied by 2048 as log_value = log2(i)*2048 i=16384 to 65535 generated using log_value
        * x because best 16 bits are taken
        */
        acc->accum_den_log += log2_32(log2_table, sigma_nsq + sigma1_sq) - 2048 * 17;

        if (sigma12 > 0 && sigma2_sq > 0) {
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

            g = MIN(g, vif_enhn_gain_limit);

            uint32_t numer1 = (sv_sq + sigma_nsq);
            int64_t numer1_tmp = (int64_t)((g * g * sigma1_sq)) + numer1; //numerator
            acc->accum_num_log += log2_64(log2_table, numer1_tmp) - log2_64(log2_table, numer1);
        }
    } else {
        acc->accum_num_non_log += sigma2_sq;
        acc->accum_den_non_log++;
    }
}

static void vif_store_residuals(const VifResiduals *acc, float *num, float *den)
{
    num[0] = acc->accum_num_log / 2048.0 +
             (acc->accum_den_non_log - ((acc->accum_num_non_log) / 16384.0) / (65025.0));
    den[0] = acc->accum_den_log / 2048.0 + acc->accum_den_non_log;
}

/* Vertical filter pass of source row i for the 8-bit scale-0 statistic. */
static FORCE_INLINE void vif_vertical_line_8(const VifBuffer *buf, const uint16_t *vif_filt_s0,
                                             unsigned fwidth, unsigned w, unsigned i)
{
    const uint8_t *ref = (const uint8_t *)buf->ref;
    const uint8_t *dis = (const uint8_t *)buf->dis;
    for (unsigned j = 0; j < w; ++j) {
        uint32_t accum_mu1 = 0;
        uint32_t accum_mu2 = 0;
        uint32_t accum_ref = 0;
        uint32_t accum_dis = 0;
        uint32_t accum_ref_dis = 0;
        for (unsigned fi = 0; fi < fwidth; ++fi) {
            int ii = i - fwidth / 2;
            int ii_check = ii + fi;
            const uint16_t fcoeff = vif_filt_s0[fi];
            uint16_t imgcoeff_ref = ref[ii_check * buf->stride + j];
            uint16_t imgcoeff_dis = dis[ii_check * buf->stride + j];
            uint32_t img_coeff_ref = fcoeff * (uint32_t)imgcoeff_ref;
            uint32_t img_coeff_dis = fcoeff * (uint32_t)imgcoeff_dis;
            accum_mu1 += img_coeff_ref;
            accum_mu2 += img_coeff_dis;
            accum_ref += img_coeff_ref * (uint32_t)imgcoeff_ref;
            accum_dis += img_coeff_dis * (uint32_t)imgcoeff_dis;
            accum_ref_dis += img_coeff_ref * (uint32_t)imgcoeff_dis;
        }
        buf->tmp.mu1[j] = (accum_mu1 + 128) >> 8;
        buf->tmp.mu2[j] = (accum_mu2 + 128) >> 8;
        buf->tmp.ref[j] = accum_ref;
        buf->tmp.dis[j] = accum_dis;
        buf->tmp.ref_dis[j] = accum_ref_dis;
    }
}

void vif_statistic_8(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h)
{
    const unsigned fwidth = vif_filter1d_width[0];
    const uint16_t *vif_filt_s0 = vif_filter1d_table[0];
    const VifBuffer *buf = &s->buf;
    VifResiduals acc = {0};

    for (unsigned i = 0; i < h; ++i) {
        vif_vertical_line_8(buf, vif_filt_s0, fwidth, w, i);

        PADDING_SQ_DATA(buf, w, fwidth / 2);

        for (unsigned j = 0; j < w; ++j) {
            const VifPixelMoments m = vif_horizontal_pixel(buf, vif_filt_s0, fwidth, j);
            vif_accumulate_pixel(&acc, s->log2_table, s->vif_enhn_gain_limit, m);
        }
    }
    vif_store_residuals(&acc, num, den);
}

/* Rounding / shift constants of the 16-bit vertical pass for one (scale, bpc). */
typedef struct VifShift {
    int32_t add_shift_round_VP;
    int32_t shift_VP;
    int32_t add_shift_round_VP_sq;
    int32_t shift_VP_sq;
} VifShift;

static VifShift vif_shift_for_scale(int scale, int bpc)
{
    VifShift sh;
    if (scale == 0) {
        sh.shift_VP = bpc;
        sh.add_shift_round_VP = 1 << (bpc - 1);
        sh.shift_VP_sq = (bpc - 8) * 2;
        sh.add_shift_round_VP_sq = (bpc == 8) ? 0 : 1 << (sh.shift_VP_sq - 1);
    } else {
        sh.shift_VP = 16;
        sh.add_shift_round_VP = 32768;
        sh.shift_VP_sq = 16;
        sh.add_shift_round_VP_sq = 32768;
    }
    return sh;
}

/* Vertical filter pass of source row i for the 16-bit (scale > 0 or bpc > 8) statistic. */
static FORCE_INLINE void vif_vertical_line_16(const VifBuffer *buf, const uint16_t *vif_filt,
                                              unsigned fwidth, unsigned w, unsigned i, VifShift sh)
{
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    const uint16_t *ref = buf->ref;
    const uint16_t *dis = buf->dis;
    for (unsigned j = 0; j < w; ++j) {
        uint32_t accum_mu1 = 0;
        uint32_t accum_mu2 = 0;
        uint64_t accum_ref = 0;
        uint64_t accum_dis = 0;
        uint64_t accum_ref_dis = 0;
        for (unsigned fi = 0; fi < fwidth; ++fi) {
            int ii = i - fwidth / 2;
            int ii_check = ii + fi;
            const uint16_t fcoeff = vif_filt[fi];
            uint16_t imgcoeff_ref = ref[ii_check * stride + j];
            uint16_t imgcoeff_dis = dis[ii_check * stride + j];
            uint32_t img_coeff_ref = fcoeff * (uint32_t)imgcoeff_ref;
            uint32_t img_coeff_dis = fcoeff * (uint32_t)imgcoeff_dis;
            accum_mu1 += img_coeff_ref;
            accum_mu2 += img_coeff_dis;
            accum_ref += img_coeff_ref * (uint64_t)imgcoeff_ref;
            accum_dis += img_coeff_dis * (uint64_t)imgcoeff_dis;
            accum_ref_dis += img_coeff_ref * (uint64_t)imgcoeff_dis;
        }
        buf->tmp.mu1[j] = (uint16_t)((accum_mu1 + sh.add_shift_round_VP) >> sh.shift_VP);
        buf->tmp.mu2[j] = (uint16_t)((accum_mu2 + sh.add_shift_round_VP) >> sh.shift_VP);
        buf->tmp.ref[j] = (uint32_t)((accum_ref + sh.add_shift_round_VP_sq) >> sh.shift_VP_sq);
        buf->tmp.dis[j] = (uint32_t)((accum_dis + sh.add_shift_round_VP_sq) >> sh.shift_VP_sq);
        buf->tmp.ref_dis[j] =
            (uint32_t)((accum_ref_dis + sh.add_shift_round_VP_sq) >> sh.shift_VP_sq);
    }
}

void vif_statistic_16(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h,
                      int bpc, int scale)
{
    const unsigned fwidth = vif_filter1d_width[scale];
    const uint16_t *vif_filt = vif_filter1d_table[scale];
    const VifBuffer *buf = &s->buf;
    const VifShift sh = vif_shift_for_scale(scale, bpc);
    VifResiduals acc = {0};

    for (unsigned i = 0; i < h; ++i) {
        vif_vertical_line_16(buf, vif_filt, fwidth, w, i, sh);

        PADDING_SQ_DATA(buf, w, fwidth / 2);

        for (unsigned j = 0; j < w; ++j) {
            const VifPixelMoments m = vif_horizontal_pixel(buf, vif_filt, fwidth, j);
            vif_accumulate_pixel(&acc, s->log2_table, s->vif_enhn_gain_limit, m);
        }
    }
    vif_store_residuals(&acc, num, den);
}

VifResiduals vif_compute_line_residuals(const VifPublicState *s, unsigned from, unsigned to,
                                        int scale)
{
    VifResiduals residuals = {0};
    const unsigned fwidth = vif_filter1d_width[scale];
    const uint16_t *vif_filt = vif_filter1d_table[scale];

    //HORIZONTAL
    for (unsigned j = from; j < to; ++j) {
        const VifPixelMoments m = vif_horizontal_pixel(&s->buf, vif_filt, fwidth, j);
        vif_accumulate_pixel(&residuals, s->log2_table, s->vif_enhn_gain_limit, m);
    }
    return residuals;
}

static void vif_init_dispatch(VifState *s)
{
    s->subsample_rd_8 = subsample_rd_8;
    s->subsample_rd_16 = subsample_rd_16;
    s->vif_statistic_8 = vif_statistic_8;
    s->vif_statistic_16 = vif_statistic_16;

#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        s->subsample_rd_8 = vif_subsample_rd_8_avx2;
        s->subsample_rd_16 = vif_subsample_rd_16_avx2;
        s->vif_statistic_8 = vif_statistic_8_avx2;
        s->vif_statistic_16 = vif_statistic_16_avx2;
    }
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        s->subsample_rd_8 = vif_subsample_rd_8_avx512;
        s->subsample_rd_16 = vif_subsample_rd_16_avx512;
        s->vif_statistic_8 = vif_statistic_8_avx512;
        s->vif_statistic_16 = vif_statistic_16_avx512;
    }
#endif
#elif ARCH_AARCH64
    const unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_ARM_CPU_FLAG_NEON) {
        s->subsample_rd_8 = vif_subsample_rd_8_neon;
        s->subsample_rd_16 = vif_subsample_rd_16_neon;
        s->vif_statistic_8 = vif_statistic_8_neon;
        s->vif_statistic_16 = vif_statistic_16_neon;
    }
#endif
}

/*
 * Carve the single aligned allocation into the padded ref / dis frames, the
 * decimated mu planes, the five per-line 32-bit rows and the seven padded
 * scratch rows.  Walk the one allocation as a byte cursor: the original
 * upstream form used `void *data` and relied on the GCC extension that treats
 * pointer arithmetic on void* as byte-wise (sizeof(void)==1).  MSVC rejects
 * that with C2036, so use uint8_t* and cast explicitly at assignments to typed
 * pointers.  Byte offsets are identical.
 */
static int vif_buffers_alloc(VifBuffer *buf, unsigned w, unsigned h, unsigned bpc)
{
    const bool hbd = bpc > 8;

    buf->stride = ALIGN_CEIL(w << hbd);
    buf->stride_16 = ALIGN_CEIL(w * sizeof(uint16_t));
    buf->stride_32 = ALIGN_CEIL(w * sizeof(uint32_t));
    buf->stride_tmp = ALIGN_CEIL((MAX_ALIGN + w + MAX_ALIGN) * sizeof(uint32_t));
    const size_t frame_size = buf->stride * h;
    const size_t pad_size = buf->stride * 8;
    const size_t data_sz = 2 * (pad_size + frame_size + pad_size) + 2 * (h * buf->stride_16) +
                           5 * (buf->stride_32) + 7 * buf->stride_tmp;
    uint8_t *data = aligned_malloc(data_sz, MAX_ALIGN);
    if (!data)
        return -ENOMEM;
    memset(data, 0, data_sz);

    buf->data = data;
    data += pad_size;
    buf->ref = data;
    data += frame_size + pad_size + pad_size;
    buf->dis = data;
    data += frame_size + pad_size;
    buf->mu1 = (uint16_t *)data;
    data += h * buf->stride_16;
    buf->mu2 = (uint16_t *)data;
    data += h * buf->stride_16;
    buf->mu1_32 = (uint32_t *)data;
    data += buf->stride_32;
    buf->mu2_32 = (uint32_t *)data;
    data += buf->stride_32;
    buf->ref_sq = (uint32_t *)data;
    data += buf->stride_32;
    buf->dis_sq = (uint32_t *)data;
    data += buf->stride_32;
    buf->ref_dis = (uint32_t *)data;
    data += buf->stride_32;
    buf->tmp.mu1 = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.mu2 = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.ref = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.dis = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.ref_dis = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.ref_convol = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.dis_convol = (uint32_t *)data;
    return 0;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;
    VifState *s = fex->priv;

    vif_init_dispatch(s);
    log_generate(s->public.log2_table);

    const int err = vif_buffers_alloc(&s->public.buf, w, h, bpc);
    if (err)
        return err;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        aligned_free(s->public.buf.data);
        s->public.buf.data = NULL;
        return -ENOMEM;
    }

    return 0;
}

typedef struct VifScore {
    struct {
        float num;
        float den;
    } scale[4];
} VifScore;

static const char *const vif_scale_score_names[4] = {
    "VMAF_integer_feature_vif_scale0_score", "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score", "VMAF_integer_feature_vif_scale3_score"};

static const char *const vif_scale_num_names[4] = {
    "integer_vif_num_scale0", "integer_vif_num_scale1", "integer_vif_num_scale2",
    "integer_vif_num_scale3"};

static const char *const vif_scale_den_names[4] = {
    "integer_vif_den_scale0", "integer_vif_den_scale1", "integer_vif_den_scale2",
    "integer_vif_den_scale3"};

static int write_scale_scores(VmafFeatureCollector *feature_collector, unsigned index,
                              const VifScore *vif, const VifState *s)
{
    int err = 0;

    const float scale0 = s->vif_skip_scale0 ? 0.0f : vif->scale[0].num / vif->scale[0].den;
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   vif_scale_score_names[0], scale0, index);

    for (unsigned k = 1; k < 4; ++k) {
        err |= vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, vif_scale_score_names[k],
            vif->scale[k].num / vif->scale[k].den, index);
    }

    return err;
}

static int write_debug_scores(VmafFeatureCollector *feature_collector, unsigned index,
                              const VifScore *vif, const VifState *s)
{
    int err = 0;
    double score_num;
    double score_den;

    if (s->vif_skip_scale0) {
        score_num =
            (double)vif->scale[1].num + (double)vif->scale[2].num + (double)vif->scale[3].num;
        score_den =
            (double)vif->scale[1].den + (double)vif->scale[2].den + (double)vif->scale[3].den;
    } else {
        score_num = (double)vif->scale[0].num + (double)vif->scale[1].num +
                    (double)vif->scale[2].num + (double)vif->scale[3].num;

        score_den = (double)vif->scale[0].den + (double)vif->scale[1].den +
                    (double)vif->scale[2].den + (double)vif->scale[3].den;
    }

    const double score = score_den == 0.0 ? 1.0f : score_num / score_den;

    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif", score, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_num", score_num, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_den", score_den, index);

    /* scale 0 is reported as (0, -1) when skipped */
    const float num_scale0 = s->vif_skip_scale0 ? 0.0f : vif->scale[0].num;
    const float den_scale0 = s->vif_skip_scale0 ? -1.0f : vif->scale[0].den;
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   vif_scale_num_names[0], num_scale0, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   vif_scale_den_names[0], den_scale0, index);

    for (unsigned k = 1; k < 4; ++k) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       vif_scale_num_names[k], vif->scale[k].num,
                                                       index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       vif_scale_den_names[k], vif->scale[k].den,
                                                       index);
    }

    return err;
}

static int write_scores(VmafFeatureCollector *feature_collector, unsigned index,
                        const VifScore *vif, const VifState *s)
{
    int err = write_scale_scores(feature_collector, index, vif, s);

    if (!s->debug)
        return err;

    err |= write_debug_scores(feature_collector, index, vif, s);
    return err;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    VifState *s = fex->priv;

    (void)ref_pic_90;
    (void)dist_pic_90;

    unsigned w = ref_pic->w[0];
    unsigned h = dist_pic->h[0];

    unsigned char *ref_in = ref_pic->data[0];
    unsigned char *dis_in = dist_pic->data[0];
    unsigned char *ref_out = s->public.buf.ref;
    unsigned char *dis_out = s->public.buf.dis;

    for (unsigned i = 0; i < h; i++) {
        memcpy(ref_out, ref_in, ref_pic->stride[0]);
        memcpy(dis_out, dis_in, dist_pic->stride[0]);
        ref_in += ref_pic->stride[0];
        dis_in += dist_pic->stride[0];
        ref_out += s->public.buf.stride;
        dis_out += s->public.buf.stride;
    }
    pad_top_and_bottom(&s->public.buf, h, vif_filter1d_width[0]);

    unsigned scale_start = 0;
    if (s->vif_skip_scale0) {
        scale_start = 1;
    }
    VifScore vif_score = {0};
    for (unsigned scale = scale_start; scale < 4; ++scale) {
        if (scale > 0) {
            if (ref_pic->bpc == 8 && scale == 1) {
                s->subsample_rd_8(&s->public.buf, w, h);
            } else {
                s->subsample_rd_16(&s->public.buf, w, h, scale - 1, ref_pic->bpc);
            }

            w /= 2;
            h /= 2;
        }

        if (ref_pic->bpc == 8 && scale == 0) {
            s->vif_statistic_8(&s->public, &vif_score.scale[scale].num, &vif_score.scale[scale].den,
                               w, h);
        } else {
            s->vif_statistic_16(&s->public, &vif_score.scale[scale].num,
                                &vif_score.scale[scale].den, w, h, ref_pic->bpc, scale);
        }
    }

    return write_scores(feature_collector, index, &vif_score, s);
}

static int close(VmafFeatureExtractor *fex)
{
    VifState *s = fex->priv;
    if (s->public.buf.data)
        aligned_free(s->public.buf.data);
    vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features[] = {"VMAF_integer_feature_vif_scale0_score",
                                          "VMAF_integer_feature_vif_scale1_score",
                                          "VMAF_integer_feature_vif_scale2_score",
                                          "VMAF_integer_feature_vif_scale3_score",
                                          "integer_vif",
                                          "integer_vif_num",
                                          "integer_vif_den",
                                          "integer_vif_num_scale0",
                                          "integer_vif_den_scale0",
                                          "integer_vif_num_scale1",
                                          "integer_vif_den_scale1",
                                          "integer_vif_num_scale2",
                                          "integer_vif_den_scale2",
                                          "integer_vif_num_scale3",
                                          "integer_vif_den_scale3",
                                          NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_integer_vif` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_vif = {
    .name = "vif",
    .init = init,
    .extract = extract,
    .options = options,
    .close = close,
    .priv_size = sizeof(VifState),
    .provided_features = provided_features,
    /* 4 scales × 1 dispatch per scale on GPU backends (see ADR-0181). */
    .chars =
        {
            .n_dispatches_per_frame = 4,
            .is_reduction_only = false,
            .min_useful_frame_area = 1280U * 720U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
