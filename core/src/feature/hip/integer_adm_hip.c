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

#include "vmaf_nullptr.h"

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

#include "hip/integer_adm_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
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
    double scale[8];
    double score;
    double numerator;
    double denominator;
} AdmScoresHip;

static void calculate_scores(const write_score_parameters_adm_hip *params, AdmScoresHip *scores)
{
    const AdmStateHip *s = params->s;
    double num = 0;
    double den = 0;
    unsigned w = params->w;
    unsigned h = params->h;
    int64_t *adm_cm = (int64_t *)s->buf.results_host;
    uint64_t *adm_csf = &((uint64_t *)s->buf.results_host)[RES_BUFFER_SIZE / 2];

    for (unsigned scale = 0; scale < 4; ++scale) {
        float num_scale;
        float den_scale;
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        conclude_adm_cm(&adm_cm[scale * 3], (int)h, (int)w, (int)scale, (float)s->adm_noise_weight,
                        s->adm_p_norm, &num_scale);
        conclude_adm_csf_den(&adm_csf[scale * 3], (int)h, (int)w, (int)scale, &den_scale,
                             &s->rfactor[scale * 3], (float)s->adm_noise_weight);
        if (scale == 0u && s->adm_skip_scale0) {
            scores->scale[0] = 0.0;
            scores->scale[1] = 1e-10;
            continue;
        }
        num += num_scale;
        den += den_scale;
        scores->scale[2 * scale + 0] = num_scale;
        scores->scale[2 * scale + 1] = den_scale;
    }

    const double numden_limit = 1e-10 * ((double)params->w * params->h) / (1920.0 * 1080.0);
    num = num < numden_limit ? 0 : num;
    den = den < numden_limit ? 0 : den;
    scores->score = den == 0.0 ? 1.0 : num / den;
    scores->numerator = num;
    scores->denominator = den;
}

static int append_standard_scores(const write_score_parameters_adm_hip *params,
                                  const AdmScoresHip *scores)
{
    VmafFeatureCollector *collector = params->feature_collector;
    const AdmStateHip *s = params->s;
    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_adm2_score", scores->score,
                                                   params->index);
    static const char *scale_names[4] = {"integer_adm_scale0", "integer_adm_scale1",
                                         "integer_adm_scale2", "integer_adm_scale3"};
    for (unsigned scale = 0; scale < 4; scale++) {
        const double value = scores->scale[2 * scale] / scores->scale[2 * scale + 1];
        err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                       scale_names[scale], value, params->index);
    }
    return err;
}

static int append_debug_scores(const write_score_parameters_adm_hip *params,
                               const AdmScoresHip *scores)
{
    VmafFeatureCollector *collector = params->feature_collector;
    const AdmStateHip *s = params->s;
    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, "integer_adm",
                                                   scores->score, params->index);
    err |= vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "integer_adm_num", scores->numerator, params->index);
    err |= vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "integer_adm_den", scores->denominator, params->index);
    static const char *component_names[8] = {
        "integer_adm_num_scale0", "integer_adm_den_scale0", "integer_adm_num_scale1",
        "integer_adm_den_scale1", "integer_adm_num_scale2", "integer_adm_den_scale2",
        "integer_adm_num_scale3", "integer_adm_den_scale3",
    };
    for (unsigned component = 0; component < 8; component++) {
        err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                       component_names[component],
                                                       scores->scale[component], params->index);
    }
    return err;
}

static void write_scores(const write_score_parameters_adm_hip *params)
{
    AdmScoresHip scores = {0};
    calculate_scores(params, &scores);
    int err = append_standard_scores(params, &scores);
    if (params->s->debug)
        err |= append_debug_scores(params, &scores);
    (void)err;
}

/* ------------------------------------------------------------------ */
/* VmafOption table                                                     */
/* ------------------------------------------------------------------ */

#define ADM_OPTION_DOUBLE(NAME, ALIAS, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM)                     \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .alias = (ALIAS),                                                                          \
        .help = (HELP),                                                                            \
        .offset = offsetof(AdmStateHip, FIELD),                                                    \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define ADM_OPTION_INT(NAME, ALIAS, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM)                        \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .alias = (ALIAS),                                                                          \
        .help = (HELP),                                                                            \
        .offset = offsetof(AdmStateHip, FIELD),                                                    \
        .type = VMAF_OPT_TYPE_INT,                                                                 \
        .default_val.i = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define ADM_OPTION_BOOL(NAME, ALIAS, HELP, FIELD, FLAGS)                                           \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .alias = (ALIAS),                                                                          \
        .help = (HELP),                                                                            \
        .offset = offsetof(AdmStateHip, FIELD),                                                    \
        .type = VMAF_OPT_TYPE_BOOL,                                                                \
        .default_val.b = false,                                                                    \
        .flags = (FLAGS),                                                                          \
    }

