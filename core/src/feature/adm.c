/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
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

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "log.h"
#include "mem.h"
#include "adm.h"
#include "adm_score.h"
#include "nonfinite_score.h"
#include "adm_options.h"
#include "adm_tools.h"
#include "offset.h"
#include "cpu.h"
#if ARCH_AARCH64
#include "arm64/float_adm_neon.h"
#endif
#if ARCH_X86
#include "x86/float_adm_avx2.h"
#if HAVE_AVX512
#include "x86/float_adm_avx512.h"
#endif
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

typedef adm_dwt_band_t_s adm_dwt_band_t;

#define adm_dwt2_lo adm_dwt2_lo_s
#define adm_decouple adm_decouple_s
#define adm_cm_thresh adm_cm_thresh_s
#define adm_cm adm_cm_s
#define adm_sum_cube adm_sum_cube_s
#define offset_image offset_image_s

#define adm_csf_den_scale adm_csf_den_scale_s

/* The wavelet and the CSF stage run through a SIMD kernel when the processor
 * has one, chosen per call from vmaf_get_cpu_flags() as the fork's other
 * float extractors choose theirs (so `--cpumask` selects the path). Every
 * kernel returns the scalar function's bits: float_adm_dwt2_neon() (its own
 * translation unit with contraction off, ADR-1057), float_adm_dwt2_avx2() /
 * float_adm_dwt2_avx512() and float_adm_csf_avx2() / float_adm_csf_avx512()
 * (strict floating-point arguments, ADR-1415). */
#define dwt2_src_indices_filt dwt2_src_indices_filt_s

static int adm_dwt2_dispatch(const float *src, const adm_dwt_band_t_s *dst, int **ind_y,
                             int **ind_x, int w, int h, int src_stride, int dst_stride)
{
#if ARCH_AARCH64
    /* The NEON kernel returns nothing: it leaves the bands untouched when its
     * row buffers cannot be allocated. */
    if (vmaf_get_cpu_flags() & VMAF_ARM_CPU_FLAG_NEON) {
        float_adm_dwt2_neon(src, dst, ind_y, ind_x, w, h, src_stride, dst_stride);
        return 0;
    }
#endif
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags();
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        return float_adm_dwt2_avx512(src, dst, ind_y, ind_x, w, h, src_stride, dst_stride);
    }
#endif
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        return float_adm_dwt2_avx2(src, dst, ind_y, ind_x, w, h, src_stride, dst_stride);
    }
#endif
    return adm_dwt2_s(src, dst, ind_y, ind_x, w, h, src_stride, dst_stride);
}

/* The band kernel of the CSF stage for this processor. */
static adm_csf_plane_fn adm_csf_plane_select(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags();
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        return float_adm_csf_avx512;
    }
#endif
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        return float_adm_csf_avx2;
    }
#endif
    return adm_csf_plane_s;
}

/* adm_csf_s()'s arguments, with the band kernel appended. */
#define adm_csf(...) adm_csf_planes_s(__VA_ARGS__, adm_csf_plane_select())

#define adm_dwt2 adm_dwt2_dispatch

/* The band planes are carved out of one aligned buffer in steps of band_len
 * samples: buf_sz_one bytes, a multiple of MAX_ALIGN and so of the sample
 * size. The cursor has the sample type, so no plane is reached through a
 * cast: cppcheck reports `(float *)` on a `char *` cursor as
 * invalidPointerCast, and clang-tidy reports the `(void *)` hop that would
 * quiet it as bugprone-casting-through-void. */
static float *init_dwt_band(adm_dwt_band_t *band, float *data_top, size_t band_len)
{
    band->band_a = data_top;
    data_top += band_len;
    band->band_h = data_top;
    data_top += band_len;
    band->band_v = data_top;
    data_top += band_len;
    band->band_d = data_top;
    data_top += band_len;
    return data_top;
}

UNUSED_FUNCTION
static double *init_dwt_band_d(adm_dwt_band_t_d *band, double *data_top, size_t band_len)
{
    band->band_a = data_top;
    data_top += band_len;
    band->band_h = data_top;
    data_top += band_len;
    band->band_v = data_top;
    data_top += band_len;
    band->band_d = data_top;
    data_top += band_len;
    return data_top;
}

static float *init_dwt_band_hvd(adm_dwt_band_t *band, float *data_top, size_t band_len)
{
    band->band_a = NULL;
    band->band_h = data_top;
    data_top += band_len;
    band->band_v = data_top;
    data_top += band_len;
    band->band_d = data_top;
    data_top += band_len;
    return data_top;
}

// Code optimized to save on multiple buffer copies
// hence the reduction in the number of buffers required from 35 to 17
#define NUM_BUFS_ADM 20

