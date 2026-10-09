/**
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Integer ADM feature extractor — HIP backend.
 *  Ported from libvmaf/src/feature/cuda/integer_adm_cuda.c call-graph-for-call-graph.
 *
 *  Scaffold posture (no HAVE_HIPCC): every lifecycle helper returns -ENOSYS;
 *  the extractor registers under the name "adm_hip" so name-lookup works and
 *  the caller receives the cleaner "runtime not ready" surface instead of
 *  "no such extractor".
 *
 *  With HAVE_HIPCC: four HSACO blobs (adm_dwt2, adm_csf, adm_csf_den,
 *  adm_cm) are loaded via hipModuleLoadData at init() time and the full
 *  4-scale DWT + CSF + CM pipeline runs on device.
 *
 *  Algorithm: identical to the CUDA twin — integer DWT2 (Daubechies-7/9),
 *  contrast sensitivity function masking, and contrast masking numerator /
 *  denominator accumulation across 4 dyadic scales. See also:
 *  Li, Z. et al. (2016). "Toward A Practical Perceptual Video Quality
 *  Metric" — the ADM2 model shipped as "VMAF_integer_feature_adm2_score".
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "adm_csf_fixed_point.h"
#include "adm_score.h"
#include "adm_view_dist.h"
#include "barten_csf_tools.h"
#include "common.h"
#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "integer_adm.h"
/* The CPU extractor's contexts and result routines: the CSF weights, the
 * border, every rounding shift and the float conclusion of a scale come from
 * here, not from a copy (ADR-1423). */
#include "integer_adm_kernels.h"
#include "nonfinite_score.h"
#include "libvmaf/picture.h"

#include "hip/integer_adm_hip.h"
#include "integer_adm/adm_dwt2_rows.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"
#include "../../hip/shared_frame.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

/* 4 scales x 3 bands per accumulator, in this order: the DLM contrast
 * measure, the CSF denominator and the AIM contrast measure (ADR-1525). */
#define RES_SLOTS_PER_TERM ((size_t)4 * 3)
#define RES_BUFFER_SIZE (RES_SLOTS_PER_TERM * 3)
/* tmp_res and results_host hold one RES_BUFFER_SIZE block per viewing
 * distance (ADR-2795). */
#define ADM_VIEWS 2u

/* ------------------------------------------------------------------ */
/* Internal state                                                       */
/* ------------------------------------------------------------------ */

typedef struct AdmStateHip {
    size_t integer_stride;
    AdmBufferHip buf;
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    /* A second viewing distance evaluated on the same DWT (Netflix/vmaf
     * cffd5b77d, ADR-2795); 0 = none. */
    double adm_norm_view_dist_extra;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    double adm_min_val;   /* ADR-0487: minimum score floor (mirrors CPU + CUDA option). */
    bool adm_skip_scale0; /* host-side suppression: scale-0 excluded from score when set */
    bool adm_skip_aim;    /* no AIM kernels and an AIM numerator of 0, as the CPU (ADR-1525) */
    double adm_dlm_weight;
    double adm_p_norm;
    /* CSF weights per viewing distance. */
    float rfactor[ADM_VIEWS][12];
    uint32_t i_rfactor[ADM_VIEWS][12];
    unsigned submit_w, submit_h; /* stored by submit for collect */
    /* Result slots of the second viewing distance: the second RES_BUFFER_SIZE
     * block of tmp_res. */
    int64_t *adm_cm_x[4];
    uint64_t *adm_csf_den_x[4];
    int64_t *adm_aim_cm_x[4];

#ifdef HAVE_HIPCC
    hipStream_t str;
    hipEvent_t ref_event, dis_event, finished;
    hipModule_t adm_dwt_module;
    hipModule_t adm_csf_module;
    hipModule_t adm_csf_den_module;
    hipModule_t adm_cm_module;

    /* DWT kernel handles */
    hipFunction_t func_dwt_s123_combined_vert_kernel_0_0_int32_t;
    hipFunction_t func_dwt_s123_combined_vert_kernel_32768_16_int32_t;
    hipFunction_t func_dwt_s123_combined_hori_kernel_16384_15;
    hipFunction_t func_dwt_s123_combined_hori_kernel_32768_16;
    hipFunction_t func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t;
    hipFunction_t func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t;

    /* CSF kernel handles */
    hipFunction_t func_adm_csf_kernel_1_4;
    hipFunction_t func_i4_adm_csf_kernel_1_4;

    /* CSF-den kernel handles */
    hipFunction_t func_adm_csf_den_scale_row_kernel;
    hipFunction_t func_adm_csf_den_s123_row_kernel;

    /* CM kernel handles */
    hipFunction_t func_adm_cm_reduce_line_kernel_4;
    hipFunction_t func_adm_cm_line_kernel_8;
    hipFunction_t func_i4_adm_cm_line_kernel;

    /* AIM CM kernel handles (ADR-1525) */
    hipFunction_t func_adm_cm_aim_line_kernel_4;
    hipFunction_t func_i4_adm_cm_aim_line_kernel;

    /* ADR-0759: device copy of `buf`. The two CSF and the two CM compute
     * kernels take `const AdmBufferHip *` and read their band pointers from
     * here instead of receiving the whole struct by value on every launch.
     * Uploaded once by adm_hip_upload_buf(); `buf` does not change after
     * that, so the copy stays equal to it until close(). */
    AdmBufferHip *buf_dev;

    /* ADR-1211: the device copy of the scale-0 luma planes, packed (width *
     * bytes-per-sample per row). The HIP backend is host-pic (ADR-0530):
     * `VmafPicture::data[]` points at HOST memory. The DWT2 kernel is a
     * device kernel, so the plane has to be copied across before it can be
     * read — the CUDA twin gets a device picture from the pool and needs no
     * equivalent. The copy is the context's shared frame, or `planes`' own
     * buffers when there is none (ADR-1408). */
    void *d_ref_luma;
    void *d_dis_luma;
    VmafHipPlaneSource planes;
#endif /* HAVE_HIPCC */

    VmafDictionary *feature_name_dict;
} AdmStateHip;

/* Viewing distances one frame is evaluated at (ADR-2795). */
static unsigned adm_hip_views(const AdmStateHip *s)
{
    return (s->adm_norm_view_dist_extra > 0.0) ? 2u : 1u;
}

/* The viewing distance of `view`: 0 = adm_norm_view_dist, 1 = the extra. */
static double adm_hip_view_dist(const AdmStateHip *s, unsigned view)
{
    return view ? s->adm_norm_view_dist_extra : s->adm_norm_view_dist;
}

/**
 * Validate a CSF configuration at viewing distance `nvd` before claiming
 * device resources; adm_hip_validate() checks every distance the instance
 * evaluates. Mirrors `adm_csf_config_check()` in core/src/feature/integer_adm.c
 * so the CPU and this twin reject the same invalid table output. Finite
 * over-range weights are assigned the shared per-scale normalisation exponent
 * later.
 */
static int adm_csf_config_check(const AdmStateHip *s, double nvd)
{
    const int geom_err = adm_viewing_geometry_check("adm_hip", nvd, s->adm_ref_display_height);
    if (geom_err) {
        return geom_err;
    }

    for (int scale = 0; scale < 4; ++scale) {
        const AdmCsfFactors f =
            adm_csf_factors(scale, nvd, s->adm_ref_display_height, s->adm_csf_mode,
                            s->adm_csf_scale, s->adm_csf_diag_scale);
        const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
        const int err =
            adm_csf_check_scale(scale, rfactor1, nvd, s->adm_ref_display_height, s->adm_csf_mode);
        if (err) {
            return err;
        }
    }
    return 0;
}

/* CSF weight per scale and band, and its fixed-point form: the CPU's
 * adm_csf_factors() (integer_adm_kernels.h), converted by the shared
 * adm_csf_fixed_scale(). The host conclusion derives the weights again
 * through the CPU's context initialisers. */
static void adm_hip_rfactors(double adm_norm_view_dist, int adm_ref_display_height,
                             int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                             float rfactor[12], uint32_t i_rfactor[12])
{
    uint32_t normalization_shift[4] = {0u, 0u, 0u, 0u};
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t band0 = (size_t)scale * 3u;
        const AdmCsfFactors f =
            adm_csf_factors((int)scale, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                            adm_csf_scale, adm_csf_diag_scale);
        rfactor[band0] = f.factor1;
        rfactor[band0 + 1] = f.factor1;
        rfactor[band0 + 2] = f.factor2;
        double fixed[3];
        normalization_shift[scale] = 0u;
        const int err = adm_csf_fixed_scale((int)scale, &rfactor[band0], adm_norm_view_dist,
                                            adm_ref_display_height, adm_csf_mode, fixed,
                                            &normalization_shift[scale]);
        if (!err) {
            i_rfactor[band0] = (uint32_t)fixed[0];
            i_rfactor[band0 + 1] = (uint32_t)fixed[1];
            i_rfactor[band0 + 2] = (uint32_t)fixed[2];
        }
    }
}

/* The score helpers below only run behind the device pipeline; the
 * scaffold build (no HAVE_HIPCC) has nothing to conclude. */
#ifdef HAVE_HIPCC

/* ------------------------------------------------------------------ */
/* Score conclusion: the CPU's result routines on the CPU's contexts    */
/* ------------------------------------------------------------------ */

/* The numerator of one scale from its three band accumulators: the CPU's
 * adm_cm_result() / i4_adm_cm_result() on the CPU's own context. The context
 * initialisers only take addresses inside the buffer they are given, so an
 * empty one stands in for the host planes this twin does not have. */