static const VmafOption options_hip[] = {
    ADM_OPTION_BOOL("debug", VMAF_NULLPTR, "debug mode: enable additional output", debug, 0),
    ADM_OPTION_DOUBLE("adm_csf_scale", "scf",
                      "scale coefficient for the horizontal & vertical direction terms of CSF",
                      adm_csf_scale, DEFAULT_ADM_CSF_SCALE, 0.0, 50.0),
    ADM_OPTION_DOUBLE("adm_csf_diag_scale", "scfd",
                      "scale coefficient for the diagonal direction term of CSF",
                      adm_csf_diag_scale, DEFAULT_ADM_CSF_DIAG_SCALE, 0.0, 50.0),
    ADM_OPTION_DOUBLE("adm_dlm_weight", "dlmw",
                      "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
                      adm_dlm_weight, 0.5, 0.0, 1.0),
    ADM_OPTION_DOUBLE("adm_enhn_gain_limit", "egl",
                      "enhancement gain imposed on adm, must be >= 1.0, "
                      "where 1.0 means the gain is completely disabled",
                      adm_enhn_gain_limit, DEFAULT_ADM_ENHN_GAIN_LIMIT, 1.0,
                      DEFAULT_ADM_ENHN_GAIN_LIMIT),
    ADM_OPTION_DOUBLE(
        "adm_norm_view_dist", "nvd",
        "normalized viewing distance = viewing distance / ref display's physical height",
        adm_norm_view_dist, DEFAULT_ADM_NORM_VIEW_DIST, 0.75, 24.0),
    ADM_OPTION_INT("adm_ref_display_height", "rdh", "reference display height in pixels",
                   adm_ref_display_height, DEFAULT_ADM_REF_DISPLAY_HEIGHT, 1, 4320),
    ADM_OPTION_INT("adm_csf_mode", "csf", "contrast sensitivity function", adm_csf_mode,
                   DEFAULT_ADM_CSF_MODE, 0, 3),
    ADM_OPTION_DOUBLE("adm_noise_weight", "nw", "noise weight", adm_noise_weight,
                      DEFAULT_ADM_NOISE_WEIGHT, 0.0, 1500.0),
    ADM_OPTION_BOOL("adm_skip_scale0", "ssz", "skip the calculation of scale 0", adm_skip_scale0,
                    VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_OPTION_DOUBLE("adm_min_val", "min",
                      "minimum value allowed; lower values will be clipped to this value",
                      adm_min_val, DEFAULT_ADM_MIN_VAL, 0.0, 1.0),
    ADM_OPTION_DOUBLE("adm_p_norm", "apn",
                      "p-norm exponent for fixed-point ADM contrast-measure finalisation",
                      adm_p_norm, 3.0, 1.0, 20.0),
    {0}};

#undef ADM_OPTION_BOOL
#undef ADM_OPTION_INT
#undef ADM_OPTION_DOUBLE

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

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

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
        (uint32_t)DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
        (uint32_t)DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1, (uint32_t)vert_out_tile_cols,
        (uint32_t)(vert_out_tile_rows / rows_per_thread), 1, 0, c_stream, args, VMAF_NULLPTR);
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
        (uint32_t)DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
        (uint32_t)DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1, (uint32_t)vert_out_tile_cols,
        (uint32_t)(vert_out_tile_rows / rows_per_thread), 1, 0, c_stream, args, VMAF_NULLPTR);
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
                                   (uint32_t)DIV_ROUND_UP(w, 128), (uint32_t)BLOCK_Y, 1, 128, 1, 1,
                                   0, cu_stream, args_vert, VMAF_NULLPTR);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    case 2: /* fall-through */
    case 3:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                                   (uint32_t)DIV_ROUND_UP(w, 128), (uint32_t)BLOCK_Y, 1, 128, 1, 1,
                                   0, cu_stream, args_vert, VMAF_NULLPTR);
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
                                   (uint32_t)DIV_ROUND_UP((w + 1) / 2, 128), (uint32_t)BLOCK_Y, 1,
                                   128, 1, 1, 0, cu_stream, args_hori, VMAF_NULLPTR);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    case 2:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_hori_kernel_32768_16,
                                   (uint32_t)DIV_ROUND_UP((w + 1) / 2, 128), (uint32_t)BLOCK_Y, 1,
                                   128, 1, 1, 0, cu_stream, args_hori, VMAF_NULLPTR);
        if (rc != hipSuccess)
            return hip_rc(rc);
        break;
    case 3:
        rc = hipModuleLaunchKernel(s->func_dwt_s123_combined_hori_kernel_16384_15,
                                   (uint32_t)DIV_ROUND_UP((w + 1) / 2, 128), (uint32_t)BLOCK_Y, 1,
                                   128, 1, 1, 0, cu_stream, args_hori, VMAF_NULLPTR);
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
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_csf_kernel_1_4, (uint32_t)DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
        (uint32_t)DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3, (uint32_t)BLOCKX,
        (uint32_t)BLOCKY, 1, 0, c_stream, args, VMAF_NULLPTR);
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
                              (uint32_t)DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
                              (uint32_t)DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3,
                              (uint32_t)BLOCKX, (uint32_t)BLOCKY, 1, 0, c_stream, args, VMAF_NULLPTR);
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
        (uint32_t)DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread), (uint32_t)buffer_h, 3,
        (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, VMAF_NULLPTR);
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
        (uint32_t)DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread), (uint32_t)buffer_h, 3,
        (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, VMAF_NULLPTR);
    return hip_rc(rc);
}