/* What compute_adm() allocates for one frame: the band planes, carved out of
 * data_buf, and the wavelet's two index tables. */
typedef struct AdmFrameBufs {
    float *data_buf;
    char *buf_y_orig;
    char *buf_x_orig;
    int *ind_y[4];
    int *ind_x[4];
    adm_dwt_band_t ref_dwt2;
    adm_dwt_band_t dis_dwt2;
    adm_dwt_band_t decouple_r;
    adm_dwt_band_t decouple_a;
    adm_dwt_band_t csf_a;
    adm_dwt_band_t csf_f; //Store filtered coeffs
    int buf_stride;
} AdmFrameBufs;

/* compute_adm()'s picture arguments. */
typedef struct AdmFrameIn {
    const float *ref;
    const float *dis;
    int w;
    int h;
    int ref_stride;
    int dis_stride;
} AdmFrameIn;

/* compute_adm()'s option arguments, as the per-scale kernels take them. */
typedef struct AdmScaleOpts {
    double border_factor;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double luminance_level;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    int adm_bypass_cm;
    double adm_p_norm;
    double adm_f1s0;
    double adm_f1s1;
    double adm_f1s2;
    double adm_f1s3;
    double adm_f2s0;
    double adm_f2s1;
    double adm_f2s2;
    double adm_f2s3;
} AdmScaleOpts;

/* The band planes. Returns 0, or 1 after upstream's message on stdout. */
static int adm_alloc_bands(AdmFrameBufs *b, int w, int h)
{
    b->buf_stride = ALIGN_CEIL(((w + 1) / 2) * sizeof(float));
    const size_t buf_sz_one = (size_t)b->buf_stride * ((h + 1) / 2);

    if (SIZE_MAX / buf_sz_one < NUM_BUFS_ADM) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "error: SIZE_MAX / buf_sz_one < NUM_BUFS_ADM, buf_sz_one = %zu.\n", buf_sz_one);
        return 1;
    }

    b->data_buf = aligned_malloc(buf_sz_one * NUM_BUFS_ADM, MAX_ALIGN);
    if (!b->data_buf) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "error: aligned_malloc failed for data_buf.\n");
        return 1;
    }

    const size_t band_len = buf_sz_one / sizeof(float);
    float *data_top = b->data_buf;

    data_top = init_dwt_band(&b->ref_dwt2, data_top, band_len);
    data_top = init_dwt_band(&b->dis_dwt2, data_top, band_len);
    data_top = init_dwt_band_hvd(&b->decouple_r, data_top, band_len);
    data_top = init_dwt_band_hvd(&b->decouple_a, data_top, band_len);
    data_top = init_dwt_band_hvd(&b->csf_a, data_top, band_len);
    (void)init_dwt_band_hvd(&b->csf_f, data_top, band_len);
    return 0;
}

/* One index table: four rows of `row_bytes` each. `name` is upstream's name of
 * the buffer in its message. Returns 0, or 1 after that message on stdout. */
static int adm_alloc_indices(char **orig, int *ind[4], int row_bytes, const char *name)
{
    *orig = aligned_malloc((size_t)row_bytes * 4, MAX_ALIGN);
    if (!*orig) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "error: aligned_malloc failed for %s.\n", name);
        return 1;
    }
    char *row = *orig;
    for (int i = 0; i < 4; i++) {
        ind[i] = (int *)row;
        row += row_bytes;
    }
    return 0;
}

/* Every buffer of one frame, in upstream's order. Returns 0 or 1; the caller
 * frees in both cases. */
static int adm_frame_alloc(AdmFrameBufs *b, int w, int h)
{
    const int ind_size_y = ALIGN_CEIL(((h + 1) / 2) * sizeof(int));
    const int ind_size_x = ALIGN_CEIL(((w + 1) / 2) * sizeof(int));

    if (adm_alloc_bands(b, w, h))
        return 1;
    if (adm_alloc_indices(&b->buf_y_orig, b->ind_y, ind_size_y, "ind_buf_y"))
        return 1;
    return adm_alloc_indices(&b->buf_x_orig, b->ind_x, ind_size_x, "ind_buf_x");
}

static void adm_frame_free(AdmFrameBufs *b)
{
    aligned_free(b->data_buf);
    aligned_free(b->buf_y_orig);
    aligned_free(b->buf_x_orig);
}

/* The wavelet of one scale, reference then distorted: the low band alone when
 * the scale is skipped. Returns 0, or -1 when a transform fails. */