static float adm_hip_cm_scale_result(const AdmStateHip *s, double nvd, const int64_t accum[3],
                                     int w, int h, int scale, double noise_weight)
{
    AdmBuffer no_planes;
    memset(&no_planes, 0, sizeof(no_planes));
    const AdmCmBounds bd = adm_cm_bounds(w, h);
    if (scale == 0) {
        AdmCmCtx c;
        adm_cm_ctx_init(&c, &no_planes, w, h, 0, 0, nvd, s->adm_ref_display_height, s->adm_csf_mode,
                        s->adm_csf_scale, s->adm_csf_diag_scale, false);
        /* The device sums modulo 2^64 into int64 storage; the scale-0 sum is
         * unsigned (adm_cm_round_row_total_s0()). */
        const uint64_t s0_accum[3] = {(uint64_t)accum[0], (uint64_t)accum[1], (uint64_t)accum[2]};
        return adm_cm_result(&c, &bd, s0_accum, noise_weight, s->adm_p_norm);
    }
    I4AdmCmCtx c;
    i4_adm_cm_ctx_init(&c, &no_planes, w, h, 0, 0, scale, nvd, s->adm_ref_display_height,
                       s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale, false);
    return i4_adm_cm_result(&c, &bd, accum, noise_weight, s->adm_p_norm);
}

/* The denominator of one scale: adm_csf_den_result() / i4_adm_csf_den_result(). */
static float adm_hip_csf_den_scale_result(const AdmStateHip *s, double nvd, const uint64_t accum[3],
                                          int w, int h, int scale)
{
    if (scale == 0) {
        AdmDenCtx c;
        adm_csf_den_ctx_init(&c, w, h, nvd, s->adm_ref_display_height, s->adm_csf_mode,
                             s->adm_csf_scale, s->adm_csf_diag_scale);
        return adm_csf_den_result(&c, accum, s->adm_noise_weight);
    }
    I4AdmDenCtx c;
    i4_adm_csf_den_ctx_init(&c, scale, w, h, nvd, s->adm_ref_display_height, s->adm_csf_mode,
                            s->adm_csf_scale, s->adm_csf_diag_scale);
    return i4_adm_csf_den_result(&c, accum, s->adm_noise_weight);
}

/* ------------------------------------------------------------------ */
/* write_scores — host-side, mirror of CUDA twin                       */
/* ------------------------------------------------------------------ */

typedef struct write_score_parameters_adm_hip {
    VmafFeatureCollector *feature_collector;
    AdmStateHip *s;
    unsigned index, h, w;
    unsigned view; /* 0: adm_norm_view_dist, 1: adm_norm_view_dist_extra */
} write_score_parameters_adm_hip;

/* The read-back result block of `view`. */
static const int64_t *adm_hip_view_results(const AdmStateHip *s, unsigned view)
{
    return &((const int64_t *)s->buf.results_host)[(size_t)view * RES_BUFFER_SIZE];
}

/* Per-scale numerator and denominator into scores[2 * scale] and
 * scores[2 * scale + 1], and their sums over the scales that count. */
static void adm_hip_scale_scores(const AdmStateHip *s, unsigned view, unsigned w, unsigned h,
                                 double scores[8], double *num, double *den)
{
    const int64_t *adm_cm = adm_hip_view_results(s, view);
    /* The device sums the denominator as uint64. */
    const uint64_t *adm_csf = (const uint64_t *)&adm_cm[RES_SLOTS_PER_TERM];
    const double nvd = adm_hip_view_dist(s, view);

    *num = 0;
    *den = 0;
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t band0 = (size_t)scale * 3u;
        w = (w + 1) / 2;
        h = (h + 1) / 2;

        /* adm_skip_scale0: integer_adm_scale0() leaves the numerator at 0 and
         * seeds the denominator with 1e-10 narrowed to float; both still enter
         * the sums and the per-scale outputs. The kernels compute scale 0
         * regardless; the suppression is host-side only. */
        float num_scale = 0.0f;
        float den_scale = (float)1e-10;
        if (scale != 0u || !s->adm_skip_scale0) {
            num_scale = adm_hip_cm_scale_result(s, nvd, &adm_cm[band0], (int)w, (int)h, (int)scale,
                                                s->adm_noise_weight);
            den_scale =
                adm_hip_csf_den_scale_result(s, nvd, &adm_csf[band0], (int)w, (int)h, (int)scale);
        }

        *num += num_scale;
        *den += den_scale;

        scores[2 * scale + 0] = num_scale;
        scores[2 * scale + 1] = den_scale;
    }
}

/* AIM numerator over the scales that count, from the full-frame size `w` x
 * `h` (integer_adm.c::integer_compute_adm): each scale concluded with noise
 * weight 0, as the CPU's measure_aim pass, and scale 0 left out under
 * adm_skip_scale0, whose CPU pass returns before the AIM pass. */
static double adm_hip_aim_num(const AdmStateHip *s, unsigned view, unsigned w, unsigned h)
{
    const int64_t *adm_aim_cm = &adm_hip_view_results(s, view)[RES_SLOTS_PER_TERM * 2u];
    const double nvd = adm_hip_view_dist(s, view);

    double aim_num = 0.0;
    for (unsigned scale = 0; scale < 4; ++scale) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        if (scale == 0u && s->adm_skip_scale0)
            continue;
        aim_num += adm_hip_cm_scale_result(s, nvd, &adm_aim_cm[(size_t)scale * 3u], (int)w, (int)h,
                                           (int)scale, 0.0);
    }
    return aim_num;
}

/* The frame's DLM and AIM ratios and their blend, adm3 (ADR-1525). */
typedef struct AdmHipFrameScores {
    double scores[8]; /* per scale: [2 * s] numerator, [2 * s + 1] denominator */
    double num;
    double den;
    double adm2;
    double aim;
    double adm3;
    double scale_scores[4];
} AdmHipFrameScores;

static int emit_adm_scores(const write_score_parameters_adm_hip *params, const AdmHipFrameScores *f)
{
    const AdmStateHip *s = params->s;
    /* View 1 files the second distance's seven scores under the keys the
     * dictionary maps to that distance's names (ADR-2795); the debug scores
     * are the first distance's only, as on the CPU. */
    const char *const *names = params->view ? vmaf_adm_extra_view_keys : vmaf_adm_view_names;
    const double view_values[VMAF_ADM_VIEW_SCORE_COUNT] = {
        f->adm2,
        f->aim,
        f->adm3,
        f->scale_scores[0],
        f->scale_scores[1],
        f->scale_scores[2],
        f->scale_scores[3],
    };
    VmafNamedScore values[18];
    for (size_t i = 0u; i < VMAF_ADM_VIEW_SCORE_COUNT; ++i)
        values[i] = (VmafNamedScore){names[i], view_values[i]};
    size_t value_count = VMAF_ADM_VIEW_SCORE_COUNT;
    if (s->debug && params->view == 0u) {
        static const char *const debug_names[8] = {
            "integer_adm_num_scale0", "integer_adm_den_scale0", "integer_adm_num_scale1",
            "integer_adm_den_scale1", "integer_adm_num_scale2", "integer_adm_den_scale2",
            "integer_adm_num_scale3", "integer_adm_den_scale3",
        };
        values[value_count++] = (VmafNamedScore){"integer_adm", f->adm2};
        values[value_count++] = (VmafNamedScore){"integer_adm_num", f->num};
        values[value_count++] = (VmafNamedScore){"integer_adm_den", f->den};
        for (size_t i = 0u; i < 8u; ++i)
            values[value_count++] = (VmafNamedScore){debug_names[i], f->scores[i]};
    }
    return vmaf_feature_emit_finite_scores(params->feature_collector, s->feature_name_dict,
                                           "integer_adm_hip", values, value_count, params->index);
}

/* The DLM and AIM ratios of the frame, as integer_adm.c::adm_result_finalise()
 * forms them: the numerator and denominator floored first, the AIM numerator
 * over the floored denominator. */
static int adm_hip_frame_ratios(const write_score_parameters_adm_hip *params, AdmHipFrameScores *f)
{
    const AdmStateHip *s = params->s;
    adm_hip_scale_scores(s, params->view, params->w, params->h, f->scores, &f->num, &f->den);

    /* CPU parity (integer_adm.c::integer_compute_adm): the precision floor
     * scales with the FULL-FRAME area, not the scale-3 area the per-scale
     * loop ends on. */
    const double numden_limit = 1e-10 * ((double)params->w * params->h) / (1920.0 * 1080.0);
    int err = vmaf_adm_floor_pair_named("integer_adm_hip", params->index, f->num, f->den,
                                        numden_limit, &f->num, &f->den);
    if (err)
        return err;

    const double aim_num =
        s->adm_skip_aim ? 0.0 : adm_hip_aim_num(s, params->view, params->w, params->h);
    const double aggregate_pairs[4] = {f->num, f->den, aim_num, f->den};
    double ratios[2];
    err = vmaf_adm_scale_ratios(aggregate_pairs, 2u, ratios);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_adm_hip: undefined or non-finite aggregate at frame %u "
                 "(num=%g den=%g aim_num=%g)\n",
                 params->index, f->num, f->den, aim_num);
        return err;
    }
    f->adm2 = ratios[0];
    f->aim = ratios[1];
    return 0;
}

static int write_view_scores(const write_score_parameters_adm_hip *params)
{
    const AdmStateHip *s = params->s;
    AdmHipFrameScores f;
    int err = adm_hip_frame_ratios(params, &f);
    if (err)
        return err;

    /* ADR-0487 clamps adm3 only: the CPU reference emits
     * VMAF_integer_feature_adm2_score unclamped (integer_adm.c::extract()
     * applies MAX(..., adm_min_val) to the adm3 expression alone). */
    err = vmaf_adm3_score_named("integer_adm_hip", params->index, f.adm2, f.aim, 0,
                                s->adm_dlm_weight, s->adm_min_val, &f.adm3);
    if (err)
        return err;
    err =
        vmaf_adm_scale_ratios_named("integer_adm_hip", params->index, f.scores, 4u, f.scale_scores);
    if (err)
        return err;
    return emit_adm_scores(params, &f);
}