typedef struct WarpShiftHip {
    uint32_t shift_cub[3];
    uint32_t add_shift_cub[3];
    uint32_t shift_sq[3];
    uint32_t add_shift_sq[3];
} WarpShiftHip;

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
            s->func_i4_adm_cm_line_kernel, (uint32_t)DIV_ROUND_UP(buffer_stride, BLOCKX),
            (uint32_t)buffer_h, 3, (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, VMAF_NULLPTR);
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
                                  (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, VMAF_NULLPTR);
        if (rc != hipSuccess)
            return hip_rc(rc);
    }
    return 0;
}

static WarpShiftHip adm_cm_warp_shifts(int w, int h, uint32_t *shift_inner_accum,
                                       uint32_t *add_shift_inner_accum)
{
    const int fixed_shift[3] = {4, 4, 3};
    const int32_t shift_xsq[3] = {29, 29, 30};
    const int32_t add_shift_xsq[3] = {268435456, 268435456, 536870912};
    WarpShiftHip shifts;
    for (int band = 0; band < 3; ++band) {
        shifts.shift_cub[band] = (uint32_t)ceilf(log2f((float)w));
        shifts.shift_cub[band] -= (uint32_t)fixed_shift[band];
        shifts.shift_sq[band] = (uint32_t)shift_xsq[band];
        shifts.add_shift_sq[band] = (uint32_t)add_shift_xsq[band];
        shifts.add_shift_cub[band] = 1u << (shifts.shift_cub[band] - 1u);
    }
    *shift_inner_accum = (uint32_t)ceilf(log2f((float)h));
    *add_shift_inner_accum = 1u << (*shift_inner_accum - 1u);
    return shifts;
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

    uint32_t shift_inner_accum;
    uint32_t add_shift_inner_accum;
    WarpShiftHip ws = adm_cm_warp_shifts(w, h, &shift_inner_accum, &add_shift_inner_accum);

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
                                  (uint32_t)DIV_ROUND_UP(buffer_h, BLOCKY * rows_per_thread), 3,
                                  (uint32_t)BLOCKX, (uint32_t)BLOCKY, 1, 0, c_stream, args, VMAF_NULLPTR);
        if (rc != hipSuccess)
            return hip_rc(rc);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main per-frame computation                                           */
/* ------------------------------------------------------------------ */

typedef struct AdmScaleStateHip {
    int w;
    int h;
    size_t ref_stride;
    size_t dis_stride;
    size_t buf_stride;
    int32_t *ref_scale;
    int32_t *dis_scale;
} AdmScaleStateHip;

static void adm_fixed_parameters_init(AdmStateHip *s, AdmFixedParametersHip *p, int w, int h,
                                      double gain_limit, double norm_view_dist,
                                      int ref_display_height)
{
    memset(p, 0, sizeof(*p));
    const int32_t lo[4] = {15826, 27411, 7345, -4240};
    const int32_t hi[4] = {-4240, -7345, 27411, -15826};
    memcpy(p->dwt2_db2_coeffs_lo, lo, sizeof(lo));
    memcpy(p->dwt2_db2_coeffs_hi, hi, sizeof(hi));
    p->dwt2_db2_coeffs_lo_sum = 46342;
    p->dwt2_db2_coeffs_hi_sum = 0;
    p->log2_w = log2f((float)w);
    p->log2_h = log2f((float)h);
    p->adm_ref_display_height = ref_display_height;
    p->adm_norm_view_dist = norm_view_dist;
    p->adm_enhn_gain_limit = gain_limit;

    const double pow2_32 = pow(2, 32);
    for (unsigned scale = 0; scale < 4; ++scale) {
        const AdmCsfFactors factors =
            adm_csf_factors((int)scale, norm_view_dist, ref_display_height, s->adm_csf_mode,
                            s->adm_csf_scale, s->adm_csf_diag_scale);
        p->rfactor[scale * 3] = factors.factor1;
        p->rfactor[scale * 3 + 1] = factors.factor1;
        p->rfactor[scale * 3 + 2] = factors.factor2;
        if (scale == 0) {
            uint16_t integer_factors[3];
            adm_csf_rfactor_scale0(p->rfactor, norm_view_dist, ref_display_height, s->adm_csf_mode,
                                   integer_factors);
            p->i_rfactor[0] = integer_factors[0];
            p->i_rfactor[1] = integer_factors[1];
            p->i_rfactor[2] = integer_factors[2];
        } else {
            p->i_rfactor[scale * 3] = (uint32_t)(p->rfactor[scale * 3] * pow2_32);
            p->i_rfactor[scale * 3 + 1] = (uint32_t)(p->rfactor[scale * 3 + 1] * pow2_32);
            p->i_rfactor[scale * 3 + 2] = (uint32_t)(p->rfactor[scale * 3 + 2] * pow2_32);
        }
    }
    memcpy(s->rfactor, p->rfactor, sizeof(p->rfactor));
}