static int adm_scale_dwt2(AdmFrameBufs *b, bool low_band_only, const float *ref, const float *dis,
                          int w, int h, int ref_stride, int dis_stride)
{
    if (low_band_only) {
        if (adm_dwt2_lo(ref, &b->ref_dwt2, b->ind_y, b->ind_x, w, h, ref_stride, b->buf_stride) < 0)
            return -1;
        if (adm_dwt2_lo(dis, &b->dis_dwt2, b->ind_y, b->ind_x, w, h, dis_stride, b->buf_stride) < 0)
            return -1;
        return 0;
    }
    if (adm_dwt2(ref, &b->ref_dwt2, b->ind_y, b->ind_x, w, h, ref_stride, b->buf_stride) < 0)
        return -1;
    if (adm_dwt2(dis, &b->dis_dwt2, b->ind_y, b->ind_x, w, h, dis_stride, b->buf_stride) < 0)
        return -1;
    return 0;
}

/* The three sums of a scale that is not skipped, with upstream's calls in
 * upstream's order: decouple, denominator, CSF of the additive part, detail
 * numerator, CSF of the restored part, additive-impairment numerator. `w` and
 * `h` are the band's size. */
static void adm_scale_sums(const AdmFrameBufs *b, const AdmScaleOpts *o, int orig_h, int scale,
                           int w, int h, float *num_scale, float *den_scale, float *aim_num_scale)
{
    const int buf_stride = b->buf_stride;

    adm_decouple(&b->ref_dwt2, &b->dis_dwt2, &b->decouple_r, &b->decouple_a, w, h, buf_stride,
                 buf_stride, buf_stride, buf_stride, o->border_factor, o->adm_enhn_gain_limit);

    *den_scale = adm_csf_den_scale(
        &b->ref_dwt2, orig_h, scale, w, h, buf_stride, o->border_factor, o->adm_norm_view_dist,
        o->adm_ref_display_height, o->adm_csf_mode, o->luminance_level, o->adm_csf_scale,
        o->adm_csf_diag_scale, o->adm_noise_weight, o->adm_p_norm, o->adm_f1s0, o->adm_f1s1,
        o->adm_f1s2, o->adm_f1s3, o->adm_f2s0, o->adm_f2s1, o->adm_f2s2, o->adm_f2s3);

    adm_csf(&b->decouple_a, &b->csf_a, &b->csf_f, orig_h, scale, w, h, buf_stride, buf_stride,
            o->border_factor, o->adm_norm_view_dist, o->adm_ref_display_height, o->adm_csf_mode,
            o->luminance_level, o->adm_csf_scale, o->adm_csf_diag_scale, o->adm_f1s0, o->adm_f1s1,
            o->adm_f1s2, o->adm_f1s3, o->adm_f2s0, o->adm_f2s1, o->adm_f2s2, o->adm_f2s3);

    *num_scale =
        adm_cm(&b->decouple_r, &b->csf_f, &b->csf_a, w, h, buf_stride, buf_stride, buf_stride,
               o->border_factor, scale, o->adm_norm_view_dist, o->adm_ref_display_height,
               o->adm_csf_mode, o->luminance_level, o->adm_csf_scale, o->adm_csf_diag_scale,
               o->adm_noise_weight, o->adm_bypass_cm, o->adm_p_norm, o->adm_f1s0, o->adm_f1s1,
               o->adm_f1s2, o->adm_f1s3, o->adm_f2s0, o->adm_f2s1, o->adm_f2s2, o->adm_f2s3);

    adm_csf(&b->decouple_r, &b->csf_f, &b->csf_a, orig_h, scale, w, h, buf_stride, buf_stride,
            o->border_factor, o->adm_norm_view_dist, o->adm_ref_display_height, o->adm_csf_mode,
            o->luminance_level, o->adm_csf_scale, o->adm_csf_diag_scale, o->adm_f1s0, o->adm_f1s1,
            o->adm_f1s2, o->adm_f1s3, o->adm_f2s0, o->adm_f2s1, o->adm_f2s2, o->adm_f2s3);

    *aim_num_scale =
        adm_cm(&b->decouple_a, &b->csf_a, &b->csf_f, w, h, buf_stride, buf_stride, buf_stride,
               o->border_factor, scale, o->adm_norm_view_dist, o->adm_ref_display_height,
               o->adm_csf_mode, o->luminance_level, o->adm_csf_scale, o->adm_csf_diag_scale, 0.0,
               o->adm_bypass_cm, o->adm_p_norm, o->adm_f1s0, o->adm_f1s1, o->adm_f1s2, o->adm_f1s3,
               o->adm_f2s0, o->adm_f2s1, o->adm_f2s2, o->adm_f2s3);
}

/* The four scales of one frame. sums[] accumulates num, den, aim_num and
 * aim_den in that order, as compute_adm()'s doubles did; scores[] receives each
 * scale's numerator and denominator. Returns 0, or 1 when a wavelet transform
 * fails. */