/* Every viewing distance's scores, the first distance's first, as the CPU
 * files them. */
static int write_scores(write_score_parameters_adm_hip *params)
{
    int err = 0;
    for (unsigned v = 0; v < adm_hip_views(params->s) && !err; ++v) {
        params->view = v;
        err = write_view_scores(params);
    }
    return err;
}

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* VmafOption table                                                     */
/* ------------------------------------------------------------------ */

static const VmafOption options_hip[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(AdmStateHip, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "adm_csf_scale",
        .alias = "scf",
        .help = "scale coefficient for the horizontal & vertical direction terms of CSF",
        .offset = offsetof(AdmStateHip, adm_csf_scale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_CSF_SCALE,
        .min = 0.0,
        .max = 50.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_csf_diag_scale",
        .alias = "scfd",
        .help = "scale coefficient for the diagonal direction term of CSF",
        .offset = offsetof(AdmStateHip, adm_csf_diag_scale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_CSF_DIAG_SCALE,
        .min = 0.0,
        .max = 50.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        /* Blends DLM and AIM into VMAF_integer_feature_adm3_score; no effect
         * on adm2, as in the CPU reference. */
        .name = "adm_dlm_weight",
        .alias = "dlmw",
        .help = "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
        .offset = offsetof(AdmStateHip, adm_dlm_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.5,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_enhn_gain_limit",
        .alias = "egl",
        .help = "enhancement gain imposed on adm, must be >= 1.0, "
                "where 1.0 means the gain is completely disabled",
        .offset = offsetof(AdmStateHip, adm_enhn_gain_limit),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_ENHN_GAIN_LIMIT,
        .min = 1.0,
        .max = DEFAULT_ADM_ENHN_GAIN_LIMIT,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_norm_view_dist",
        .alias = "nvd",
        .help = "normalized viewing distance = viewing distance / ref display's physical height",
        .offset = offsetof(AdmStateHip, adm_norm_view_dist),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_NORM_VIEW_DIST,
        .min = 0.75,
        .max = 24.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        /* Not a feature parameter: the scores of this distance are filed
         * under the names its own `adm_norm_view_dist` would give them. */
        .name = "adm_norm_view_dist_extra",
        .alias = "nvde",
        .help = "second normalized viewing distance; when > 0, ADM is also evaluated at it "
                "from the same DWT and decouple, and its scores carry that distance's nvd "
                "suffix",
        .offset = offsetof(AdmStateHip, adm_norm_view_dist_extra),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.0,
        .min = 0.0,
        .max = 24.0,
    },
    {
        .name = "adm_ref_display_height",
        .alias = "rdh",
        .help = "reference display height in pixels",
        .offset = offsetof(AdmStateHip, adm_ref_display_height),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_ADM_REF_DISPLAY_HEIGHT,
        .min = 1,
        .max = 4320,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_csf_mode",
        .alias = "csf",
        .help = "contrast sensitivity function",
        .offset = offsetof(AdmStateHip, adm_csf_mode),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_ADM_CSF_MODE,
        .min = 0,
        .max = 3,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_noise_weight",
        .alias = "nw",
        .help = "noise weight",
        .offset = offsetof(AdmStateHip, adm_noise_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_NOISE_WEIGHT,
        .min = 0.0,
        .max = 1500.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_skip_aim",
        .help = "skip the calculation of AIM",
        .offset = offsetof(AdmStateHip, adm_skip_aim),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "adm_skip_scale0",
        .alias = "ssz",
        .help = "skip the calculation of scale 0",
        .offset = offsetof(AdmStateHip, adm_skip_scale0),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_min_val",
        .alias = "min",
        .help = "minimum value allowed; lower values will be clipped to this value",
        .offset = offsetof(AdmStateHip, adm_min_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_MIN_VAL,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_p_norm",
        .alias = "apn",
        .help = "p-norm exponent for fixed-point ADM contrast-measure finalisation",
        .offset = offsetof(AdmStateHip, adm_p_norm),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 3.0,
        .min = 1.0,
        .max = 20.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}};

/* ================================================================== */
/* HAVE_HIPCC path — real kernel dispatch                              */
/* ================================================================== */

#ifdef HAVE_HIPCC

/* Translate a HIP error to a negative errno. */
static int hip_rc(hipError_t rc)
{
    if (rc == hipSuccess)
        return 0;
    switch (rc) {
    case hipErrorInvalidValue:
    case hipErrorInvalidHandle:
        return -EINVAL;
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorNoDevice:
    case hipErrorInvalidDevice:
        return -ENODEV;
    case hipErrorNotSupported:
        return -ENOSYS;
    default:
        return -EIO;
    }
}

/* ------------------------------------------------------------------ */
/* Device-dispatch helpers (mirror integer_adm_cuda.c functions)      */
/* ------------------------------------------------------------------ */

#ifndef DIV_ROUND_UP /* cuda_helper.cuh defines the same expression when it is on the path */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif

static int dwt2_8_device_hip(AdmStateHip *s, const uint8_t *d_picture, hip_adm_dwt_band_t *d_dst,
                             hip_i4_adm_dwt_band_t i4_dwt_dst, int w, int h, int src_stride,
                             int dst_stride, AdmFixedParametersHip *p, hipStream_t c_stream)
{
    /* adm_dwt2_rows.h: the geometry the kernel instantiation assumes. */
    const int rows_per_thread = ADM_DWT2_V_ROWS_PER_THREAD;
    const int vert_out_tile_rows = ADM_DWT2_TILE_ROWS;
    const int vert_out_tile_cols = ADM_DWT2_TILE_COLS;
    const int horz_out_tile_cols = vert_out_tile_cols / 2 - 2;
    const int horz_out_tile_rows = vert_out_tile_rows;
    int16_t v_shift = 8;
    int32_t v_add_shift = 1 << (v_shift - 1);

    void *args[] = {(void *)&d_picture, d_dst,       &i4_dwt_dst, &w,           &h,
                    &src_stride,        &dst_stride, &v_shift,    &v_add_shift, p};
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t,
        (uint32_t)DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
        (uint32_t)DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1, (uint32_t)vert_out_tile_cols,
        (uint32_t)(vert_out_tile_rows / rows_per_thread), 1, 0, c_stream, args, NULL);
    return hip_rc(rc);
}

static int dwt2_16_device_hip(AdmStateHip *s, const uint16_t *d_picture, hip_adm_dwt_band_t *d_dst,
                              hip_i4_adm_dwt_band_t i4_dwt_dst, int w, int h, int src_stride,
                              int dst_stride, int inp_size_bits, AdmFixedParametersHip *p,
                              hipStream_t c_stream)
{
    /* adm_dwt2_rows.h: the geometry the kernel instantiation assumes. */
    const int rows_per_thread = ADM_DWT2_V_ROWS_PER_THREAD;
    const int vert_out_tile_rows = ADM_DWT2_TILE_ROWS;
    const int vert_out_tile_cols = ADM_DWT2_TILE_COLS;
    const int horz_out_tile_cols = vert_out_tile_cols / 2 - 2;
    const int horz_out_tile_rows = vert_out_tile_rows;
    int16_t v_shift = (int16_t)inp_size_bits;
    int32_t v_add_shift = 1 << (inp_size_bits - 1);

    void *args[] = {(void *)&d_picture, d_dst,       &i4_dwt_dst, &w,           &h,
                    &src_stride,        &dst_stride, &v_shift,    &v_add_shift, p};
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t,
        (uint32_t)DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
        (uint32_t)DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1, (uint32_t)vert_out_tile_cols,
        (uint32_t)(vert_out_tile_rows / rows_per_thread), 1, 0, c_stream, args, NULL);
    return hip_rc(rc);
}

static int adm_dwt2_s123_combined_device_hip(AdmStateHip *s, const int32_t *d_i4_scale,
                                             int32_t *tmp_buf, hip_i4_adm_dwt_band_t i4_dwt, int w,
                                             int h, int img_stride, int dst_stride, int scale,
                                             AdmFixedParametersHip *p, hipStream_t cu_stream)
{
    const int BLOCK_Y = (h + 1) / 2;

    void *args_vert[] = {(void *)&d_i4_scale, (void *)&tmp_buf, &w, &h, &img_stride, p};
    hipError_t rc;
    switch (scale) {
    case 1:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_vert_kernel_0_0_int32_t,
                                   (uint32_t)DIV_ROUND_UP(w, 128), (uint32_t)BLOCK_Y, 1, 128, 1, 1,
                                   0, cu_stream, args_vert, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    case 2: /* fall-through */
    case 3:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                                   (uint32_t)DIV_ROUND_UP(w, 128), (uint32_t)BLOCK_Y, 1, 128, 1, 1,
                                   0, cu_stream, args_vert, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    default:
        break; /* scale 0 handled in caller */
    }

    void *args_hori[] = {&i4_dwt, (void *)&tmp_buf, &w, &h, &dst_stride, p};
    switch (scale) {
    case 1:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_hori_kernel_16384_15,
                                   (uint32_t)DIV_ROUND_UP((w + 1) / 2, 128), (uint32_t)BLOCK_Y, 1,
                                   128, 1, 1, 0, cu_stream, args_hori, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    case 2:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_hori_kernel_32768_16,
                                   (uint32_t)DIV_ROUND_UP((w + 1) / 2, 128), (uint32_t)BLOCK_Y, 1,
                                   128, 1, 1, 0, cu_stream, args_hori, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    case 3:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_hori_kernel_16384_15,
                                   (uint32_t)DIV_ROUND_UP((w + 1) / 2, 128), (uint32_t)BLOCK_Y, 1,
                                   128, 1, 1, 0, cu_stream, args_hori, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    default:
        break;
    }
    return 0;
}

static int adm_csf_device_hip(AdmStateHip *s, AdmBufferHip *buf, int w, int h, int stride,
                              AdmFixedParametersHip *p, hipStream_t c_stream)
{
    for (int band = 0; band < 3; ++band)
        assert(((size_t)(buf->csf_f.bands[band]) & 15) == 0);
    assert(stride % 4 == 0);

    int left = (int)(w * (float)(ADM_BORDER_FACTOR)-0.5f - 1.0f);
    int top = (int)(h * (float)(ADM_BORDER_FACTOR)-0.5f - 1.0f);
    int right = w - left + 2;
    int bottom = h - top + 2;

    if (left < 0)
        left = 0;
    if (right > w)
        right = w;
    if (top < 0)
        top = 0;
    if (bottom > h)
        bottom = h;
    left = left & ~3;

    const int cols_per_thread = 4;
    const int rows_per_thread = 1;
    const int BLOCKX = 32;
    const int BLOCKY = 4;

    void *args[] = {(void *)&s->buf_dev, &top, &bottom, &left, &right, &stride, p};
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_csf_kernel_1_4, (uint32_t)DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
        (uint32_t)DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3, (uint32_t)BLOCKX,
        (uint32_t)BLOCKY, 1, 0, c_stream, args, NULL);
    return hip_rc(rc);
}

static int i4_adm_csf_device_hip(AdmStateHip *s, AdmBufferHip *buf, int scale, int w, int h,
                                 int stride, AdmFixedParametersHip *p, hipStream_t c_stream)
{
    for (int band = 0; band < 3; ++band)
        assert(((size_t)(buf->i4_csf_f.bands[band]) & 15) == 0);
    assert(stride % 4 == 0);

    int left = (int)(w * (float)(ADM_BORDER_FACTOR)-0.5f - 1.0f);
    int top = (int)(h * (float)(ADM_BORDER_FACTOR)-0.5f - 1.0f);
    int right = w - left + 2;
    int bottom = h - top + 2;

    if (left < 0)
        left = 0;
    if (right > w)
        right = w;
    if (top < 0)
        top = 0;
    if (bottom > h)
        bottom = h;
    left = left & ~3;

    const int cols_per_thread = 4;
    const int rows_per_thread = 1;
    const int BLOCKX = 32;
    const int BLOCKY = 4;

    void *args[] = {(void *)&s->buf_dev, &scale, &top, &bottom, &left, &right, &stride, p};
    hipError_t rc =
        hipModuleLaunchKernel(s->func_i4_adm_csf_kernel_1_4,
                              (uint32_t)DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
                              (uint32_t)DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3,
                              (uint32_t)BLOCKX, (uint32_t)BLOCKY, 1, 0, c_stream, args, NULL);
    return hip_rc(rc);
}

/* Launch geometry of the denominator kernels: one block per row of the border
 * region and band (ADM_CSF_DEN_THREADS in adm_csf_den.hip). */
#define ADM_HIP_CSF_DEN_THREADS 128u

/* Threads of the scales 1-3 AIM kernel (aim_i4_threads in adm_cm.hip). */
#define ADM_HIP_AIM_I4_THREADS 128u

static int adm_csf_den_s123_device_hip(AdmStateHip *s, AdmBufferHip *buf, int scale, int w, int h,
                                       int src_stride, double nvd, hipStream_t c_stream)
{
    /* The CPU's context: the border and every rounding shift of
     * adm_csf_den_s123(). */
    I4AdmDenCtx c;
    i4_adm_csf_den_ctx_init(&c, scale, w, h, nvd, s->adm_ref_display_height, s->adm_csf_mode,
                            s->adm_csf_scale, s->adm_csf_diag_scale);
    const int rows = c.b.bottom - c.b.top;
    if (rows <= 0 || c.b.right <= c.b.left)
        return 0;

    void *args[] = {&buf->i4_ref_dwt2,  &c.b.top,         &c.b.left,
                    &c.b.right,         &src_stride,      &c.add_shift_sq,
                    &c.shift_sq,        &c.add_shift_cub, &c.shift_cub,
                    &c.add_shift_accum, &c.shift_accum,   (void *)&buf->adm_csf_den[scale]};
    return hip_rc(hipModuleLaunchKernel(s->func_adm_csf_den_s123_row_kernel, 1u, (uint32_t)rows, 3u,
                                        ADM_HIP_CSF_DEN_THREADS, 1u, 1u, 0u, c_stream, args, NULL));
}

static int adm_csf_den_scale_device_hip(AdmStateHip *s, AdmBufferHip *buf, int w, int h,
                                        int src_stride, double nvd, hipStream_t c_stream)
{
    /* The CPU's context: the border and the rounding shift of
     * adm_csf_den_scale(). */
    AdmDenCtx c;
    adm_csf_den_ctx_init(&c, w, h, nvd, s->adm_ref_display_height, s->adm_csf_mode,
                         s->adm_csf_scale, s->adm_csf_diag_scale);
    const int rows = c.b.bottom - c.b.top;
    if (rows <= 0 || c.b.right <= c.b.left)
        return 0;
    uint32_t add_shift_accum = (uint32_t)c.add_shift_accum;
    uint32_t shift_accum = (uint32_t)c.shift_accum;

    void *args[] = {&buf->ref_dwt2, &c.b.top,         &c.b.left,    &c.b.right,
                    &src_stride,    &add_shift_accum, &shift_accum, (void *)&buf->adm_csf_den[0]};
    return hip_rc(hipModuleLaunchKernel(s->func_adm_csf_den_scale_row_kernel, 1u, (uint32_t)rows,
                                        3u, ADM_HIP_CSF_DEN_THREADS, 1u, 1u, 0u, c_stream, args,
                                        NULL));
}

typedef struct WarpShiftHip {
    uint32_t shift_cub[3];
    uint32_t add_shift_cub[3];
    uint32_t shift_sq[3];
    uint32_t add_shift_sq[3];
} WarpShiftHip;

/* The scales 1-3 CM active region: the band minus the ADM border, clamped so
 * the 3x3 threshold window always has a neighbour on each side. Scale 0 clamps
 * differently (adm_cm_device_hip below), which is why this is not shared. */
typedef struct I4AdmCmRegionHip {
    int left;
    int top;
    int right;
    int bottom;
    int start_col;
    int end_col;
    int start_row;
    int end_row;
    int buffer_stride;
    int buffer_h;
} I4AdmCmRegionHip;

/* Lifted whole out of i4_adm_cm_device_hip (HISS-04). Every statement moved
 * unchanged and in the same order, so the launch geometry is identical. */
static I4AdmCmRegionHip i4_adm_cm_region_hip(int w, int h)
{
    I4AdmCmRegionHip r;
    r.left = (int)(w * (float)(ADM_BORDER_FACTOR)-0.5f);
    r.top = (int)(h * (float)(ADM_BORDER_FACTOR)-0.5f);
    r.right = w - r.left;
    r.bottom = h - r.top;

    r.start_col = (r.left > 1) ? r.left : ((r.left <= 0) ? 0 : 1);
    r.end_col = (r.right < (w - 1)) ? r.right : ((r.right > (w - 1)) ? w : w - 1);
    r.start_row = (r.top > 1) ? r.top : ((r.top <= 0) ? 0 : 1);
    r.end_row = (r.bottom < (h - 1)) ? r.bottom : ((r.bottom > (h - 1)) ? h : h - 1);

    r.buffer_stride = r.end_col - r.start_col;
    r.buffer_h = r.end_row - r.start_row;
    return r;
}

static int i4_adm_cm_device_hip(AdmStateHip *s, AdmBufferHip *buf, int w, int h, int src_stride,
                                int csf_a_stride, int scale, AdmFixedParametersHip *p,
                                hipStream_t c_stream)
{
    I4AdmCmRegionHip r = i4_adm_cm_region_hip(w, h);

    /* inner CM kernel */
    {
        const int BLOCKX = 128;
        void *args[] = {(void *)&s->buf_dev,
                        &h,
                        &w,
                        &r.top,
                        &r.bottom,
                        &r.left,
                        &r.right,
                        &r.start_row,
                        &r.end_row,
                        &r.start_col,
                        &r.end_col,
                        &src_stride,
                        &csf_a_stride,
                        &scale,
                        &r.buffer_h,
                        &r.buffer_stride,
                        (void *)&buf->tmp_accum,
                        p};
        hipError_t rc = hipModuleLaunchKernel(
            s->func_i4_adm_cm_line_kernel, (uint32_t)DIV_ROUND_UP(r.buffer_stride, BLOCKX),
            (uint32_t)r.buffer_h, 3, (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
    }

    /* reduce kernel */
    {
        const int warps_per_cta = 4;
        const int BLOCKX = 32 * warps_per_cta;
        void *args[] = {&h,
                        &w,
                        &scale,
                        &r.buffer_h,
                        &r.buffer_stride,
                        (void *)&buf->tmp_accum,
                        (void *)&buf->adm_cm[scale]};
        hipError_t rc =
            hipModuleLaunchKernel(s->func_adm_cm_reduce_line_kernel_4, 1, (uint32_t)r.buffer_h, 3,
                                  (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
    }
    return 0;
}

/* What the scale-0 DLM and AIM kernels share: the CM region of a w x h band
 * and the rounding shifts of the CPU's adm_cm_ctx_init(), so the cube and
 * the row fold use integer_adm.c's own values (ADR-1525). */
typedef struct AdmCmS0LaunchHip {
    int top;
    int bottom;
    int left;
    int right;
    int start_row;
    int end_row;
    int start_col;
    int end_col;
    int buffer_stride;
    int buffer_h;
    WarpShiftHip ws;
    uint32_t shift_inner_accum;
    uint32_t add_shift_inner_accum;
} AdmCmS0LaunchHip;

static AdmCmS0LaunchHip adm_cm_s0_launch(const AdmStateHip *s, double nvd, int w, int h)
{
    AdmCmS0LaunchHip l;
    l.left = (int)(w * (float)(ADM_BORDER_FACTOR)-0.5f);
    l.top = (int)(h * (float)(ADM_BORDER_FACTOR)-0.5f);
    l.right = w - l.left;
    l.bottom = h - l.top;
    l.start_col = (l.left > 0) ? l.left : 0;
    l.end_col = (l.right < w) ? l.right : w;
    l.start_row = (l.top > 0) ? l.top : 0;
    l.end_row = (l.bottom < h) ? l.bottom : h;
    l.buffer_stride = l.end_col - l.start_col;
    l.buffer_h = l.end_row - l.start_row;

    AdmBuffer no_planes;
    memset(&no_planes, 0, sizeof(no_planes));
    AdmCmCtx c;
    adm_cm_ctx_init(&c, &no_planes, w, h, 0, 0, nvd, s->adm_ref_display_height, s->adm_csf_mode,
                    s->adm_csf_scale, s->adm_csf_diag_scale, false);
    for (int band = 0; band < 3; ++band) {
        l.ws.shift_cub[band] = c.band[band].shift_cub;
        l.ws.add_shift_cub[band] = c.band[band].add_shift_cub;
        l.ws.shift_sq[band] = (uint32_t)c.band[band].shift_sq;
        l.ws.add_shift_sq[band] = (uint32_t)c.band[band].add_shift_sq;
    }
    l.shift_inner_accum = c.shift_inner_accum;
    l.add_shift_inner_accum = c.add_shift_inner_accum;
    return l;
}

/* One scale-0 CM launch, DLM (`aim` false) or AIM: the two kernels take the
 * same arguments and launch shape, `rows_per_thread` rows per thread. */
static int adm_cm_s0_launch_kernel(AdmStateHip *s, AdmBufferHip *buf, hipFunction_t kernel,
                                   int rows_per_thread, int64_t *accum, int w, int h,
                                   int src_stride, int csf_a_stride, AdmFixedParametersHip *p,
                                   hipStream_t c_stream)
{
    int scale = 0;
    AdmCmS0LaunchHip l = adm_cm_s0_launch(s, p->adm_norm_view_dist, w, h);
    const int BLOCKX = 32;
    const int BLOCKY = 4;
    void *args[] = {(void *)&s->buf_dev,
                    &h,
                    &w,
                    &l.top,
                    &l.bottom,
                    &l.left,
                    &l.right,
                    &l.start_row,
                    &l.end_row,
                    &l.start_col,
                    &l.end_col,
                    &src_stride,
                    &csf_a_stride,
                    &l.buffer_h,
                    &l.buffer_stride,
                    (void *)&buf->tmp_accum,
                    p,
                    &scale,
                    (void *)&accum,
                    &l.ws,
                    &l.shift_inner_accum,
                    &l.add_shift_inner_accum};
    const hipError_t rc = hipModuleLaunchKernel(
        kernel, 1, (uint32_t)DIV_ROUND_UP(l.buffer_h, BLOCKY * rows_per_thread), 3,
        (uint32_t)BLOCKX, (uint32_t)BLOCKY, 1, 0, c_stream, args, NULL);
    return hip_rc(rc);
}

/* Scale-0 DLM contrast measure: the fused CM + reduce kernel. */
static int adm_cm_device_hip(AdmStateHip *s, AdmBufferHip *buf, int w, int h, int src_stride,
                             int csf_a_stride, AdmFixedParametersHip *p, hipStream_t c_stream)
{
    return adm_cm_s0_launch_kernel(s, buf, s->func_adm_cm_line_kernel_8, 8, buf->adm_cm[0], w, h,
                                   src_stride, csf_a_stride, p, c_stream);
}

/* Scale-0 AIM contrast measure (ADR-1525). */
static int adm_cm_aim_device_hip(AdmStateHip *s, AdmBufferHip *buf, int w, int h, int src_stride,
                                 int csf_a_stride, AdmFixedParametersHip *p, hipStream_t c_stream)
{
    return adm_cm_s0_launch_kernel(s, buf, s->func_adm_cm_aim_line_kernel_4, 4, buf->adm_aim_cm[0],
                                   w, h, src_stride, csf_a_stride, p, c_stream);
}

/* The scales 1-3 AIM contrast measure (ADR-1525): one block per row of the
 * region of i4_adm_cm_device_hip(), the shifts from the CPU's
 * i4_adm_cm_ctx_init(). */
static int i4_adm_cm_aim_device_hip(AdmStateHip *s, AdmBufferHip *buf, int w, int h, int src_stride,
                                    int scale, AdmFixedParametersHip *p, hipStream_t c_stream)
{
    I4AdmCmRegionHip r = i4_adm_cm_region_hip(w, h);
    if (r.buffer_h <= 0)
        return 0;

    /* The shifts of this launch's viewing distance (ADR-2795). */
    const double nvd = p->adm_norm_view_dist;
    AdmBuffer no_planes;
    memset(&no_planes, 0, sizeof(no_planes));
    I4AdmCmCtx c;
    i4_adm_cm_ctx_init(&c, &no_planes, w, h, 0, 0, scale, nvd, s->adm_ref_display_height,
                       s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale, false);
    AdmCmShiftsHip shifts = {
        .add_shift_sq = c.band.add_shift_sq,
        .shift_sq = (uint32_t)c.band.shift_sq,
        .add_shift_cub = c.band.add_shift_cub,
        .shift_cub = c.band.shift_cub,
        .add_shift_inner_accum = c.add_shift_inner_accum,
        .shift_inner_accum = c.shift_inner_accum,
    };

    void *args[] = {(void *)&s->buf_dev,
                    &h,
                    &w,
                    &r.top,
                    &r.bottom,
                    &r.left,
                    &r.right,
                    &r.start_row,
                    &r.end_row,
                    &r.start_col,
                    &r.end_col,
                    &src_stride,
                    &scale,
                    &shifts,
                    (void *)&buf->adm_aim_cm[scale],
                    p};
    return hip_rc(hipModuleLaunchKernel(s->func_i4_adm_cm_aim_line_kernel, 1, (uint32_t)r.buffer_h,
                                        3, ADM_HIP_AIM_I4_THREADS, 1, 1, 0, c_stream, args, NULL));
}

/* ------------------------------------------------------------------ */
/* Main per-frame computation                                           */
/* ------------------------------------------------------------------ */

/* Fixed-point parameters every ADM kernel reads, for a w x h luma plane. */
static void adm_hip_fixed_params(const AdmStateHip *s, unsigned view, int w, int h,
                                 AdmFixedParametersHip *p)
{
    memset(p, 0, sizeof(*p));
    p->dwt2_db2_coeffs_lo[0] = 15826;
    p->dwt2_db2_coeffs_lo[1] = 27411;
    p->dwt2_db2_coeffs_lo[2] = 7345;
    p->dwt2_db2_coeffs_lo[3] = -4240;
    p->dwt2_db2_coeffs_hi[0] = -4240;
    p->dwt2_db2_coeffs_hi[1] = -7345;
    p->dwt2_db2_coeffs_hi[2] = 27411;
    p->dwt2_db2_coeffs_hi[3] = -15826;
    p->dwt2_db2_coeffs_lo_sum = 46342;
    p->dwt2_db2_coeffs_hi_sum = 0;
    p->log2_w = log2f((float)w);
    p->log2_h = log2f((float)h);
    p->adm_ref_display_height = s->adm_ref_display_height;
    p->adm_norm_view_dist = adm_hip_view_dist(s, view);
    p->adm_enhn_gain_limit = s->adm_enhn_gain_limit;

    memcpy(p->rfactor, s->rfactor[view], sizeof(p->rfactor));
    memcpy(p->i_rfactor, s->i_rfactor[view], sizeof(p->i_rfactor));
}

/* ADR-1211: get the host-resident luma planes onto the device.
 * `VmafPicture::data[]` is HOST memory under the host-pic HIP backend
 * (ADR-0530), so handing it straight to the DWT2 kernel faults the GPU
 * ("Memory access fault ... Page not present"). The kernel reads a device
 * copy instead: the one the context shares between its twins (ADR-1408), or
 * this twin's own when there is no shared frame. Rows are tightly packed on
 * the device side, so the element stride the kernels see is `w`, not the
 * picture's. Returns once both pictures are read
 * (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
static int adm_hip_stage_luma(AdmStateHip *s, VmafHipSharedFrame *frame, const VmafPicture *ref_pic,
                              const VmafPicture *dis_pic)
{
    return vmaf_hip_plane_source_acquire_luma(&s->planes, frame, ref_pic, dis_pic,
                                              vmaf_hip_stream_bits(s->str), &s->d_ref_luma,
                                              &s->d_dis_luma);
}

/* Scale-0 DWT of both staged luma planes, queued on the picture stream. */
static int adm_hip_dwt2_scale0(AdmStateHip *s, AdmBufferHip *buf, const VmafPicture *ref_pic,
                               const VmafPicture *dis_pic, int w, int h, int buf_stride,
                               AdmFixedParametersHip *p)
{
    /* Strides are in ELEMENTS and refer to the staged, tightly-packed copy. */
    const int luma_stride = w;
    int err;

    if (ref_pic->bpc == 8) {
        err = dwt2_8_device_hip(s, (const uint8_t *)s->d_ref_luma, &buf->ref_dwt2, buf->i4_ref_dwt2,
                                w, h, luma_stride, buf_stride, p,
                                /* pic_stream */ 0);
        if (err)
            return err;
        return dwt2_8_device_hip(s, (const uint8_t *)s->d_dis_luma, &buf->dis_dwt2,
                                 buf->i4_dis_dwt2, w, h, luma_stride, buf_stride, p,
                                 /* pic_stream */ 0);
    }
    err = dwt2_16_device_hip(s, (const uint16_t *)s->d_ref_luma, &buf->ref_dwt2, buf->i4_ref_dwt2,
                             w, h, luma_stride, buf_stride, (int)ref_pic->bpc, p,
                             /* pic_stream */ 0);
    if (err)
        return err;
    return dwt2_16_device_hip(s, (const uint16_t *)s->d_dis_luma, &buf->dis_dwt2, buf->i4_dis_dwt2,
                              w, h, luma_stride, buf_stride, (int)dis_pic->bpc, p,
                              /* pic_stream */ 0);
}

/* Sync: record per-picture events, wait on the ADM stream. */
static int adm_hip_join_pic_stream(AdmStateHip *s)
{
    hipError_t hip_err = hipEventRecord(s->ref_event, /* pic stream */ 0);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);
    hip_err = hipEventRecord(s->dis_event, /* pic stream */ 0);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);
    hip_err = hipStreamWaitEvent(s->str, s->dis_event, 0);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);
    hip_err = hipStreamWaitEvent(s->str, s->ref_event, 0);
    return hip_rc(hip_err);
}

/* The DWT does not depend on the viewing distance; the denominator, CSF and
 * contrast-masking kernels do (they decouple inline from the DWT bands). A
 * frame runs the DWT once per scale and the rest once per distance, each
 * distance into its own result block (Netflix/vmaf cffd5b77d, ADR-2795). The
 * kernels of one distance write only csf_f / i4_csf_f, tmp_accum and their
 * result slots, so the next distance reads the DWT bands the transform left. */

/* `buf` with the result slots of `view`. The kernels read the band pointers
 * from buf_dev; the result slots are launch arguments. */
static AdmBufferHip adm_hip_view_buffer(const AdmStateHip *s, unsigned view)
{
    AdmBufferHip vb = s->buf;
    for (unsigned scale = 0; view && scale < 4u; ++scale) {
        vb.adm_cm[scale] = s->adm_cm_x[scale];
        vb.adm_csf_den[scale] = s->adm_csf_den_x[scale];
        vb.adm_aim_cm[scale] = s->adm_aim_cm_x[scale];
    }
    return vb;
}

/* Scale 0, distance-independent half: the int16 DWT on the picture stream,
 * then the ADM stream waits for it. */
static int adm_hip_scale0_transform(AdmStateHip *s, AdmBufferHip *buf, const VmafPicture *ref_pic,
                                    const VmafPicture *dis_pic, int w, int h, int buf_stride,
                                    AdmFixedParametersHip *p)
{
    const int err = adm_hip_dwt2_scale0(s, buf, ref_pic, dis_pic, w, h, buf_stride, p);
    if (err)
        return err;
    return adm_hip_join_pic_stream(s);
}

/* Scale 0 at one viewing distance, from the scale's `w2` x `h2` bands: CSF
 * denominator, CSF, DLM CM and AIM CM into `vb`'s result slots. */
static int adm_hip_scale0_weigh(AdmStateHip *s, AdmBufferHip *vb, int w2, int h2, int buf_stride,
                                AdmFixedParametersHip *p)
{
    int err =
        adm_csf_den_scale_device_hip(s, vb, w2, h2, buf_stride, p->adm_norm_view_dist, s->str);
    if (err)
        return err;
    err = adm_csf_device_hip(s, vb, w2, h2, buf_stride, p, s->str);
    if (err)
        return err;
    err = adm_cm_device_hip(s, vb, w2, h2, buf_stride, buf_stride, p, s->str);
    if (err || s->adm_skip_aim)
        return err;
    return adm_cm_aim_device_hip(s, vb, w2, h2, buf_stride, buf_stride, p, s->str);
}

/* Scales 1-3, distance-independent half: the int32 DWT of the previous
 * scale's `w` x `h` approximation band. */
static int adm_hip_scale123_transform(AdmStateHip *s, AdmBufferHip *buf, int scale, int w, int h,
                                      int buf_stride, AdmFixedParametersHip *p)
{
    const int err = adm_dwt2_s123_combined_device_hip(s, buf->i4_ref_dwt2.band_a,
                                                      (int32_t *)buf->tmp_ref, buf->i4_ref_dwt2, w,
                                                      h, buf_stride, buf_stride, scale, p, s->str);
    if (err)
        return err;
    return adm_dwt2_s123_combined_device_hip(s, buf->i4_dis_dwt2.band_a, (int32_t *)buf->tmp_dis,
                                             buf->i4_dis_dwt2, w, h, buf_stride, buf_stride, scale,
                                             p, s->str);
}

/* Scales 1-3 at one viewing distance, from the scale's `w2` x `h2` bands,
 * into `vb`'s result slots. */
static int adm_hip_scale123_weigh(AdmStateHip *s, AdmBufferHip *vb, int scale, int w2, int h2,
                                  int buf_stride, AdmFixedParametersHip *p)
{
    int err = adm_csf_den_s123_device_hip(s, vb, scale, w2, h2, buf_stride, p->adm_norm_view_dist,
                                          s->str);
    if (err)
        return err;
    err = i4_adm_csf_device_hip(s, vb, scale, w2, h2, buf_stride, p, s->str);
    if (err)
        return err;
    err = i4_adm_cm_device_hip(s, vb, w2, h2, buf_stride, buf_stride, scale, p, s->str);
    if (err || s->adm_skip_aim)
        return err;
    return i4_adm_cm_aim_device_hip(s, vb, w2, h2, buf_stride, scale, p, s->str);
}

/* The distance-dependent half of `scale` for every viewing distance. */
static int adm_hip_weigh_views(AdmStateHip *s, AdmFixedParametersHip p[ADM_VIEWS], int scale,
                               int w2, int h2, int buf_stride)
{
    int err = 0;
    for (unsigned v = 0; v < adm_hip_views(s) && !err; ++v) {
        AdmBufferHip vb = adm_hip_view_buffer(s, v);
        err = scale == 0 ? adm_hip_scale0_weigh(s, &vb, w2, h2, buf_stride, &p[v]) :
                           adm_hip_scale123_weigh(s, &vb, scale, w2, h2, buf_stride, &p[v]);
    }
    return err;
}

static int integer_compute_adm_hip(AdmStateHip *s, VmafHipSharedFrame *frame, VmafPicture *ref_pic,
                                   VmafPicture *dis_pic, AdmBufferHip *buf)
{
    int w = (int)ref_pic->w[0];
    int h = (int)ref_pic->h[0];
    const unsigned views = adm_hip_views(s);

    AdmFixedParametersHip p[ADM_VIEWS];
    for (unsigned v = 0; v < views; ++v)
        adm_hip_fixed_params(s, v, w, h, &p[v]);
    int err = adm_hip_stage_luma(s, frame, ref_pic, dis_pic);
    if (err)
        return err;

    /* Zero the result accumulators of every distance, after the upload: a
     * clear queued ahead of it is lost in the first context of a process
     * that needs larger planes than the contexts before, and the frame then
     * adds onto what recycled device memory holds (ADR-1423,
     * T-HIP-ADM-FIRST-FRAME-STALE-ACCUMULATORS-2026-10-01). */
    const size_t res_bytes = sizeof(int64_t) * RES_BUFFER_SIZE * views;
    hipError_t hip_err = hipMemsetAsync(buf->tmp_res, 0, res_bytes, s->str);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);

    const int buf_stride = (int)(buf->ind_size_x >> 2); /* bytes → int32 elements */
    err = adm_hip_scale0_transform(s, buf, ref_pic, dis_pic, w, h, buf_stride, &p[0]);
    for (int scale = 0; scale < 4 && err == 0; ++scale) {
        if (scale > 0)
            err = adm_hip_scale123_transform(s, buf, scale, w, h, buf_stride, &p[0]);
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        if (err == 0)
            err = adm_hip_weigh_views(s, p, scale, w, h, buf_stride);
    }
    if (err)
        return err;

    hip_err =
        hipMemcpyAsync(buf->results_host, buf->tmp_res, res_bytes, hipMemcpyDeviceToHost, s->str);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);
    hip_err = hipEventRecord(s->finished, s->str);
    return hip_rc(hip_err);
}

/* ------------------------------------------------------------------ */
/* Device resources: created by init_fex_hip(), released by close     */
/* ------------------------------------------------------------------ */

/* ALIGN_CEIL: round up to the nearest multiple of 64-byte cache line. */
#define ADM_HIP_ALIGN 64
#define ADM_ALIGN_CEIL(x) (((x) + ADM_HIP_ALIGN - 1) & ~(size_t)(ADM_HIP_ALIGN - 1))

/* The first failure's errno, or `rc` when that is already set. */
static int adm_hip_first_error(int rc, hipError_t hip_err)
{
    return (rc == 0 && hip_err != hipSuccess) ? hip_rc(hip_err) : rc;
}

/* Private stream and its three events. On failure, releases what it made. */
static int adm_hip_create_stream(AdmStateHip *s)
{
    hipError_t hip_err = hipStreamCreateWithFlags(&s->str, hipStreamNonBlocking);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);

    hipEvent_t *const events[] = {&s->finished, &s->ref_event, &s->dis_event};
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        hip_err = hipEventCreateWithFlags(events[i], hipEventDefault);
        if (hip_err != hipSuccess) {
            for (size_t k = i; k > 0; --k) {
                (void)hipEventDestroy(*events[k - 1]);
            }
            (void)hipStreamDestroy(s->str);
            return hip_rc(hip_err);
        }
    }
    return 0;
}