static int adm_stage_luma(AdmStateHip *s, const VmafPicture *ref_pic, const VmafPicture *dis_pic,
                          int w, int h)
{
    const size_t bpp = ref_pic->bpc > 8 ? sizeof(uint16_t) : sizeof(uint8_t);
    const size_t row_bytes = (size_t)w * bpp;
    hipError_t rc =
        hipMemcpy2DAsync(s->d_ref_luma, s->luma_pitch, ref_pic->data[0], (size_t)ref_pic->stride[0],
                         row_bytes, (size_t)h, hipMemcpyHostToDevice, s->str);
    if (rc != hipSuccess)
        return hip_rc(rc);
    rc =
        hipMemcpy2DAsync(s->d_dis_luma, s->luma_pitch, dis_pic->data[0], (size_t)dis_pic->stride[0],
                         row_bytes, (size_t)h, hipMemcpyHostToDevice, s->str);
    if (rc != hipSuccess)
        return hip_rc(rc);
    return hip_rc(hipStreamSynchronize(s->str));
}

static int adm_initial_dwt(AdmStateHip *s, AdmBufferHip *buf, const VmafPicture *ref_pic,
                           const VmafPicture *dis_pic, const AdmScaleStateHip *scale,
                           AdmFixedParametersHip *p)
{
    int err;
    if (ref_pic->bpc == 8) {
        err = dwt2_8_device_hip(s, (const uint8_t *)s->d_ref_luma, &buf->ref_dwt2, buf->i4_ref_dwt2,
                                scale->w, scale->h, (int)scale->ref_stride, (int)scale->buf_stride,
                                p, 0);
        if (err != 0)
            return err;
        return dwt2_8_device_hip(s, (const uint8_t *)s->d_dis_luma, &buf->dis_dwt2,
                                 buf->i4_dis_dwt2, scale->w, scale->h, (int)scale->dis_stride,
                                 (int)scale->buf_stride, p, 0);
    }
    err = dwt2_16_device_hip(s, (const uint16_t *)s->d_ref_luma, &buf->ref_dwt2, buf->i4_ref_dwt2,
                             scale->w, scale->h, (int)scale->ref_stride, (int)scale->buf_stride,
                             (int)ref_pic->bpc, p, 0);
    if (err != 0)
        return err;
    return dwt2_16_device_hip(s, (const uint16_t *)s->d_dis_luma, &buf->dis_dwt2, buf->i4_dis_dwt2,
                              scale->w, scale->h, (int)scale->dis_stride, (int)scale->buf_stride,
                              (int)dis_pic->bpc, p, 0);
}

static int adm_wait_initial_dwt(AdmStateHip *s)
{
    hipError_t rc = hipEventRecord(s->ref_event, 0);
    if (rc != hipSuccess)
        return hip_rc(rc);
    rc = hipEventRecord(s->dis_event, 0);
    if (rc != hipSuccess)
        return hip_rc(rc);
    rc = hipStreamWaitEvent(s->str, s->dis_event, 0);
    if (rc != hipSuccess)
        return hip_rc(rc);
    return hip_rc(hipStreamWaitEvent(s->str, s->ref_event, 0));
}

static int adm_process_initial_scale(AdmStateHip *s, AdmBufferHip *buf, const VmafPicture *ref_pic,
                                     const VmafPicture *dis_pic, AdmScaleStateHip *scale,
                                     AdmFixedParametersHip *p)
{
    int err = adm_initial_dwt(s, buf, ref_pic, dis_pic, scale, p);
    if (err == 0)
        err = adm_wait_initial_dwt(s);
    scale->w = (scale->w + 1) / 2;
    scale->h = (scale->h + 1) / 2;
    if (err == 0)
        err = adm_csf_den_scale_device_hip(s, buf, scale->w, scale->h, (int)scale->buf_stride,
                                           s->str);
    if (err == 0)
        err = adm_csf_device_hip(s, buf, scale->w, scale->h, (int)scale->buf_stride, p, s->str);
    if (err == 0)
        err = adm_cm_device_hip(s, buf, scale->w, scale->h, (int)scale->buf_stride,
                                (int)scale->buf_stride, p, s->str);
    return err;
}

