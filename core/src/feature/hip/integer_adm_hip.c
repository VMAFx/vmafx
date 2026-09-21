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
#include "barten_csf_tools.h"
#include "common.h"
#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "integer_adm.h"
#include "libvmaf/picture.h"
#include "log.h"

#include "hip/integer_adm_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define RES_BUFFER_SIZE (4 * 3 * 2)

/* ------------------------------------------------------------------ */
/* Internal state                                                       */
/* ------------------------------------------------------------------ */

typedef struct AdmStateHip {
    size_t integer_stride;
    AdmBufferHip buf;
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    double adm_min_val;   /* ADR-0487: minimum score floor (mirrors CPU + CUDA option). */
    bool adm_skip_scale0; /* host-side suppression: scale-0 excluded from score when set */
    double adm_dlm_weight;
    double adm_p_norm;
    float rfactor[12];
    uint32_t i_rfactor[12];
    unsigned submit_w, submit_h; /* stored by submit for collect */

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
    hipFunction_t func_adm_csf_den_scale_line_kernel;
    hipFunction_t func_adm_csf_den_s123_line_kernel;

    /* CM kernel handles */
    hipFunction_t func_adm_cm_reduce_line_kernel_4;
    hipFunction_t func_adm_cm_line_kernel_8;
    hipFunction_t func_i4_adm_cm_line_kernel;

    /* ADR-1211: device staging for the scale-0 luma plane.
     * The HIP backend is host-pic (ADR-0530): `VmafPicture::data[]` points at
     * HOST memory. The DWT2 kernel is a device kernel, so the plane has to be
     * copied across before it can be read — the CUDA twin gets a device
     * picture from the pool and needs no equivalent. Mirrors the staging
     * `integer_psnr_hip.c` already does with `ref_in` / `dis_in`. */
    void *d_ref_luma;
    void *d_dis_luma;
    size_t luma_pitch; /* bytes per staged row = width * bytes-per-sample */
    unsigned luma_h;
#endif /* HAVE_HIPCC */

    VmafDictionary *feature_name_dict;
} AdmStateHip;

/* ------------------------------------------------------------------ */
/* dwt_quant_step — identical to the CUDA twin                         */
/* ------------------------------------------------------------------ */

static inline float dwt_quant_step(const struct dwt_model_params *params, int lambda, int theta,
                                   double adm_norm_view_dist, int adm_ref_display_height)
{
    float r = (float)(adm_norm_view_dist * adm_ref_display_height * M_PI / 180.0);
    float temp = log10(pow(2.0, lambda + 1) * params->f0 * params->g[theta] / r);
    float Q = 2.0 * params->a * pow(10.0, params->k * (double)temp * temp) /
              dwt_7_9_basis_function_amplitudes[lambda][theta];
    return Q;
}

typedef struct AdmCsfFactors {
    float factor1; /* horizontal and vertical bands */
    float factor2; /* diagonal band */
} AdmCsfFactors;

static AdmCsfFactors adm_csf_factors(int scale, double adm_norm_view_dist,
                                     int adm_ref_display_height, int adm_csf_mode,
                                     double adm_csf_scale, double adm_csf_diag_scale)
{
    AdmCsfFactors f;
    if (adm_csf_mode == ADM_CSF_MODE_BARTEN) {
        f.factor1 = barten_csf(scale, adm_norm_view_dist, adm_ref_display_height,
                               DEFAULT_ADM_CSF_LUM, adm_csf_scale);
        f.factor2 = barten_csf(scale, adm_norm_view_dist, adm_ref_display_height,
                               DEFAULT_ADM_CSF_LUM, adm_csf_diag_scale);
    } else if (adm_csf_mode == ADM_CSF_MODE_BARTEN_WATSON_BLEND) {
        f.factor1 = barten_watson_blend_csf(scale, 0, adm_norm_view_dist, adm_ref_display_height);
        f.factor2 = barten_watson_blend_csf(scale, 1, adm_norm_view_dist, adm_ref_display_height);
    } else if (adm_csf_mode == ADM_CSF_MODE_BARTEN_WATSON_BLEND_MAE) {
        f.factor1 =
            barten_watson_blend_csf_mae(scale, 0, adm_norm_view_dist, adm_ref_display_height);
        f.factor2 =
            barten_watson_blend_csf_mae(scale, 1, adm_norm_view_dist, adm_ref_display_height);
    } else {
        f.factor1 = 1.0f / dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], scale, 1, adm_norm_view_dist,
                                          adm_ref_display_height);
        f.factor2 = 1.0f / dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], scale, 2, adm_norm_view_dist,
                                          adm_ref_display_height);
    }
    return f;
}

static void adm_csf_rfactor_scale0(const float rfactor1[3], double adm_norm_view_dist,
                                   int adm_ref_display_height, int adm_csf_mode,
                                   uint16_t i_rfactor[3])
{
    if (fabs(adm_norm_view_dist * adm_ref_display_height -
             DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT) < 1.0e-8 &&
        adm_csf_mode == ADM_CSF_MODE_WATSON97) {
        i_rfactor[0] = 36453;
        i_rfactor[1] = 36453;
        i_rfactor[2] = 49417;
    } else {
        const double pow2_21 = pow(2, 21);
        const double pow2_23 = pow(2, 23);
        i_rfactor[0] = (uint16_t)(rfactor1[0] * pow2_21);
        i_rfactor[1] = (uint16_t)(rfactor1[1] * pow2_21);
        i_rfactor[2] = (uint16_t)(rfactor1[2] * pow2_23);
    }
}

/**
 * Refuse a CSF configuration whose fixed-point weights would wrap
 * (ADR-1191). Mirrors `adm_csf_config_check()` in
 * core/src/feature/integer_adm.c so the CPU reference and this twin accept
 * exactly the same set of configurations -- the bounds in
 * adm_csf_fixed_point.h are the CPU pipeline's, deliberately applied here
 * too, because a twin that accepted a configuration the CPU rejects would
 * break the option / feature-name parity contract (ADR-1183). Returns 0 or
 * -EINVAL.
 */