/* Init-failure teardown of adm_hip_create_stream(); statuses are discarded. */
static void adm_hip_destroy_stream(AdmStateHip *s)
{
    (void)hipEventDestroy(s->dis_event);
    (void)hipEventDestroy(s->ref_event);
    (void)hipEventDestroy(s->finished);
    (void)hipStreamDestroy(s->str);
}

/* close() teardown: drain the stream, then destroy it and its events.
 * Returns the first failure. */
static int adm_hip_close_stream(AdmStateHip *s)
{
    int rc = adm_hip_first_error(0, hipStreamSynchronize(s->str));
    rc = adm_hip_first_error(rc, hipStreamDestroy(s->str));
    rc = adm_hip_first_error(rc, hipEventDestroy(s->finished));
    rc = adm_hip_first_error(rc, hipEventDestroy(s->ref_event));
    return adm_hip_first_error(rc, hipEventDestroy(s->dis_event));
}

/* Load the four HSACO modules. On failure, unloads what it loaded. */
static int adm_hip_load_modules(AdmStateHip *s)
{
    hipModule_t *const modules[] = {&s->adm_dwt_module, &s->adm_csf_module, &s->adm_csf_den_module,
                                    &s->adm_cm_module};
    const unsigned char *const blobs[] = {adm_dwt2_hsaco, adm_csf_hsaco, adm_csf_den_hsaco,
                                          adm_cm_hsaco};
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        const hipError_t hip_err = hipModuleLoadData(modules[i], (const void *)blobs[i]);
        if (hip_err != hipSuccess) {
            for (size_t k = i; k > 0; --k) {
                (void)hipModuleUnload(*modules[k - 1]);
                *modules[k - 1] = NULL;
            }
            return hip_rc(hip_err);
        }
    }
    return 0;
}