static int adm_process_later_scale(AdmStateHip *s, AdmBufferHip *buf, unsigned scale_index,
                                   AdmScaleStateHip *scale, AdmFixedParametersHip *p)
{
    int err = adm_dwt2_s123_combined_device_hip(
        s, scale->ref_scale, (int32_t *)buf->tmp_ref, buf->i4_ref_dwt2, scale->w, scale->h,
        (int)scale->ref_stride, (int)scale->buf_stride, (int)scale_index, p, s->str);
    if (err != 0)
        return err;
    err = adm_dwt2_s123_combined_device_hip(
        s, scale->dis_scale, (int32_t *)buf->tmp_dis, buf->i4_dis_dwt2, scale->w, scale->h,
        (int)scale->dis_stride, (int)scale->buf_stride, (int)scale_index, p, s->str);
    if (err != 0)
        return err;

    scale->w = (scale->w + 1) / 2;
    scale->h = (scale->h + 1) / 2;
    err = adm_csf_den_s123_device_hip(s, buf, (int)scale_index, scale->w, scale->h,
                                      (int)scale->buf_stride, s->str);
    if (err == 0)
        err = i4_adm_csf_device_hip(s, buf, (int)scale_index, scale->w, scale->h,
                                    (int)scale->buf_stride, p, s->str);
    if (err == 0)
        err = i4_adm_cm_device_hip(s, buf, scale->w, scale->h, (int)scale->buf_stride,
                                   (int)scale->buf_stride, (int)scale_index, p, s->str);
    return err;
}

static int integer_compute_adm_hip(AdmStateHip *s, const VmafPicture *ref_pic, const VmafPicture *dis_pic,
                                   AdmBufferHip *buf, double adm_enhn_gain_limit,
                                   double adm_norm_view_dist, int adm_ref_display_height)
{
    int w = (int)ref_pic->w[0];
    int h = (int)ref_pic->h[0];
    AdmFixedParametersHip p;
    adm_fixed_parameters_init(s, &p, w, h, adm_enhn_gain_limit, adm_norm_view_dist,
                              adm_ref_display_height);

    hipError_t hip_err = hipMemsetAsync(buf->tmp_res, 0, sizeof(int64_t) * RES_BUFFER_SIZE, s->str);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);

    int err = adm_stage_luma(s, ref_pic, dis_pic, w, h);
    if (err != 0)
        return err;

    AdmScaleStateHip scale_state = {
        .w = w,
        .h = h,
        .ref_stride = (size_t)w,
        .dis_stride = (size_t)w,
        .buf_stride = buf->ind_size_x >> 2,
        .ref_scale = VMAF_NULLPTR,
        .dis_scale = VMAF_NULLPTR,
    };

    for (unsigned scale = 0; scale < 4; ++scale) {
        err = scale == 0 ? adm_process_initial_scale(s, buf, ref_pic, dis_pic, &scale_state, &p) :
                           adm_process_later_scale(s, buf, scale, &scale_state, &p);
        if (err != 0)
            return err;
        scale_state.ref_scale = buf->i4_ref_dwt2.band_a;
        scale_state.dis_scale = buf->i4_dis_dwt2.band_a;
        scale_state.ref_stride = scale_state.buf_stride;
        scale_state.dis_stride = scale_state.buf_stride;
    }

    hip_err = hipMemcpyAsync(buf->results_host, buf->tmp_res, sizeof(int64_t) * RES_BUFFER_SIZE,
                             hipMemcpyDeviceToHost, s->str);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);
    hip_err = hipEventRecord(s->finished, s->str);
    return hip_rc(hip_err);
}

#endif /* HAVE_HIPCC */

/* ================================================================== */
/* init / submit / collect / flush / close                             */
/* ================================================================== */

/* ALIGN_CEIL: round up to the nearest multiple of 64-byte cache line. */
#define ADM_HIP_ALIGN 64
#define ADM_ALIGN_CEIL(x) (((x) + ADM_HIP_ALIGN - 1) & ~(size_t)(ADM_HIP_ALIGN - 1))