static int adm_accumulate_scales(AdmFrameBufs *b, const AdmScaleOpts *o, const AdmFrameIn *in,
                                 int adm_skip_aim_scale, bool adm_skip_scale0, double sums[4],
                                 double *scores)
{
    const float *curr_ref_scale = in->ref;
    const float *curr_dis_scale = in->dis;
    int curr_ref_stride = in->ref_stride;
    int curr_dis_stride = in->dis_stride;
    int w = in->w;
    int h = in->h;

    for (int scale = 0; scale < 4; ++scale) {
        float num_scale = 0.0;
        float den_scale = 0.0;
        float aim_num_scale = 0.0;
        const bool skipped = (scale == 0) && (adm_skip_scale0);

        dwt2_src_indices_filt(b->ind_y, b->ind_x, w, h);
        if (adm_scale_dwt2(b, skipped, curr_ref_scale, curr_dis_scale, w, h, curr_ref_stride,
                           curr_dis_stride) < 0)
            return 1;

        w = (w + 1) / 2;
        h = (h + 1) / 2;
        if (skipped) {
            den_scale = 1e-10; // avoid divide by zero
        } else {
            adm_scale_sums(b, o, in->h, scale, w, h, &num_scale, &den_scale, &aim_num_scale);
        }

        sums[0] += num_scale;
        sums[1] += den_scale;
        if (adm_skip_aim_scale != scale) {
            sums[3] += den_scale;
            sums[2] += aim_num_scale;
        }

        curr_ref_scale = b->ref_dwt2.band_a;
        curr_dis_scale = b->dis_dwt2.band_a;

        curr_ref_stride = b->buf_stride;
        curr_dis_stride = b->buf_stride;

        scores[2 * scale + 0] = num_scale;
        scores[2 * scale + 1] = den_scale;
    }
    return 0;
}

/* ADM_OPT_SINGLE_PRECISION branch removed: the symbol was never defined
 * anywhere in the build system, making the 1e-2 threshold permanently
 * dead code. Keep only the validated 1e-10 path. */
int compute_adm(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride,
                double *score, double *score_num, double *score_den, double *scores,
                double border_factor, double adm_enhn_gain_limit, double adm_norm_view_dist,
                int adm_ref_display_height, int adm_csf_mode, double luminance_level,
                double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                int adm_bypass_cm, double adm_p_norm, double *score_aim, double adm_f1s0,
                double adm_f1s1, double adm_f1s2, double adm_f1s3, double adm_f2s0, double adm_f2s1,
                double adm_f2s2, double adm_f2s3, int adm_skip_aim_scale, bool adm_skip_scale0,
                unsigned index)
{
    double numden_limit = 1e-10 * (w * h) / (1920.0 * 1080.0);
    const AdmScaleOpts opts = {
        .border_factor = border_factor,
        .adm_enhn_gain_limit = adm_enhn_gain_limit,
        .adm_norm_view_dist = adm_norm_view_dist,
        .adm_ref_display_height = adm_ref_display_height,
        .adm_csf_mode = adm_csf_mode,
        .luminance_level = luminance_level,
        .adm_csf_scale = adm_csf_scale,
        .adm_csf_diag_scale = adm_csf_diag_scale,
        .adm_noise_weight = adm_noise_weight,
        .adm_bypass_cm = adm_bypass_cm,
        .adm_p_norm = adm_p_norm,
        .adm_f1s0 = adm_f1s0,
        .adm_f1s1 = adm_f1s1,
        .adm_f1s2 = adm_f1s2,
        .adm_f1s3 = adm_f1s3,
        .adm_f2s0 = adm_f2s0,
        .adm_f2s1 = adm_f2s1,
        .adm_f2s2 = adm_f2s2,
        .adm_f2s3 = adm_f2s3,
    };
    const AdmFrameIn in = {
        .ref = ref, .dis = dis, .w = w, .h = h, .ref_stride = ref_stride, .dis_stride = dis_stride};
    AdmFrameBufs bufs = {0};
    double sums[4] = {0, 0, 0, 0}; /* num, den, aim_num, aim_den */

    int ret = adm_frame_alloc(&bufs, w, h);
    if (!ret) {
        ret = adm_accumulate_scales(&bufs, &opts, &in, adm_skip_aim_scale, adm_skip_scale0, sums,
                                    scores);
    }
    if (!ret) {
        double num = sums[0];
        double den = sums[1];
        ret = vmaf_adm_floor_pair_named("float_adm", index, num, den, numden_limit, &num, &den);
        if (!ret) {
            ret = vmaf_adm_finalize_scores_named("float_adm", index, num, den, sums[2], sums[3],
                                                 score, score_aim);
        }
        if (!ret) {
            *score_num = num;
            *score_den = den;
        }
    }

    adm_frame_free(&bufs);
    return ret;
}

/* NOLINTEND(modernize-use-nullptr) */