/* Unload every loaded module, last loaded first. */
static void adm_hip_unload_modules(AdmStateHip *s)
{
    hipModule_t *const modules[] = {&s->adm_cm_module, &s->adm_csf_den_module, &s->adm_csf_module,
                                    &s->adm_dwt_module};
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        if (*modules[i] != NULL) {
            (void)hipModuleUnload(*modules[i]);
            *modules[i] = NULL;
        }
    }
}

typedef struct AdmHipKernelSlot {
    hipModule_t module;
    hipFunction_t *fn;
    const char *name;
} AdmHipKernelSlot;

/* Kernel function handles, resolved by name from the loaded modules. */
static int adm_hip_get_functions(AdmStateHip *s)
{
    const AdmHipKernelSlot slots[] = {
        {s->adm_dwt_module, &s->func_dwt_s123_combined_vert_kernel_0_0_int32_t,
         "dwt_s123_combined_vert_kernel_0_0_int32_t"},
        {s->adm_dwt_module, &s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
         "dwt_s123_combined_vert_kernel_32768_16_int32_t"},
        {s->adm_dwt_module, &s->func_dwt_s123_combined_hori_kernel_16384_15,
         "dwt_s123_combined_hori_kernel_16384_15"},
        {s->adm_dwt_module, &s->func_dwt_s123_combined_hori_kernel_32768_16,
         "dwt_s123_combined_hori_kernel_32768_16"},
        {s->adm_dwt_module, &s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t,
         "adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t"},
        {s->adm_dwt_module, &s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t,
         "adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t"},

        {s->adm_csf_module, &s->func_adm_csf_kernel_1_4, "adm_csf_kernel_1_4"},
        {s->adm_csf_module, &s->func_i4_adm_csf_kernel_1_4, "i4_adm_csf_kernel_1_4"},

        {s->adm_csf_den_module, &s->func_adm_csf_den_scale_row_kernel,
         "adm_csf_den_scale_row_kernel"},
        {s->adm_csf_den_module, &s->func_adm_csf_den_s123_row_kernel,
         "adm_csf_den_s123_row_kernel"},

        {s->adm_cm_module, &s->func_adm_cm_reduce_line_kernel_4, "adm_cm_reduce_line_kernel_4"},
        {s->adm_cm_module, &s->func_adm_cm_line_kernel_8, "adm_cm_line_kernel_8"},
        {s->adm_cm_module, &s->func_i4_adm_cm_line_kernel, "i4_adm_cm_line_kernel"},
        {s->adm_cm_module, &s->func_adm_cm_aim_line_kernel_4, "adm_cm_aim_line_kernel_4"},
        {s->adm_cm_module, &s->func_i4_adm_cm_aim_line_kernel, "i4_adm_cm_aim_line_kernel"},
    };
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        const hipError_t hip_err =
            hipModuleGetFunction(slots[i].fn, slots[i].module, slots[i].name);
        if (hip_err != hipSuccess)
            return hip_rc(hip_err);
    }
    return 0;
}