#ifdef HAVE_HIPCC
static int adm_configuration_init(AdmStateHip *s)
{
    if (s->adm_norm_view_dist * s->adm_ref_display_height <
        DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT)
        return -EINVAL;
    int err = adm_csf_config_check(s);
    if (err != 0)
        return err;

    const double pow2_32 = pow(2, 32);
    for (unsigned scale = 0; scale < 4; scale++) {
        const AdmCsfFactors factors =
            adm_csf_factors((int)scale, s->adm_norm_view_dist, s->adm_ref_display_height,
                            s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale);
        s->rfactor[scale * 3] = factors.factor1;
        s->rfactor[scale * 3 + 1] = factors.factor1;
        s->rfactor[scale * 3 + 2] = factors.factor2;
        if (scale == 0) {
            uint16_t integer_factors[3];
            adm_csf_rfactor_scale0(s->rfactor, s->adm_norm_view_dist, s->adm_ref_display_height,
                                   s->adm_csf_mode, integer_factors);
            s->i_rfactor[0] = integer_factors[0];
            s->i_rfactor[1] = integer_factors[1];
            s->i_rfactor[2] = integer_factors[2];
        } else {
            s->i_rfactor[scale * 3] = (uint32_t)(s->rfactor[scale * 3] * pow2_32);
            s->i_rfactor[scale * 3 + 1] = (uint32_t)(s->rfactor[scale * 3 + 1] * pow2_32);
            s->i_rfactor[scale * 3 + 2] = (uint32_t)(s->rfactor[scale * 3 + 2] * pow2_32);
        }
    }
    return 0;
}

static void adm_device_free(void **buffer)
{
    if (*buffer != VMAF_NULLPTR)
        (void)hipFree(*buffer);
    *buffer = VMAF_NULLPTR;
}

static void adm_host_free(void **buffer)
{
    if (*buffer != VMAF_NULLPTR)
        (void)hipHostFree(*buffer);
    *buffer = VMAF_NULLPTR;
}

static void adm_buffers_free(AdmStateHip *s)
{
    adm_device_free(&s->d_ref_luma);
    adm_device_free(&s->d_dis_luma);
    adm_host_free(&s->buf.results_host);
    adm_device_free(&s->buf.tmp_res);
    adm_device_free(&s->buf.tmp_accum_h);
    adm_device_free(&s->buf.tmp_accum);
    adm_device_free(&s->buf.tmp_dis);
    adm_device_free(&s->buf.tmp_ref);
    adm_device_free(&s->buf.data_buf);
}

static void adm_module_unload(hipModule_t *module)
{
    if (*module != VMAF_NULLPTR)
        (void)hipModuleUnload(*module);
    *module = VMAF_NULLPTR;
}

static void adm_modules_unload(AdmStateHip *s)
{
    adm_module_unload(&s->adm_cm_module);
    adm_module_unload(&s->adm_csf_den_module);
    adm_module_unload(&s->adm_csf_module);
    adm_module_unload(&s->adm_dwt_module);
}

static void adm_init_runtime_destroy(AdmStateHip *s)
{
    if (s->dis_event != VMAF_NULLPTR)
        (void)hipEventDestroy(s->dis_event);
    if (s->ref_event != VMAF_NULLPTR)
        (void)hipEventDestroy(s->ref_event);
    if (s->finished != VMAF_NULLPTR)
        (void)hipEventDestroy(s->finished);
    if (s->str != VMAF_NULLPTR)
        (void)hipStreamDestroy(s->str);
    s->dis_event = VMAF_NULLPTR;
    s->ref_event = VMAF_NULLPTR;
    s->finished = VMAF_NULLPTR;
    s->str = VMAF_NULLPTR;
}

static int adm_runtime_create(AdmStateHip *s)
{
    hipError_t rc = hipStreamCreateWithFlags(&s->str, hipStreamNonBlocking);
    if (rc == hipSuccess)
        rc = hipEventCreateWithFlags(&s->finished, hipEventDefault);
    if (rc == hipSuccess)
        rc = hipEventCreateWithFlags(&s->ref_event, hipEventDefault);
    if (rc == hipSuccess)
        rc = hipEventCreateWithFlags(&s->dis_event, hipEventDefault);
    if (rc != hipSuccess)
        adm_init_runtime_destroy(s);
    return hip_rc(rc);
}

static int adm_modules_load(AdmStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->adm_dwt_module, (const void *)adm_dwt2_hsaco);
    if (rc == hipSuccess)
        rc = hipModuleLoadData(&s->adm_csf_module, (const void *)adm_csf_hsaco);
    if (rc == hipSuccess)
        rc = hipModuleLoadData(&s->adm_csf_den_module, (const void *)adm_csf_den_hsaco);
    if (rc == hipSuccess)
        rc = hipModuleLoadData(&s->adm_cm_module, (const void *)adm_cm_hsaco);
    if (rc != hipSuccess)
        adm_modules_unload(s);
    return hip_rc(rc);
}

static int adm_get_function(hipModule_t module, hipFunction_t *function, const char *name)
{
    return hip_rc(hipModuleGetFunction(function, module, name));
}