static int adm_csf_config_check(const AdmStateHip *s)
{
    for (int scale = 0; scale < 4; ++scale) {
        const AdmCsfFactors f =
            adm_csf_factors(scale, s->adm_norm_view_dist, s->adm_ref_display_height,
                            s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale);
        const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
        const int err = adm_csf_check_scale(scale, rfactor1, s->adm_norm_view_dist,
                                            s->adm_ref_display_height, s->adm_csf_mode);
        if (err) {
            return err;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Score computation helpers (host-side, same as CUDA twin)            */
/* ------------------------------------------------------------------ */

static void conclude_adm_cm(int64_t *accum, int h, int w, int scale, float noise_weight,
                            double p_norm, float *result)
{
    int left = (int)(w * ADM_BORDER_FACTOR - 0.5);
    int top = (int)(h * ADM_BORDER_FACTOR - 0.5);
    int right = w - left;
    int bottom = h - top;
    const uint32_t shift_inner_accum = (uint32_t)ceil(log2((double)h));
    const double p_norm_exp = 1.0 / p_norm;

    const uint32_t shift_xcub[3] = {(uint32_t)ceil(log2((double)w) - 4.0),
                                    (uint32_t)ceil(log2((double)w) - 4.0),
                                    (uint32_t)ceil(log2((double)w) - 3.0)};
    int constant_offset[3] = {52, 52, 57};

    uint32_t shift_cub = (uint32_t)ceil(log2((double)w));
    float final_shift[3] = {powf(2.0f, (float)(45 - (int)shift_cub - (int)shift_inner_accum)),
                            powf(2.0f, (float)(39 - (int)shift_cub - (int)shift_inner_accum)),
                            powf(2.0f, (float)(36 - (int)shift_cub - (int)shift_inner_accum))};
    float powf_add =
        powf((float)((bottom - top) * (right - left)) * noise_weight, (float)p_norm_exp);

    float f_accum;
    *result = 0;
    for (int i = 0; i < 3; ++i) {
        if (scale == 0) {
            f_accum = (float)(accum[i] / pow(2.0, (double)(constant_offset[i] - (int)shift_xcub[i] -
                                                           (int)shift_inner_accum)));
        } else {
            f_accum = (float)((double)accum[i] / (double)final_shift[scale - 1]);
        }
        *result += powf(f_accum, (float)p_norm_exp) + powf_add;
    }
}

static void conclude_adm_csf_den(uint64_t *accum, int h, int w, int scale, float *result,
                                 const float rfactor[3], float noise_weight)
{
    const int left = (int)(w * ADM_BORDER_FACTOR - 0.5);
    const int top = (int)(h * ADM_BORDER_FACTOR - 0.5);
    const int right = w - left;
    const int bottom = h - top;
    const uint32_t accum_convert_float[4] = {18, 32, 27, 23};

    int32_t shift_accum;
    double shift_csf;
    if (scale == 0) {
        shift_accum = (int32_t)ceil(log2((double)((bottom - top) * (right - left))) - 20.0);
        shift_accum = shift_accum > 0 ? shift_accum : 0;
        shift_csf = pow(2.0, (double)(accum_convert_float[scale] - (uint32_t)shift_accum));
    } else {
        shift_accum = (int32_t)ceil(log2((double)(bottom - top)));
        const uint32_t shift_cub = (uint32_t)ceil(log2((double)(right - left)));
        shift_csf =
            pow(2.0, (double)(accum_convert_float[scale] - (uint32_t)shift_accum - shift_cub));
    }
    const float powf_add =
        powf((float)((bottom - top) * (right - left)) * noise_weight, 1.0f / 3.0f);

    *result = 0;
    for (int i = 0; i < 3; ++i) {
        const double csf = (double)(accum[i] / shift_csf) * pow((double)rfactor[i], 3.0);
        *result += powf((float)csf, 1.0f / 3.0f) + powf_add;
    }
}

/* ------------------------------------------------------------------ */
/* write_scores — host-side, mirror of CUDA twin                       */
/* ------------------------------------------------------------------ */

typedef struct write_score_parameters_adm_hip {
    VmafFeatureCollector *feature_collector;
    AdmStateHip *s;
    unsigned index, h, w;
} write_score_parameters_adm_hip;

typedef struct AdmScoresHip {
    double scales[8];
    double score;
    double num;
    double den;
} AdmScoresHip;

static void calculate_adm_scores(const write_score_parameters_adm_hip *params, AdmScoresHip *result)
{
    const AdmStateHip *s = params->s;
    double num = 0;
    double den = 0;
    unsigned w = params->w;
    unsigned h = params->h;
    int64_t *adm_cm = (int64_t *)s->buf.results_host;
    uint64_t *adm_csf = &((uint64_t *)s->buf.results_host)[RES_BUFFER_SIZE / 2];

    for (unsigned scale = 0; scale < 4; ++scale) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        float num_scale;
        float den_scale;
        conclude_adm_cm(&adm_cm[scale * 3], (int)h, (int)w, (int)scale, (float)s->adm_noise_weight,
                        s->adm_p_norm, &num_scale);
        conclude_adm_csf_den(&adm_csf[scale * 3], (int)h, (int)w, (int)scale, &den_scale,
                             &s->rfactor[scale * 3], (float)s->adm_noise_weight);
        if (scale == 0u && s->adm_skip_scale0) {
            result->scales[0] = 0.0;
            result->scales[1] = 1e-10;
            continue;
        }
        num += num_scale;
        den += den_scale;
        result->scales[2 * scale + 0] = num_scale;
        result->scales[2 * scale + 1] = den_scale;
    }
    const double numden_limit = 1e-10 * ((double)params->w * params->h) / (1920.0 * 1080.0);
    result->num = num < numden_limit ? 0 : num;
    result->den = den < numden_limit ? 0 : den;
    result->score = result->den == 0.0 ? 1.0 : result->num / result->den;
}

static int append_adm_primary_scores(const write_score_parameters_adm_hip *params,
                                     const AdmScoresHip *scores)
{
    static const char *const names[] = {"VMAF_integer_feature_adm2_score", "integer_adm_scale0",
                                        "integer_adm_scale1", "integer_adm_scale2",
                                        "integer_adm_scale3"};
    const double values[] = {
        scores->score, scores->scales[0] / scores->scales[1], scores->scales[2] / scores->scales[3],
        scores->scales[4] / scores->scales[5], scores->scales[6] / scores->scales[7]};
    int err = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        err |= vmaf_feature_collector_append_with_dict(params->feature_collector,
                                                       params->s->feature_name_dict, names[i],
                                                       values[i], params->index);
    return err;
}

static int append_adm_debug_scores(const write_score_parameters_adm_hip *params,
                                   const AdmScoresHip *scores)
{
    static const char *const names[] = {"integer_adm",
                                        "integer_adm_num",
                                        "integer_adm_den",
                                        "integer_adm_num_scale0",
                                        "integer_adm_den_scale0",
                                        "integer_adm_num_scale1",
                                        "integer_adm_den_scale1",
                                        "integer_adm_num_scale2",
                                        "integer_adm_den_scale2",
                                        "integer_adm_num_scale3",
                                        "integer_adm_den_scale3"};
    const double values[] = {scores->score,     scores->num,       scores->den,
                             scores->scales[0], scores->scales[1], scores->scales[2],
                             scores->scales[3], scores->scales[4], scores->scales[5],
                             scores->scales[6], scores->scales[7]};
    int err = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        err |= vmaf_feature_collector_append_with_dict(params->feature_collector,
                                                       params->s->feature_name_dict, names[i],
                                                       values[i], params->index);
    return err;
}

static int write_scores(const write_score_parameters_adm_hip *params)
{
    AdmScoresHip scores;
    calculate_adm_scores(params, &scores);
    int err = append_adm_primary_scores(params, &scores);
    if (params->s->debug)
        err |= append_adm_debug_scores(params, &scores);
    return err;
}

/* ------------------------------------------------------------------ */
/* VmafOption table                                                     */
/* ------------------------------------------------------------------ */

#define ADM_HIP_BOOL_OPTION(name_, alias_, help_, member_, flags_)                                 \
    {                                                                                              \
        .name = name_,                                                                             \
        .help = help_,                                                                             \
        .alias = alias_,                                                                           \
        .offset = offsetof(AdmStateHip, member_),                                                  \
        .type = VMAF_OPT_TYPE_BOOL,                                                                \
        .default_val.b = false,                                                                    \
        .flags = flags_,                                                                           \
    }
#define ADM_HIP_DOUBLE_OPTION(name_, alias_, help_, member_, default_, min_, max_)                 \
    {                                                                                              \
        .name = name_,                                                                             \
        .help = help_,                                                                             \
        .alias = alias_,                                                                           \
        .offset = offsetof(AdmStateHip, member_),                                                  \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = default_,                                                                 \
        .min = min_,                                                                               \
        .max = max_,                                                                               \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define ADM_HIP_INT_OPTION(name_, alias_, help_, member_, default_, min_, max_)                    \
    {                                                                                              \
        .name = name_,                                                                             \
        .help = help_,                                                                             \
        .alias = alias_,                                                                           \
        .offset = offsetof(AdmStateHip, member_),                                                  \
        .type = VMAF_OPT_TYPE_INT,                                                                 \
        .default_val.i = default_,                                                                 \
        .min = min_,                                                                               \
        .max = max_,                                                                               \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }

static const VmafOption options_hip[] = {
    ADM_HIP_BOOL_OPTION("debug", NULL, "debug mode: enable additional output", debug, 0),
    ADM_HIP_DOUBLE_OPTION("adm_csf_scale", "scf",
                          "scale coefficient for the horizontal & vertical direction terms of CSF",
                          adm_csf_scale, DEFAULT_ADM_CSF_SCALE, 0.0, 50.0),
    ADM_HIP_DOUBLE_OPTION("adm_csf_diag_scale", "scfd",
                          "scale coefficient for the diagonal direction term of CSF",
                          adm_csf_diag_scale, DEFAULT_ADM_CSF_DIAG_SCALE, 0.0, 50.0),
    ADM_HIP_DOUBLE_OPTION("adm_dlm_weight", "dlmw",
                          "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
                          adm_dlm_weight, 0.5, 0.0, 1.0),
    ADM_HIP_DOUBLE_OPTION("adm_enhn_gain_limit", "egl",
                          "enhancement gain imposed on adm, must be >= 1.0, where 1.0 means the "
                          "gain is completely disabled",
                          adm_enhn_gain_limit, DEFAULT_ADM_ENHN_GAIN_LIMIT, 1.0,
                          DEFAULT_ADM_ENHN_GAIN_LIMIT),
    ADM_HIP_DOUBLE_OPTION(
        "adm_norm_view_dist", "nvd",
        "normalized viewing distance = viewing distance / ref display's physical height",
        adm_norm_view_dist, DEFAULT_ADM_NORM_VIEW_DIST, 0.75, 24.0),
    ADM_HIP_INT_OPTION("adm_ref_display_height", "rdh", "reference display height in pixels",
                       adm_ref_display_height, DEFAULT_ADM_REF_DISPLAY_HEIGHT, 1, 4320),
    ADM_HIP_INT_OPTION("adm_csf_mode", "csf", "contrast sensitivity function", adm_csf_mode,
                       DEFAULT_ADM_CSF_MODE, 0, 3),
    ADM_HIP_DOUBLE_OPTION("adm_noise_weight", "nw", "noise weight", adm_noise_weight,
                          DEFAULT_ADM_NOISE_WEIGHT, 0.0, 1500.0),
    ADM_HIP_BOOL_OPTION("adm_skip_scale0", "ssz", "skip the calculation of scale 0",
                        adm_skip_scale0, VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_HIP_DOUBLE_OPTION("adm_min_val", "min",
                          "minimum value allowed; lower values will be clipped to this value",
                          adm_min_val, DEFAULT_ADM_MIN_VAL, 0.0, 1.0),
    ADM_HIP_DOUBLE_OPTION("adm_p_norm", "apn",
                          "p-norm exponent for fixed-point ADM contrast-measure finalisation",
                          adm_p_norm, 3.0, 1.0, 20.0),
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

#define HIP_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

static int dwt2_8_device_hip(AdmStateHip *s, const uint8_t *d_picture, hip_adm_dwt_band_t *d_dst,
                             hip_i4_adm_dwt_band_t i4_dwt_dst, int w, int h, int src_stride,
                             int dst_stride, AdmFixedParametersHip *p, hipStream_t c_stream)
{
    const int rows_per_thread = 4;
    const int vert_out_tile_rows = 8;
    const int vert_out_tile_cols = 128;
    const int horz_out_tile_cols = vert_out_tile_cols / 2 - 2;
    const int horz_out_tile_rows = vert_out_tile_rows;
    int16_t v_shift = 8;
    int32_t v_add_shift = 1 << (v_shift - 1);

    void *args[] = {&d_picture,  d_dst,       &i4_dwt_dst, &w,           &h,
                    &src_stride, &dst_stride, &v_shift,    &v_add_shift, p};
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t,
        (uint32_t)HIP_DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
        (uint32_t)HIP_DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1,
        (uint32_t)vert_out_tile_cols, (uint32_t)(vert_out_tile_rows / rows_per_thread), 1, 0,
        c_stream, args, NULL);
    return hip_rc(rc);
}

static int dwt2_16_device_hip(AdmStateHip *s, const uint16_t *d_picture, hip_adm_dwt_band_t *d_dst,
                              hip_i4_adm_dwt_band_t i4_dwt_dst, int w, int h, int src_stride,
                              int dst_stride, int inp_size_bits, AdmFixedParametersHip *p,
                              hipStream_t c_stream)
{
    const int rows_per_thread = 4;
    const int vert_out_tile_rows = 8;
    const int vert_out_tile_cols = 128;
    const int horz_out_tile_cols = vert_out_tile_cols / 2 - 2;
    const int horz_out_tile_rows = vert_out_tile_rows;
    int16_t v_shift = (int16_t)inp_size_bits;
    int32_t v_add_shift = 1 << (inp_size_bits - 1);

    void *args[] = {&d_picture,  d_dst,       &i4_dwt_dst, &w,           &h,
                    &src_stride, &dst_stride, &v_shift,    &v_add_shift, p};
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t,
        (uint32_t)HIP_DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
        (uint32_t)HIP_DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1,
        (uint32_t)vert_out_tile_cols, (uint32_t)(vert_out_tile_rows / rows_per_thread), 1, 0,
        c_stream, args, NULL);
    return hip_rc(rc);
}

static int adm_dwt2_s123_combined_device_hip(AdmStateHip *s, const int32_t *d_i4_scale,
                                             int32_t *tmp_buf, hip_i4_adm_dwt_band_t i4_dwt, int w,
                                             int h, int img_stride, int dst_stride, int scale,
                                             AdmFixedParametersHip *p, hipStream_t cu_stream)
{
    const int BLOCK_Y = (h + 1) / 2;

    void *args_vert[] = {&d_i4_scale, &tmp_buf, &w, &h, &img_stride, p};
    hipError_t rc;
    switch (scale) {
    case 1:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_vert_kernel_0_0_int32_t,
                                   (uint32_t)HIP_DIV_ROUND_UP(w, 128), (uint32_t)BLOCK_Y, 1, 128, 1,
                                   1, 0, cu_stream, args_vert, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    case 2: /* fall-through */
    case 3:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                                   (uint32_t)HIP_DIV_ROUND_UP(w, 128), (uint32_t)BLOCK_Y, 1, 128, 1,
                                   1, 0, cu_stream, args_vert, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    default:
        break; /* scale 0 handled in caller */
    }

    void *args_hori[] = {&i4_dwt, &tmp_buf, &w, &h, &dst_stride, p};
    switch (scale) {
    case 1:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_hori_kernel_16384_15,
                                   (uint32_t)HIP_DIV_ROUND_UP((w + 1) / 2, 128), (uint32_t)BLOCK_Y,
                                   1, 128, 1, 1, 0, cu_stream, args_hori, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    case 2:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_hori_kernel_32768_16,
                                   (uint32_t)HIP_DIV_ROUND_UP((w + 1) / 2, 128), (uint32_t)BLOCK_Y,
                                   1, 128, 1, 1, 0, cu_stream, args_hori, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    case 3:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_hori_kernel_16384_15,
                                   (uint32_t)HIP_DIV_ROUND_UP((w + 1) / 2, 128), (uint32_t)BLOCK_Y,
                                   1, 128, 1, 1, 0, cu_stream, args_hori, NULL);
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
    const int BLOCKX = 32, BLOCKY = 4;

    void *args[] = {buf, &top, &bottom, &left, &right, &stride, p};
    hipError_t rc =
        hipModuleLaunchKernel(s->func_adm_csf_kernel_1_4,
                              (uint32_t)HIP_DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
                              (uint32_t)HIP_DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3,
                              (uint32_t)BLOCKX, (uint32_t)BLOCKY, 1, 0, c_stream, args, NULL);
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
    const int BLOCKX = 32, BLOCKY = 4;

    void *args[] = {buf, &scale, &top, &bottom, &left, &right, &stride, p};
    hipError_t rc =
        hipModuleLaunchKernel(s->func_i4_adm_csf_kernel_1_4,
                              (uint32_t)HIP_DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
                              (uint32_t)HIP_DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3,
                              (uint32_t)BLOCKX, (uint32_t)BLOCKY, 1, 0, c_stream, args, NULL);
    return hip_rc(rc);
}

static int adm_csf_den_s123_device_hip(AdmStateHip *s, AdmBufferHip *buf, int scale, int w, int h,
                                       int src_stride, hipStream_t c_stream)
{
    int left = (int)(w * (float)(ADM_BORDER_FACTOR)-0.5f);
    int top = (int)(h * (float)(ADM_BORDER_FACTOR)-0.5f);
    int right = w - left;
    int bottom = h - top;
    int buffer_stride = right - left;
    int buffer_h = bottom - top;

    const int val_per_thread = 8;
    const int warps_per_cta = 4;
    const int BLOCKX = 32 * warps_per_cta;

    uint32_t shift_sq[3] = {31, 30, 31};
    uint32_t add_shift_sq[3] = {1u << shift_sq[0], 1u << shift_sq[1], 1u << shift_sq[2]};

    void *args[] = {&buf->i4_ref_dwt2,
                    &h,
                    &top,
                    &bottom,
                    &left,
                    &right,
                    &src_stride,
                    &add_shift_sq[scale - 1],
                    &shift_sq[scale - 1],
                    &buf->adm_csf_den[scale]};
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_csf_den_s123_line_kernel,
        (uint32_t)HIP_DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread), (uint32_t)buffer_h, 3,
        (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, NULL);
    return hip_rc(rc);
}

static int adm_csf_den_scale_device_hip(AdmStateHip *s, AdmBufferHip *buf, int w, int h,
                                        int src_stride, hipStream_t c_stream)
{
    int scale = 0;
    int left = (int)(w * (float)(ADM_BORDER_FACTOR)-0.5f);
    int top = (int)(h * (float)(ADM_BORDER_FACTOR)-0.5f);
    int right = w - left;
    int bottom = h - top;
    int buffer_stride = right - left;
    int buffer_h = bottom - top;

    const int val_per_thread = 8;
    const int warps_per_cta = 4;
    const int BLOCKX = 32 * warps_per_cta;

    void *args[] = {&buf->ref_dwt2, &h,     &top,        &bottom,
                    &left,          &right, &src_stride, &buf->adm_csf_den[scale]};
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_csf_den_scale_line_kernel,
        (uint32_t)HIP_DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread), (uint32_t)buffer_h, 3,
        (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, NULL);
    return hip_rc(rc);
}

typedef struct WarpShiftHip {
    uint32_t shift_cub[3];
    uint32_t add_shift_cub[3];
    uint32_t shift_sq[3];
    uint32_t add_shift_sq[3];
} WarpShiftHip;

static WarpShiftHip make_warp_shift_hip(int w)
{
    static const int fixed_shift[3] = {4, 4, 3};
    static const int32_t shift_xsq[3] = {29, 29, 30};
    static const int32_t add_shift_xsq[3] = {268435456, 268435456, 536870912};
    WarpShiftHip shift;
    for (int band = 0; band < 3; ++band) {
        shift.shift_cub[band] = (uint32_t)ceilf(log2f((float)w));
        shift.shift_cub[band] -= (uint32_t)fixed_shift[band];
        shift.shift_sq[band] = (uint32_t)shift_xsq[band];
        shift.add_shift_sq[band] = (uint32_t)add_shift_xsq[band];
        shift.add_shift_cub[band] = 1u << (shift.shift_cub[band] - 1u);
    }
    return shift;
}

static int i4_adm_cm_device_hip(AdmStateHip *s, AdmBufferHip *buf, int w, int h, int src_stride,
                                int csf_a_stride, int scale, AdmFixedParametersHip *p,
                                hipStream_t c_stream)
{
    int left = (int)(w * (float)(ADM_BORDER_FACTOR)-0.5f);
    int top = (int)(h * (float)(ADM_BORDER_FACTOR)-0.5f);
    int right = w - left;
    int bottom = h - top;

    int start_col = (left > 1) ? left : ((left <= 0) ? 0 : 1);
    int end_col = (right < (w - 1)) ? right : ((right > (w - 1)) ? w : w - 1);
    int start_row = (top > 1) ? top : ((top <= 0) ? 0 : 1);
    int end_row = (bottom < (h - 1)) ? bottom : ((bottom > (h - 1)) ? h : h - 1);

    int buffer_stride = end_col - start_col;
    int buffer_h = end_row - start_row;

    /* inner CM kernel */
    {
        const int BLOCKX = 128;
        void *args[] = {
            buf,           &h,         &w,        &top,           &bottom,         &left,
            &right,        &start_row, &end_row,  &start_col,     &end_col,        &src_stride,
            &csf_a_stride, &scale,     &buffer_h, &buffer_stride, &buf->tmp_accum, p};
        hipError_t rc = hipModuleLaunchKernel(
            s->func_i4_adm_cm_line_kernel, (uint32_t)HIP_DIV_ROUND_UP(buffer_stride, BLOCKX),
            (uint32_t)buffer_h, 3, (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
    }

    /* reduce kernel */
    {
        const int warps_per_cta = 4;
        const int BLOCKX = 32 * warps_per_cta;
        void *args[] = {
            &h, &w, &scale, &buffer_h, &buffer_stride, &buf->tmp_accum, &buf->adm_cm[scale]};
        hipError_t rc =
            hipModuleLaunchKernel(s->func_adm_cm_reduce_line_kernel_4, 1, (uint32_t)buffer_h, 3,
                                  (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
    }
    return 0;
}

static int adm_cm_device_hip(AdmStateHip *s, AdmBufferHip *buf, int w, int h, int src_stride,
                             int csf_a_stride, AdmFixedParametersHip *p, hipStream_t c_stream)
{
    int scale = 0;
    int left = (int)(w * (float)(ADM_BORDER_FACTOR)-0.5f);
    int top = (int)(h * (float)(ADM_BORDER_FACTOR)-0.5f);
    int right = w - left;
    int bottom = h - top;

    int start_col = (left > 0) ? left : 0;
    int end_col = (right < w) ? right : w;
    int start_row = (top > 0) ? top : 0;
    int end_row = (bottom < h) ? bottom : h;

    int buffer_stride = end_col - start_col;
    int buffer_h = end_row - start_row;

    WarpShiftHip ws = make_warp_shift_hip(w);
    uint32_t shift_inner_accum = (uint32_t)ceilf(log2f((float)h));
    uint32_t add_shift_inner_accum = 1u << (shift_inner_accum - 1u);

    /* fused CM + reduce kernel */
    {
        const int rows_per_thread = 8;
        const int BLOCKX = 32, BLOCKY = 4;
        void *args[] = {buf,
                        &h,
                        &w,
                        &top,
                        &bottom,
                        &left,
                        &right,
                        &start_row,
                        &end_row,
                        &start_col,
                        &end_col,
                        &src_stride,
                        &csf_a_stride,
                        &buffer_h,
                        &buffer_stride,
                        &buf->tmp_accum,
                        p,
                        &scale,
                        &buf->adm_cm[scale],
                        &ws,
                        &shift_inner_accum,
                        &add_shift_inner_accum};
        hipError_t rc =
            hipModuleLaunchKernel(s->func_adm_cm_line_kernel_8, 1,
                                  (uint32_t)HIP_DIV_ROUND_UP(buffer_h, BLOCKY * rows_per_thread), 3,
                                  (uint32_t)BLOCKX, (uint32_t)BLOCKY, 1, 0, c_stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main per-frame computation                                           */
/* ------------------------------------------------------------------ */

typedef struct AdmComputeContextHip {
    AdmStateHip *s;
    AdmBufferHip *buf;
    AdmFixedParametersHip fixed;
    int w;
    int h;
    size_t buf_stride;
    size_t curr_ref_stride;
    size_t curr_dis_stride;
    int32_t *curr_ref;
    int32_t *curr_dis;
} AdmComputeContextHip;

static void init_adm_fixed_parameters(AdmComputeContextHip *ctx, double gain_limit,
                                      double view_distance, int display_height)
{
    static const int32_t coeffs_lo[4] = {15826, 27411, 7345, -4240};
    static const int32_t coeffs_hi[4] = {-4240, -7345, 27411, -15826};
    AdmFixedParametersHip *const p = &ctx->fixed;
    memset(p, 0, sizeof(*p));
    memcpy(p->dwt2_db2_coeffs_lo, coeffs_lo, sizeof(coeffs_lo));
    memcpy(p->dwt2_db2_coeffs_hi, coeffs_hi, sizeof(coeffs_hi));
    p->dwt2_db2_coeffs_lo_sum = 46342;
    p->dwt2_db2_coeffs_hi_sum = 0;
    p->log2_w = log2f((float)ctx->w);
    p->log2_h = log2f((float)ctx->h);
    p->adm_ref_display_height = display_height;
    p->adm_norm_view_dist = view_distance;
    p->adm_enhn_gain_limit = gain_limit;
    const double pow2_32 = pow(2, 32);
    for (unsigned scale = 0; scale < 4; ++scale) {
        const AdmCsfFactors factors =
            adm_csf_factors((int)scale, view_distance, display_height, ctx->s->adm_csf_mode,
                            ctx->s->adm_csf_scale, ctx->s->adm_csf_diag_scale);
        p->rfactor[scale * 3] = factors.factor1;
        p->rfactor[scale * 3 + 1] = factors.factor1;
        p->rfactor[scale * 3 + 2] = factors.factor2;
        if (scale == 0) {
            uint16_t i_rf[3];
            adm_csf_rfactor_scale0(p->rfactor, view_distance, display_height, ctx->s->adm_csf_mode,
                                   i_rf);
            p->i_rfactor[0] = i_rf[0];
            p->i_rfactor[1] = i_rf[1];
            p->i_rfactor[2] = i_rf[2];
        } else {
            p->i_rfactor[scale * 3] = (uint32_t)(p->rfactor[scale * 3] * pow2_32);
            p->i_rfactor[scale * 3 + 1] = (uint32_t)(p->rfactor[scale * 3 + 1] * pow2_32);
            p->i_rfactor[scale * 3 + 2] = (uint32_t)(p->rfactor[scale * 3 + 2] * pow2_32);
        }
    }
    memcpy(ctx->s->rfactor, p->rfactor, sizeof(p->rfactor));
}

static int stage_adm_luma(AdmComputeContextHip *ctx, const VmafPicture *ref_pic,
                          const VmafPicture *dis_pic)
{
    const size_t bytes_per_pixel = ref_pic->bpc > 8 ? sizeof(uint16_t) : sizeof(uint8_t);
    const size_t row_bytes = (size_t)ctx->w * bytes_per_pixel;
    hipError_t rc = hipMemcpy2DAsync(ctx->s->d_ref_luma, ctx->s->luma_pitch, ref_pic->data[0],
                                     (size_t)ref_pic->stride[0], row_bytes, (size_t)ctx->h,
                                     hipMemcpyHostToDevice, ctx->s->str);
    if (rc != hipSuccess)
        return hip_rc(rc);
    rc = hipMemcpy2DAsync(ctx->s->d_dis_luma, ctx->s->luma_pitch, dis_pic->data[0],
                          (size_t)dis_pic->stride[0], row_bytes, (size_t)ctx->h,
                          hipMemcpyHostToDevice, ctx->s->str);
    if (rc != hipSuccess)
        return hip_rc(rc);
    return hip_rc(hipStreamSynchronize(ctx->s->str));
}

static int wait_for_adm_input(AdmStateHip *s)
{
    hipError_t rc = hipEventRecord(s->ref_event, 0);
    if (rc == hipSuccess)
        rc = hipEventRecord(s->dis_event, 0);
    if (rc == hipSuccess)
        rc = hipStreamWaitEvent(s->str, s->dis_event, 0);
    if (rc == hipSuccess)
        rc = hipStreamWaitEvent(s->str, s->ref_event, 0);
    return hip_rc(rc);
}

static int run_adm_scale0(AdmComputeContextHip *ctx, const VmafPicture *ref_pic,
                          const VmafPicture *dis_pic)
{
    int err;
    if (ref_pic->bpc == 8) {
        err = dwt2_8_device_hip(ctx->s, ctx->s->d_ref_luma, &ctx->buf->ref_dwt2,
                                ctx->buf->i4_ref_dwt2, ctx->w, ctx->h, (int)ctx->curr_ref_stride,
                                (int)ctx->buf_stride, &ctx->fixed, 0);
        if (!err)
            err = dwt2_8_device_hip(
                ctx->s, ctx->s->d_dis_luma, &ctx->buf->dis_dwt2, ctx->buf->i4_dis_dwt2, ctx->w,
                ctx->h, (int)ctx->curr_dis_stride, (int)ctx->buf_stride, &ctx->fixed, 0);
    } else {
        err = dwt2_16_device_hip(ctx->s, ctx->s->d_ref_luma, &ctx->buf->ref_dwt2,
                                 ctx->buf->i4_ref_dwt2, ctx->w, ctx->h, (int)ctx->curr_ref_stride,
                                 (int)ctx->buf_stride, (int)ref_pic->bpc, &ctx->fixed, 0);
        if (!err)
            err =
                dwt2_16_device_hip(ctx->s, ctx->s->d_dis_luma, &ctx->buf->dis_dwt2,
                                   ctx->buf->i4_dis_dwt2, ctx->w, ctx->h, (int)ctx->curr_dis_stride,
                                   (int)ctx->buf_stride, (int)dis_pic->bpc, &ctx->fixed, 0);
    }
    if (err)
        return err;
    err = wait_for_adm_input(ctx->s);
    ctx->w = (ctx->w + 1) / 2;
    ctx->h = (ctx->h + 1) / 2;
    if (!err)
        err = adm_csf_den_scale_device_hip(ctx->s, ctx->buf, ctx->w, ctx->h, (int)ctx->buf_stride,
                                           ctx->s->str);
    if (!err)
        err = adm_csf_device_hip(ctx->s, ctx->buf, ctx->w, ctx->h, (int)ctx->buf_stride,
                                 &ctx->fixed, ctx->s->str);
    if (!err)
        err = adm_cm_device_hip(ctx->s, ctx->buf, ctx->w, ctx->h, (int)ctx->buf_stride,
                                (int)ctx->buf_stride, &ctx->fixed, ctx->s->str);
    return err;
}

static int run_adm_later_scale(AdmComputeContextHip *ctx, unsigned scale)
{
    int err = adm_dwt2_s123_combined_device_hip(
        ctx->s, ctx->curr_ref, ctx->buf->tmp_ref, ctx->buf->i4_ref_dwt2, ctx->w, ctx->h,
        (int)ctx->curr_ref_stride, (int)ctx->buf_stride, (int)scale, &ctx->fixed, ctx->s->str);
    if (!err)
        err = adm_dwt2_s123_combined_device_hip(
            ctx->s, ctx->curr_dis, ctx->buf->tmp_dis, ctx->buf->i4_dis_dwt2, ctx->w, ctx->h,
            (int)ctx->curr_dis_stride, (int)ctx->buf_stride, (int)scale, &ctx->fixed, ctx->s->str);
    if (err)
        return err;
    ctx->w = (ctx->w + 1) / 2;
    ctx->h = (ctx->h + 1) / 2;
    err = adm_csf_den_s123_device_hip(ctx->s, ctx->buf, (int)scale, ctx->w, ctx->h,
                                      (int)ctx->buf_stride, ctx->s->str);
    if (!err)
        err = i4_adm_csf_device_hip(ctx->s, ctx->buf, (int)scale, ctx->w, ctx->h,
                                    (int)ctx->buf_stride, &ctx->fixed, ctx->s->str);
    if (!err)
        err = i4_adm_cm_device_hip(ctx->s, ctx->buf, ctx->w, ctx->h, (int)ctx->buf_stride,
                                   (int)ctx->buf_stride, (int)scale, &ctx->fixed, ctx->s->str);
    return err;
}

static void advance_adm_scale(AdmComputeContextHip *ctx)
{
    ctx->curr_ref = ctx->buf->i4_ref_dwt2.band_a;
    ctx->curr_dis = ctx->buf->i4_dis_dwt2.band_a;
    ctx->curr_ref_stride = ctx->buf_stride;
    ctx->curr_dis_stride = ctx->buf_stride;
}

static int integer_compute_adm_hip(AdmStateHip *s, VmafPicture *ref_pic, VmafPicture *dis_pic,
                                   AdmBufferHip *buf, double adm_enhn_gain_limit,
                                   double adm_norm_view_dist, int adm_ref_display_height)
{
    AdmComputeContextHip ctx = {.s = s,
                                .buf = buf,
                                .w = (int)ref_pic->w[0],
                                .h = (int)ref_pic->h[0],
                                .buf_stride = buf->ind_size_x >> 2,
                                .curr_ref_stride = ref_pic->w[0],
                                .curr_dis_stride = ref_pic->w[0]};
    init_adm_fixed_parameters(&ctx, adm_enhn_gain_limit, adm_norm_view_dist,
                              adm_ref_display_height);
    int err = hip_rc(hipMemsetAsync(buf->tmp_res, 0, sizeof(int64_t) * RES_BUFFER_SIZE, s->str));
    if (!err)
        err = stage_adm_luma(&ctx, ref_pic, dis_pic);
    for (unsigned scale = 0; !err && scale < 4; ++scale) {
        err =
            scale == 0 ? run_adm_scale0(&ctx, ref_pic, dis_pic) : run_adm_later_scale(&ctx, scale);
        if (!err)
            advance_adm_scale(&ctx);
    }
    if (!err)
        err = hip_rc(hipMemcpyAsync(buf->results_host, buf->tmp_res,
                                    sizeof(int64_t) * RES_BUFFER_SIZE, hipMemcpyDeviceToHost,
                                    s->str));
    if (!err)
        err = hip_rc(hipEventRecord(s->finished, s->str));
    return err;
}

#endif /* HAVE_HIPCC */

/* ================================================================== */
/* init / submit / collect / flush / close                             */
/* ================================================================== */

/* ALIGN_CEIL: round up to the nearest multiple of 64-byte cache line. */
#define ADM_HIP_ALIGN 64
#define ADM_ALIGN_CEIL(x) (((x) + ADM_HIP_ALIGN - 1) & ~(size_t)(ADM_HIP_ALIGN - 1))

static int init_adm_configuration(AdmStateHip *s)
{
    if (s->adm_norm_view_dist * s->adm_ref_display_height <
        DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT)
        return -EINVAL;
    const int config_err = adm_csf_config_check(s);
    if (config_err)
        return config_err;
    const double pow2_32 = pow(2, 32);
    for (unsigned scale = 0; scale < 4; scale++) {
        const AdmCsfFactors f =
            adm_csf_factors((int)scale, s->adm_norm_view_dist, s->adm_ref_display_height,
                            s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale);
        s->rfactor[scale * 3 + 0] = f.factor1;
        s->rfactor[scale * 3 + 1] = f.factor1;
        s->rfactor[scale * 3 + 2] = f.factor2;
        if (scale == 0) {
            uint16_t i_rf[3];
            adm_csf_rfactor_scale0(&s->rfactor[0], s->adm_norm_view_dist, s->adm_ref_display_height,
                                   s->adm_csf_mode, i_rf);
            s->i_rfactor[0] = i_rf[0];
            s->i_rfactor[1] = i_rf[1];
            s->i_rfactor[2] = i_rf[2];
        } else {
            s->i_rfactor[scale * 3 + 0] = (uint32_t)(s->rfactor[scale * 3 + 0] * pow2_32);
            s->i_rfactor[scale * 3 + 1] = (uint32_t)(s->rfactor[scale * 3 + 1] * pow2_32);
            s->i_rfactor[scale * 3 + 2] = (uint32_t)(s->rfactor[scale * 3 + 2] * pow2_32);
        }
    }
    return 0;
}

#ifdef HAVE_HIPCC

static void record_hip_cleanup_error(int *first_err, hipError_t rc, const char *operation)
{
    if (rc == hipSuccess)
        return;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "adm_hip %s failed with HIP error %d\n", operation, (int)rc);
    if (*first_err == 0)
        *first_err = hip_rc(rc);
}

static void release_hip_pointer(void **ptr, int *first_err, const char *name)
{
    if (!*ptr)
        return;
    record_hip_cleanup_error(first_err, hipFree(*ptr), name);
    *ptr = NULL;
}

static void release_hip_module(hipModule_t *module, int *first_err, const char *name)
{
    if (!*module)
        return;
    record_hip_cleanup_error(first_err, hipModuleUnload(*module), name);
    *module = NULL;
}

static void release_hip_event(hipEvent_t *event, int *first_err, const char *name)
{
    if (!*event)
        return;
    record_hip_cleanup_error(first_err, hipEventDestroy(*event), name);
    *event = NULL;
}

static int release_adm_runtime(AdmStateHip *s, bool synchronize)
{
    int first_err = 0;
    if (synchronize && s->str)
        record_hip_cleanup_error(&first_err, hipStreamSynchronize(s->str), "stream synchronize");
    release_hip_pointer(&s->d_ref_luma, &first_err, "reference luma free");
    release_hip_pointer(&s->d_dis_luma, &first_err, "distorted luma free");
    if (s->buf.results_host) {
        record_hip_cleanup_error(&first_err, hipHostFree(s->buf.results_host), "host result free");
        s->buf.results_host = NULL;
    }
    release_hip_pointer(&s->buf.tmp_res, &first_err, "result buffer free");
    release_hip_pointer(&s->buf.tmp_accum_h, &first_err, "row accumulator free");
    release_hip_pointer(&s->buf.tmp_accum, &first_err, "accumulator free");
    release_hip_pointer(&s->buf.tmp_dis, &first_err, "distorted scratch free");
    release_hip_pointer(&s->buf.tmp_ref, &first_err, "reference scratch free");
    release_hip_pointer(&s->buf.data_buf, &first_err, "band buffer free");
    release_hip_module(&s->adm_cm_module, &first_err, "CM module unload");
    release_hip_module(&s->adm_csf_den_module, &first_err, "CSF denominator module unload");
    release_hip_module(&s->adm_csf_module, &first_err, "CSF module unload");
    release_hip_module(&s->adm_dwt_module, &first_err, "DWT module unload");
    release_hip_event(&s->dis_event, &first_err, "distorted event destroy");
    release_hip_event(&s->ref_event, &first_err, "reference event destroy");
    release_hip_event(&s->finished, &first_err, "finished event destroy");
    if (s->str) {
        record_hip_cleanup_error(&first_err, hipStreamDestroy(s->str), "stream destroy");
        s->str = NULL;
    }
    return first_err;
}

static int create_adm_control_resources(AdmStateHip *s)
{
    int err = hip_rc(hipStreamCreateWithFlags(&s->str, hipStreamNonBlocking));
    if (!err)
        err = hip_rc(hipEventCreateWithFlags(&s->finished, hipEventDefault));
    if (!err)
        err = hip_rc(hipEventCreateWithFlags(&s->ref_event, hipEventDefault));
    if (!err)
        err = hip_rc(hipEventCreateWithFlags(&s->dis_event, hipEventDefault));
    return err;
}

static int load_adm_modules(AdmStateHip *s)
{
    int err = hip_rc(hipModuleLoadData(&s->adm_dwt_module, adm_dwt2_hsaco));
    if (!err)
        err = hip_rc(hipModuleLoadData(&s->adm_csf_module, adm_csf_hsaco));
    if (!err)
        err = hip_rc(hipModuleLoadData(&s->adm_csf_den_module, adm_csf_den_hsaco));
    if (!err)
        err = hip_rc(hipModuleLoadData(&s->adm_cm_module, adm_cm_hsaco));
    return err;
}

static int get_adm_function(hipFunction_t *function, hipModule_t module, const char *name)
{
    return hip_rc(hipModuleGetFunction(function, module, name));
}

static int load_adm_dwt_functions(AdmStateHip *s)
{
    int err = get_adm_function(&s->func_dwt_s123_combined_vert_kernel_0_0_int32_t,
                               s->adm_dwt_module, "dwt_s123_combined_vert_kernel_0_0_int32_t");
    if (!err)
        err = get_adm_function(&s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                               s->adm_dwt_module, "dwt_s123_combined_vert_kernel_32768_16_int32_t");
    if (!err)
        err = get_adm_function(&s->func_dwt_s123_combined_hori_kernel_16384_15, s->adm_dwt_module,
                               "dwt_s123_combined_hori_kernel_16384_15");
    if (!err)
        err = get_adm_function(&s->func_dwt_s123_combined_hori_kernel_32768_16, s->adm_dwt_module,
                               "dwt_s123_combined_hori_kernel_32768_16");
    if (!err)
        err = get_adm_function(&s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t,
                               s->adm_dwt_module,
                               "adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t");
    if (!err)
        err = get_adm_function(&s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t,
                               s->adm_dwt_module,
                               "adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t");
    return err;
}

static int load_adm_filter_functions(AdmStateHip *s)
{
    int err =
        get_adm_function(&s->func_adm_csf_kernel_1_4, s->adm_csf_module, "adm_csf_kernel_1_4");
    if (!err)
        err = get_adm_function(&s->func_i4_adm_csf_kernel_1_4, s->adm_csf_module,
                               "i4_adm_csf_kernel_1_4");
    if (!err)
        err = get_adm_function(&s->func_adm_csf_den_scale_line_kernel, s->adm_csf_den_module,
                               "adm_csf_den_scale_line_kernel_8_128");
    if (!err)
        err = get_adm_function(&s->func_adm_csf_den_s123_line_kernel, s->adm_csf_den_module,
                               "adm_csf_den_s123_line_kernel_8_128");
    if (!err)
        err = get_adm_function(&s->func_adm_cm_reduce_line_kernel_4, s->adm_cm_module,
                               "adm_cm_reduce_line_kernel_4");
    if (!err)
        err = get_adm_function(&s->func_adm_cm_line_kernel_8, s->adm_cm_module,
                               "adm_cm_line_kernel_8");
    if (!err)
        err = get_adm_function(&s->func_i4_adm_cm_line_kernel, s->adm_cm_module,
                               "i4_adm_cm_line_kernel");
    return err;
}

typedef struct AdmAllocationSizesHip {
    size_t band;
    size_t data;
    size_t temp;
    size_t accum;
    size_t accum_h;
    size_t result;
    size_t luma;
} AdmAllocationSizesHip;

static int checked_size_mul(size_t left, size_t right, size_t *result)
{
    if (right != 0 && left > SIZE_MAX / right)
        return -EOVERFLOW;
    *result = left * right;
    return 0;
}

static int get_adm_allocation_sizes(AdmStateHip *s, unsigned w, unsigned h, unsigned bpc,
                                    AdmAllocationSizesHip *sizes)
{
    const size_t half_w = ((size_t)w + 1) / 2;
    const size_t half_h = ((size_t)h + 1) / 2;
    size_t row_bytes;
    int err = checked_size_mul(w, sizeof(int32_t), &row_bytes);
    if (err || row_bytes > SIZE_MAX - (ADM_HIP_ALIGN - 1))
        return -EOVERFLOW;
    s->integer_stride = ADM_ALIGN_CEIL(row_bytes);
    err = checked_size_mul(half_w, sizeof(int32_t), &row_bytes);
    if (err || row_bytes > SIZE_MAX - (ADM_HIP_ALIGN - 1))
        return -EOVERFLOW;
    s->buf.ind_size_x = ADM_ALIGN_CEIL(row_bytes);
    err = checked_size_mul(half_h, sizeof(int32_t), &row_bytes);
    if (err || row_bytes > SIZE_MAX - (ADM_HIP_ALIGN - 1))
        return -EOVERFLOW;
    s->buf.ind_size_y = ADM_ALIGN_CEIL(row_bytes);
    if (checked_size_mul(s->buf.ind_size_x, half_h, &sizes->band) ||
        checked_size_mul(sizes->band, 11, &sizes->data) ||
        sizes->data > SIZE_MAX - (sizes->band / 2) * 11)
        return -EOVERFLOW;
    sizes->data += (sizes->band / 2) * 11;
    if (checked_size_mul(s->integer_stride, 4, &sizes->temp) ||
        checked_size_mul(sizes->temp, half_h, &sizes->temp) ||
        checked_size_mul(sizeof(uint64_t) * 3, w, &sizes->accum) ||
        checked_size_mul(sizes->accum, h, &sizes->accum) ||
        checked_size_mul(sizeof(uint64_t) * 3, h, &sizes->accum_h))
        return -EOVERFLOW;
    sizes->result = sizeof(uint64_t) * RES_BUFFER_SIZE;
    const size_t bytes_per_pixel = bpc > 8u ? sizeof(uint16_t) : sizeof(uint8_t);
    if (checked_size_mul(w, bytes_per_pixel, &s->luma_pitch) ||
        checked_size_mul(s->luma_pitch, h, &sizes->luma))
        return -EOVERFLOW;
    s->luma_h = h;
    return 0;
}

static int allocate_adm_buffers(AdmStateHip *s, const AdmAllocationSizesHip *sizes)
{
    int err = hip_rc(hipMalloc(&s->buf.data_buf, sizes->data));
    if (!err)
        err = hip_rc(hipMalloc(&s->buf.tmp_ref, sizes->temp));
    if (!err)
        err = hip_rc(hipMalloc(&s->buf.tmp_dis, sizes->temp));
    if (!err)
        err = hip_rc(hipMalloc(&s->buf.tmp_accum, sizes->accum));
    if (!err)
        err = hip_rc(hipMalloc(&s->buf.tmp_accum_h, sizes->accum_h));
    if (!err)
        err = hip_rc(hipMalloc(&s->buf.tmp_res, sizes->result));
    if (!err)
        err = hip_rc(hipHostMalloc(&s->buf.results_host, sizes->result, hipHostMallocDefault));
    if (!err)
        err = hip_rc(hipMalloc(&s->d_ref_luma, sizes->luma));
    if (!err)
        err = hip_rc(hipMalloc(&s->d_dis_luma, sizes->luma));
    return err;
}

static uint8_t *slice_adm_short_bands(AdmBufferHip *buf, uint8_t *top, size_t band_size)
{
    const size_t half = band_size / 2;
    buf->ref_dwt2.band_a = (int16_t *)top;
    buf->ref_dwt2.band_h = (int16_t *)(top + half);
    buf->ref_dwt2.band_v = (int16_t *)(top + 2u * half);
    buf->ref_dwt2.band_d = (int16_t *)(top + 3u * half);
    top += 4u * half;
    buf->dis_dwt2.band_a = (int16_t *)top;
    buf->dis_dwt2.band_h = (int16_t *)(top + half);
    buf->dis_dwt2.band_v = (int16_t *)(top + 2u * half);
    buf->dis_dwt2.band_d = (int16_t *)(top + 3u * half);
    top += 4u * half;
    buf->csf_f.band_a = NULL;
    buf->csf_f.band_h = (int16_t *)top;
    buf->csf_f.band_v = (int16_t *)(top + half);
    buf->csf_f.band_d = (int16_t *)(top + 2u * half);
    return top + 3u * half;
}

static void slice_adm_integer_bands(AdmBufferHip *buf, uint8_t *top, size_t band_size)
{
    buf->i4_ref_dwt2.band_a = (int32_t *)top;
    buf->i4_ref_dwt2.band_h = (int32_t *)(top + band_size);
    buf->i4_ref_dwt2.band_v = (int32_t *)(top + 2u * band_size);
    buf->i4_ref_dwt2.band_d = (int32_t *)(top + 3u * band_size);
    top += 4u * band_size;
    buf->i4_dis_dwt2.band_a = (int32_t *)top;
    buf->i4_dis_dwt2.band_h = (int32_t *)(top + band_size);
    buf->i4_dis_dwt2.band_v = (int32_t *)(top + 2u * band_size);
    buf->i4_dis_dwt2.band_d = (int32_t *)(top + 3u * band_size);
    top += 4u * band_size;
    buf->i4_csf_f.band_a = NULL;
    buf->i4_csf_f.band_h = (int32_t *)top;
    buf->i4_csf_f.band_v = (int32_t *)(top + band_size);
    buf->i4_csf_f.band_d = (int32_t *)(top + 2u * band_size);
}

static void slice_adm_results(AdmBufferHip *buf)
{
    const size_t cm_stride = 3u * sizeof(int64_t);
    const size_t csf_stride = 3u * sizeof(uint64_t);
    uint8_t *results = buf->tmp_res;
    for (int i = 0; i < 4; ++i)
        buf->adm_cm[i] = (int64_t *)(results + (size_t)i * cm_stride);
    results += 4u * cm_stride;
    for (int i = 0; i < 4; ++i)
        buf->adm_csf_den[i] = (uint64_t *)(results + (size_t)i * csf_stride);
}

static int init_adm_runtime(AdmStateHip *s, unsigned w, unsigned h, unsigned bpc)
{
    AdmAllocationSizesHip sizes;
    int err = create_adm_control_resources(s);
    if (!err)
        err = load_adm_modules(s);
    if (!err)
        err = load_adm_dwt_functions(s);
    if (!err)
        err = load_adm_filter_functions(s);
    if (!err)
        err = get_adm_allocation_sizes(s, w, h, bpc, &sizes);
    if (!err)
        err = allocate_adm_buffers(s, &sizes);
    if (err)
        return err;
    uint8_t *top = slice_adm_short_bands(&s->buf, s->buf.data_buf, sizes.band);
    slice_adm_integer_bands(&s->buf, top, sizes.band);
    slice_adm_results(&s->buf);
    return 0;
}

#endif /* HAVE_HIPCC */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    AdmStateHip *const s = fex->priv;
    int err = init_adm_configuration(s);
#ifndef HAVE_HIPCC
    (void)bpc;
    (void)w;
    (void)h;
    return err ? err : -ENOSYS;
#else
    if (!err)
        err = init_adm_runtime(s, w, h, bpc);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err) {
        const int cleanup_err = release_adm_runtime(s, false);
        if (cleanup_err)
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "adm_hip initialization cleanup failed: %d\n",
                     cleanup_err);
    }
    return err;
#endif
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
    return -ENOSYS;
#else
    return integer_compute_adm_hip(s, ref_pic, dist_pic, &s->buf, s->adm_enhn_gain_limit,
                                   s->adm_norm_view_dist, s->adm_ref_display_height);
#endif
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    AdmStateHip *s = fex->priv;

#ifndef HAVE_HIPCC
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    hipError_t hip_err = hipStreamSynchronize(s->str);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);

    write_score_parameters_adm_hip params = {
        .feature_collector = feature_collector,
        .s = s,
        .index = index,
        .w = s->submit_w,
        .h = s->submit_h,
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
    AdmStateHip *const s = fex->priv;
    int rc = 0;
#ifdef HAVE_HIPCC
    rc = release_adm_runtime(s, true);
#endif /* HAVE_HIPCC */
    const int dict_err = vmaf_dictionary_free(&s->feature_name_dict);
    if (dict_err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "adm_hip feature dictionary free failed: %d\n", dict_err);
        if (!rc)
            rc = dict_err;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Feature extractor descriptor                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"VMAF_integer_feature_adm2_score",
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
    /*
     * VMAF_FEATURE_EXTRACTOR_HIP flag bit is reserved (mirrors the
     * pattern used by the CUDA twin which uses VMAF_FEATURE_EXTRACTOR_CUDA).
     * The runtime PR (T7-10b) wires in the buffer-type plumbing and flips
     * this flag on. Until then, callers receive -ENOSYS from init() on
     * non-ROCm builds.
     */
    .flags = 0,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