/* Buffer allocation — mirrors init_fex_cuda layout exactly. On failure,
 * frees what it allocated. */
static int adm_hip_alloc_buffers(AdmStateHip *s, unsigned w, unsigned h)
{
    s->integer_stride = ADM_ALIGN_CEIL(w * sizeof(int32_t));
    s->buf.ind_size_x = ADM_ALIGN_CEIL(((w + 1) / 2) * sizeof(int32_t));
    s->buf.ind_size_y = ADM_ALIGN_CEIL(((h + 1) / 2) * sizeof(int32_t));
    const size_t buf_sz_one = s->buf.ind_size_x * ((h + 1) / 2);

    void **const bufs[] = {&s->buf.data_buf,  &s->buf.tmp_ref,     &s->buf.tmp_dis,
                           &s->buf.tmp_accum, &s->buf.tmp_accum_h, &s->buf.tmp_res};
    const size_t sizes[] = {
        buf_sz_one * 11 + buf_sz_one / 2 * 11,          /* data_buf */
        s->integer_stride * 4 * ((h + 1) / 2),          /* tmp_ref */
        s->integer_stride * 4 * ((h + 1) / 2),          /* tmp_dis */
        sizeof(uint64_t) * 3u * (size_t)w * (size_t)h,  /* tmp_accum */
        sizeof(uint64_t) * 3u * (size_t)h,              /* tmp_accum_h */
        sizeof(uint64_t) * RES_BUFFER_SIZE * ADM_VIEWS, /* tmp_res, one block per distance */
    };
    const size_t count = sizeof(bufs) / sizeof(bufs[0]);

    size_t done = 0;
    hipError_t hip_err = hipSuccess;
    while (done < count && hip_err == hipSuccess) {
        hip_err = hipMalloc(bufs[done], sizes[done]);
        if (hip_err == hipSuccess)
            ++done;
    }
    if (hip_err == hipSuccess) {
        hip_err =
            hipHostMalloc(&s->buf.results_host, sizeof(uint64_t) * RES_BUFFER_SIZE * ADM_VIEWS,
                          hipHostMallocDefault);
    }
    if (hip_err != hipSuccess) {
        for (size_t k = done; k > 0; --k) {
            (void)hipFree(*bufs[k - 1]);
            *bufs[k - 1] = NULL;
        }
    }
    return hip_rc(hip_err);
}