static int adm_dwt_functions_load(AdmStateHip *s)
{
    int err =
        adm_get_function(s->adm_dwt_module, &s->func_dwt_s123_combined_vert_kernel_0_0_int32_t,
                         "dwt_s123_combined_vert_kernel_0_0_int32_t");
    if (err == 0)
        err = adm_get_function(s->adm_dwt_module,
                               &s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                               "dwt_s123_combined_vert_kernel_32768_16_int32_t");
    if (err == 0)
        err = adm_get_function(s->adm_dwt_module, &s->func_dwt_s123_combined_hori_kernel_16384_15,
                               "dwt_s123_combined_hori_kernel_16384_15");
    if (err == 0)
        err = adm_get_function(s->adm_dwt_module, &s->func_dwt_s123_combined_hori_kernel_32768_16,
                               "dwt_s123_combined_hori_kernel_32768_16");
    if (err == 0)
        err = adm_get_function(s->adm_dwt_module,
                               &s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t,
                               "adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t");
    if (err == 0)
        err = adm_get_function(s->adm_dwt_module,
                               &s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t,
                               "adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t");
    return err;
}

static int adm_feature_functions_load(AdmStateHip *s)
{
    int err =
        adm_get_function(s->adm_csf_module, &s->func_adm_csf_kernel_1_4, "adm_csf_kernel_1_4");
    if (err == 0)
        err = adm_get_function(s->adm_csf_module, &s->func_i4_adm_csf_kernel_1_4,
                               "i4_adm_csf_kernel_1_4");
    if (err == 0)
        err = adm_get_function(s->adm_csf_den_module, &s->func_adm_csf_den_scale_line_kernel,
                               "adm_csf_den_scale_line_kernel_8_128");
    if (err == 0)
        err = adm_get_function(s->adm_csf_den_module, &s->func_adm_csf_den_s123_line_kernel,
                               "adm_csf_den_s123_line_kernel_8_128");
    if (err == 0)
        err = adm_get_function(s->adm_cm_module, &s->func_adm_cm_reduce_line_kernel_4,
                               "adm_cm_reduce_line_kernel_4");
    if (err == 0)
        err = adm_get_function(s->adm_cm_module, &s->func_adm_cm_line_kernel_8,
                               "adm_cm_line_kernel_8");
    if (err == 0)
        err = adm_get_function(s->adm_cm_module, &s->func_i4_adm_cm_line_kernel,
                               "i4_adm_cm_line_kernel");
    return err;
}

static int adm_buffers_alloc(AdmStateHip *s, unsigned bpc, unsigned w, unsigned h,
                             size_t *band_size)
{
    s->integer_stride = ADM_ALIGN_CEIL(w * sizeof(int32_t));
    s->buf.ind_size_x = ADM_ALIGN_CEIL(((w + 1) / 2) * sizeof(int32_t));
    s->buf.ind_size_y = ADM_ALIGN_CEIL(((h + 1) / 2) * sizeof(int32_t));
    *band_size = s->buf.ind_size_x * ((h + 1) / 2);

    hipError_t rc = hipMalloc(&s->buf.data_buf, *band_size * 11 + *band_size / 2 * 11);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->buf.tmp_ref, s->integer_stride * 4 * ((h + 1) / 2));
    if (rc == hipSuccess)
        rc = hipMalloc(&s->buf.tmp_dis, s->integer_stride * 4 * ((h + 1) / 2));
    if (rc == hipSuccess)
        rc = hipMalloc(&s->buf.tmp_accum, sizeof(uint64_t) * 3u * (size_t)w * (size_t)h);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->buf.tmp_accum_h, sizeof(uint64_t) * 3u * (size_t)h);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->buf.tmp_res, sizeof(uint64_t) * RES_BUFFER_SIZE);
    if (rc == hipSuccess)
        rc = hipHostMalloc(&s->buf.results_host, sizeof(uint64_t) * RES_BUFFER_SIZE,
                           hipHostMallocDefault);
    s->luma_pitch = (size_t)w * (bpc > 8u ? sizeof(uint16_t) : sizeof(uint8_t));
    s->luma_h = h;
    if (rc == hipSuccess)
        rc = hipMalloc(&s->d_ref_luma, s->luma_pitch * (size_t)h);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->d_dis_luma, s->luma_pitch * (size_t)h);
    return hip_rc(rc);
}