/* Free the buffers adm_hip_alloc_buffers() made, pinned readback first. */
static void adm_hip_free_buffers(AdmStateHip *s)
{
    if (s->buf.results_host != NULL) {
        (void)hipHostFree(s->buf.results_host);
        s->buf.results_host = NULL;
    }
    void **const bufs[] = {&s->buf.tmp_res, &s->buf.tmp_accum_h, &s->buf.tmp_accum,
                           &s->buf.tmp_dis, &s->buf.tmp_ref,     &s->buf.data_buf};
    for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); ++i) {
        if (*bufs[i] != NULL) {
            (void)hipFree(*bufs[i]);
            *bufs[i] = NULL;
        }
    }
}

/* Let go of the luma planes (ADR-1408): the hold on the context's shared
 * frame, and this twin's own copies when it made any. */
static void adm_hip_free_luma(AdmStateHip *s)
{
    vmaf_hip_plane_source_close(&s->planes);
    s->d_ref_luma = NULL;
    s->d_dis_luma = NULL;
}

/* Slice the backing buffer into band pointers — mirrors init_dwt_band_cuda logic */
static void adm_hip_slice_bands(AdmStateHip *s, unsigned h)
{
    const size_t buf_sz_one = s->buf.ind_size_x * ((h + 1) / 2);
    uint8_t *top = (uint8_t *)s->buf.data_buf;
    const size_t half = buf_sz_one / 2;

    s->buf.ref_dwt2.band_a = (int16_t *)(top);
    s->buf.ref_dwt2.band_h = (int16_t *)(top + half);
    s->buf.ref_dwt2.band_v = (int16_t *)(top + 2u * half);
    s->buf.ref_dwt2.band_d = (int16_t *)(top + 3u * half);
    top += 4u * half;

    s->buf.dis_dwt2.band_a = (int16_t *)(top);
    s->buf.dis_dwt2.band_h = (int16_t *)(top + half);
    s->buf.dis_dwt2.band_v = (int16_t *)(top + 2u * half);
    s->buf.dis_dwt2.band_d = (int16_t *)(top + 3u * half);
    top += 4u * half;

    /* csf_f: band_a == NULL (hvd only) */
    s->buf.csf_f.band_a = NULL;
    s->buf.csf_f.band_h = (int16_t *)(top);
    s->buf.csf_f.band_v = (int16_t *)(top + half);
    s->buf.csf_f.band_d = (int16_t *)(top + 2u * half);
    top += 3u * half;

    /* i4 bands (full int32 size = buf_sz_one each) */
    s->buf.i4_ref_dwt2.band_a = (int32_t *)(top);
    s->buf.i4_ref_dwt2.band_h = (int32_t *)(top + buf_sz_one);
    s->buf.i4_ref_dwt2.band_v = (int32_t *)(top + 2u * buf_sz_one);
    s->buf.i4_ref_dwt2.band_d = (int32_t *)(top + 3u * buf_sz_one);
    top += 4u * buf_sz_one;

    s->buf.i4_dis_dwt2.band_a = (int32_t *)(top);
    s->buf.i4_dis_dwt2.band_h = (int32_t *)(top + buf_sz_one);
    s->buf.i4_dis_dwt2.band_v = (int32_t *)(top + 2u * buf_sz_one);
    s->buf.i4_dis_dwt2.band_d = (int32_t *)(top + 3u * buf_sz_one);
    top += 4u * buf_sz_one;

    s->buf.i4_csf_f.band_a = NULL;
    s->buf.i4_csf_f.band_h = (int32_t *)(top);
    s->buf.i4_csf_f.band_v = (int32_t *)(top + buf_sz_one);
    s->buf.i4_csf_f.band_d = (int32_t *)(top + 2u * buf_sz_one);
}

/* Slice one viewing distance's result block at `res`: DLM CM, CSF
 * denominator, AIM CM (RES_SLOTS_PER_TERM slots each, the order
 * adm_hip_scale_scores() and adm_hip_aim_num() read them in). Returns the
 * next block. */
static uint8_t *adm_hip_slice_block(uint8_t *res, int64_t *adm_cm[4], uint64_t *adm_csf_den[4],
                                    int64_t *adm_aim_cm[4])
{
    const size_t cm_stride = 3u * sizeof(int64_t);
    const size_t csf_stride = 3u * sizeof(uint64_t);
    for (int i = 0; i < 4; ++i) {
        adm_cm[i] = (int64_t *)(res + (size_t)i * cm_stride);
    }
    res += 4u * cm_stride;
    for (int i = 0; i < 4; ++i) {
        adm_csf_den[i] = (uint64_t *)(res + (size_t)i * csf_stride);
    }
    res += 4u * csf_stride;
    for (int i = 0; i < 4; ++i) {
        adm_aim_cm[i] = (int64_t *)(res + (size_t)i * cm_stride);
    }
    return res + 4u * cm_stride;
}

/* Slice the result accumulator: the first distance's block into `buf`, the
 * second's into the adm_*_x slots (ADR-2795). */
static void adm_hip_slice_results(AdmStateHip *s)
{
    uint8_t *res = adm_hip_slice_block((uint8_t *)s->buf.tmp_res, s->buf.adm_cm, s->buf.adm_csf_den,
                                       s->buf.adm_aim_cm);
    (void)adm_hip_slice_block(res, s->adm_cm_x, s->adm_csf_den_x, s->adm_aim_cm_x);
}

/* ADR-0759: upload `s->buf` to the device copy the CSF and CM kernels read.
 * Call it after adm_hip_slice_bands() and adm_hip_slice_results(), once every
 * pointer in the struct is final. Nothing writes `s->buf` between init and
 * close, so one upload serves every launch; code that changes `s->buf` after
 * init must upload it again before the next launch. On failure, frees what
 * it allocated. */
static int adm_hip_upload_buf(AdmStateHip *s)
{
    void *dev = NULL;
    hipError_t hip_err = hipMalloc(&dev, sizeof(s->buf));
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);
    hip_err = hipMemcpy(dev, &s->buf, sizeof(s->buf), hipMemcpyHostToDevice);
    if (hip_err != hipSuccess) {
        (void)hipFree(dev);
        return hip_rc(hip_err);
    }
    s->buf_dev = dev;
    return 0;
}

static void adm_hip_free_buf_dev(AdmStateHip *s)
{
    if (s->buf_dev != NULL) {
        (void)hipFree(s->buf_dev);
        s->buf_dev = NULL;
    }
}

/* Every device resource init_fex_hip() needs, in dependency order. On
 * failure, releases what it created. */
static int adm_hip_init_device(AdmStateHip *s, unsigned w, unsigned h)
{
    int err = adm_hip_create_stream(s);
    if (err)
        return err;
    err = adm_hip_load_modules(s);
    if (err) {
        adm_hip_destroy_stream(s);
        return err;
    }

    err = adm_hip_get_functions(s);
    if (err == 0) {
        err = adm_hip_alloc_buffers(s, w, h);
    }
    if (err) {
        adm_hip_unload_modules(s);
        adm_hip_destroy_stream(s);
        return err;
    }

    adm_hip_slice_bands(s, h);
    adm_hip_slice_results(s);
    err = adm_hip_upload_buf(s);
    if (err) {
        adm_hip_free_luma(s);
        adm_hip_free_buffers(s);
        adm_hip_unload_modules(s);
        adm_hip_destroy_stream(s);
    }
    return err;
}

#endif /* HAVE_HIPCC */

/* ================================================================== */
/* init / submit / collect / flush / close                             */
/* ================================================================== */

/* Rejections shared with the CPU reference, checked before any device
 * resource is claimed. */
static int adm_hip_validate(const AdmStateHip *s, unsigned w, unsigned h)
{
    /* Same frame-size bound as the CPU reference. */
    const int size_err = adm_frame_size_check("adm_hip", w, h);
    if (size_err) {
        return size_err;
    }

    /* Reject invalid CSF table output at every viewing distance before any
     * device resource is claimed. Finite over-range weights are normalised
     * with the CPU's shared per-scale exponent in adm_hip_rfactors(). */
    for (unsigned v = 0; v < adm_hip_views(s); ++v) {
        const int csf_err = adm_csf_config_check(s, adm_hip_view_dist(s, v));
        if (csf_err) {
            return csf_err;
        }
    }
    return 0;
}

#ifdef HAVE_HIPCC
/* The feature-name dictionary, with the second viewing distance's names
 * (ADR-2795); none on failure. */
static int adm_hip_init_names(VmafFeatureExtractor *fex, AdmStateHip *s)
{
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        return -ENOMEM;
    }
    const int err = vmaf_adm_extend_name_dict(fex, &s->feature_name_dict);
    if (err) {
        (void)vmaf_dictionary_free(&s->feature_name_dict);
    }
    return err;
}
#endif /* HAVE_HIPCC */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;

    AdmStateHip *s = fex->priv;

    const int err = adm_hip_validate(s, w, h);
    if (err) {
        return err;
    }

    for (unsigned v = 0; v < adm_hip_views(s); ++v) {
        adm_hip_rfactors(adm_hip_view_dist(s, v), s->adm_ref_display_height, s->adm_csf_mode,
                         s->adm_csf_scale, s->adm_csf_diag_scale, s->rfactor[v], s->i_rfactor[v]);
    }

#ifndef HAVE_HIPCC
    (void)w;
    (void)h;
    /* Scaffold: no runtime available. */
    return -ENOSYS;
#else
    const int dev_err = adm_hip_init_device(s, w, h);
    if (dev_err) {
        return dev_err;
    }

    const int name_err = adm_hip_init_names(fex, s);
    if (name_err) {
        /* The framework never calls close() after a failed init(), so every
         * device resource is released here. */
        adm_hip_free_buf_dev(s);
        adm_hip_free_luma(s);
        adm_hip_free_buffers(s);
        adm_hip_unload_modules(s);
        adm_hip_destroy_stream(s);
        return name_err;
    }
    return 0;
#endif /* HAVE_HIPCC */
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;

    AdmStateHip *s = fex->priv;
    s->submit_w = ref_pic->w[0];
    s->submit_h = ref_pic->h[0];

#ifndef HAVE_HIPCC
    (void)dist_pic;
    return -ENOSYS;
#else
    return integer_compute_adm_hip(s, fex->hip_frame, ref_pic, dist_pic, &s->buf);
#endif
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    AdmStateHip *s = fex->priv;
    hipError_t hip_err = hipStreamSynchronize(s->str);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);

    write_score_parameters_adm_hip params = {
        .feature_collector = feature_collector,
        .s = s,
        .index = index,
        .w = s->submit_w,
        .h = s->submit_h,
        .view = 0u,
    };
    return write_scores(&params);
#endif
}

static int flush_fex_hip(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    (void)feature_collector;
    AdmStateHip *s = fex->priv;
#ifndef HAVE_HIPCC
    (void)s;
    return -ENOSYS;
#else
    hipError_t hip_err = hipStreamSynchronize(s->str);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);
    return 1;
#endif
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    AdmStateHip *s = fex->priv;
    int rc = 0;

#ifdef HAVE_HIPCC
    rc = adm_hip_close_stream(s);
    adm_hip_unload_modules(s);
    adm_hip_free_buf_dev(s);
    adm_hip_free_luma(s);
    adm_hip_free_buffers(s);
#endif /* HAVE_HIPCC */

    if (s->feature_name_dict != NULL) {
        int err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0)
            rc = err;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Feature extractor descriptor                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"VMAF_integer_feature_adm2_score",
                                          "VMAF_integer_feature_aim_score",
                                          "VMAF_integer_feature_adm3_score",
                                          "integer_adm_scale0",
                                          "integer_adm_scale1",
                                          "integer_adm_scale2",
                                          "integer_adm_scale3",
                                          "integer_adm",
                                          "integer_adm_num",
                                          "integer_adm_den",
                                          "integer_adm_num_scale0",
                                          "integer_adm_den_scale0",
                                          "integer_adm_num_scale1",
                                          "integer_adm_den_scale1",
                                          "integer_adm_num_scale2",
                                          "integer_adm_den_scale2",
                                          "integer_adm_num_scale3",
                                          "integer_adm_den_scale3",
                                          NULL};

/*
 * Registration note: the extractor is declared as non-static so it can be
 * referenced by `extern VmafFeatureExtractor vmaf_fex_integer_adm_hip` in
 * feature_extractor.c's lookup table. Making it static would unlink it from
 * the registry. Same pattern as every other GPU feature extractor in this
 * tree (e.g. `vmaf_fex_integer_adm_cuda` in integer_adm_cuda.c).
 */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_adm_hip = {
    .name = "adm_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .flush = flush_fex_hip,
    .close = close_fex_hip,
    .options = options_hip,
    .priv_size = sizeof(AdmStateHip),
    .provided_features = provided_features,
    .merge = vmaf_adm_merge_view_dist,
    .extend_name_dict = vmaf_adm_extend_name_dict,
    /* ADR-1525: every output, aim and adm3 included, is the CPU's, so
     * `--backend hip` and a model's ADM features select this twin. */
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