static void adm_slice_bands(AdmStateHip *s, size_t band_size)
{
    uint8_t *top = (uint8_t *)s->buf.data_buf;
    const size_t half = band_size / 2;
    s->buf.ref_dwt2.band_a = (int16_t *)top;
    s->buf.ref_dwt2.band_h = (int16_t *)(top + half);
    s->buf.ref_dwt2.band_v = (int16_t *)(top + 2u * half);
    s->buf.ref_dwt2.band_d = (int16_t *)(top + 3u * half);
    top += 4u * half;
    s->buf.dis_dwt2.band_a = (int16_t *)top;
    s->buf.dis_dwt2.band_h = (int16_t *)(top + half);
    s->buf.dis_dwt2.band_v = (int16_t *)(top + 2u * half);
    s->buf.dis_dwt2.band_d = (int16_t *)(top + 3u * half);
    top += 4u * half;
    s->buf.csf_f.band_a = VMAF_NULLPTR;
    s->buf.csf_f.band_h = (int16_t *)top;
    s->buf.csf_f.band_v = (int16_t *)(top + half);
    s->buf.csf_f.band_d = (int16_t *)(top + 2u * half);
    top += 3u * half;
    s->buf.i4_ref_dwt2.band_a = (int32_t *)top;
    s->buf.i4_ref_dwt2.band_h = (int32_t *)(top + band_size);
    s->buf.i4_ref_dwt2.band_v = (int32_t *)(top + 2u * band_size);
    s->buf.i4_ref_dwt2.band_d = (int32_t *)(top + 3u * band_size);
    top += 4u * band_size;
    s->buf.i4_dis_dwt2.band_a = (int32_t *)top;
    s->buf.i4_dis_dwt2.band_h = (int32_t *)(top + band_size);
    s->buf.i4_dis_dwt2.band_v = (int32_t *)(top + 2u * band_size);
    s->buf.i4_dis_dwt2.band_d = (int32_t *)(top + 3u * band_size);
    top += 4u * band_size;
    s->buf.i4_csf_f.band_a = VMAF_NULLPTR;
    s->buf.i4_csf_f.band_h = (int32_t *)top;
    s->buf.i4_csf_f.band_v = (int32_t *)(top + band_size);
    s->buf.i4_csf_f.band_d = (int32_t *)(top + 2u * band_size);
}

static void adm_slice_results(AdmStateHip *s)
{
    const size_t cm_stride = 3u * sizeof(int64_t);
    const size_t csf_stride = 3u * sizeof(uint64_t);
    uint8_t *results = (uint8_t *)s->buf.tmp_res;
    for (int i = 0; i < 4; ++i)
        s->buf.adm_cm[i] = (int64_t *)(results + (size_t)i * cm_stride);
    results += 4u * cm_stride;
    for (int i = 0; i < 4; ++i)
        s->buf.adm_csf_den[i] = (uint64_t *)(results + (size_t)i * csf_stride);
}

static void adm_init_abort(AdmStateHip *s)
{
    adm_buffers_free(s);
    adm_modules_unload(s);
    adm_init_runtime_destroy(s);
}

static int adm_runtime_close(AdmStateHip *s)
{
    hipError_t hip_err = hipStreamSynchronize(s->str);
    int err = hip_rc(hip_err);
    hip_err = hipStreamDestroy(s->str);
    if (err == 0)
        err = hip_rc(hip_err);
    s->str = VMAF_NULLPTR;
    hip_err = hipEventDestroy(s->finished);
    if (err == 0)
        err = hip_rc(hip_err);
    s->finished = VMAF_NULLPTR;
    hip_err = hipEventDestroy(s->ref_event);
    if (err == 0)
        err = hip_rc(hip_err);
    s->ref_event = VMAF_NULLPTR;
    hip_err = hipEventDestroy(s->dis_event);
    if (err == 0)
        err = hip_rc(hip_err);
    s->dis_event = VMAF_NULLPTR;
    return err;
}
#endif /* HAVE_HIPCC */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)pix_fmt;
    (void)bpc;
    (void)w;
    (void)h;
    return -ENOSYS;
#else
    (void)pix_fmt;
    AdmStateHip *s = fex->priv;
    int err = adm_configuration_init(s);
    if (err == 0)
        err = adm_runtime_create(s);
    if (err == 0)
        err = adm_modules_load(s);
    if (err == 0)
        err = adm_dwt_functions_load(s);
    if (err == 0)
        err = adm_feature_functions_load(s);

    size_t band_size = 0;
    if (err == 0)
        err = adm_buffers_alloc(s, bpc, w, h, &band_size);

    if (err == 0)
        adm_slice_bands(s, band_size);

    if (err == 0)
        adm_slice_results(s);
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == VMAF_NULLPTR)
            err = -ENOMEM;
    }
    if (err != 0)
        adm_init_abort(s);
    return err;
#endif /* HAVE_HIPCC */
}

static int submit_fex_hip(VmafFeatureExtractor *fex, const VmafPicture *ref_pic, const VmafPicture *ref_pic_90,
                          const VmafPicture *dist_pic, const VmafPicture *dist_pic_90, unsigned index)
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
    write_scores(&params);
    return 0;
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
    rc = adm_runtime_close(s);
    adm_modules_unload(s);
    adm_buffers_free(s);
#endif /* HAVE_HIPCC */

    if (s->feature_name_dict != VMAF_NULLPTR) {
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
                                          VMAF_NULLPTR};

/*
 * Registration note: the extractor is declared as non-static so it can be
 * referenced by `extern VmafFeatureExtractor vmaf_fex_integer_adm_hip` in
 * feature_extractor.c's lookup table. Making it static would unlink it from
 * the registry. Same pattern as every other GPU feature extractor in this
 * tree (e.g. `vmaf_fex_integer_adm_cuda` in integer_adm_cuda.c).
 */
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
