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

#include "hip/integer_adm_hip.h"

#ifdef HAVE_HIPCC
/* The HIP headers need the platform macro, which is reserved to the
 * implementation. The build supplies it: hip_runtime_dep in
 * core/src/hip/meson.build compiles every HIP translation unit with
 * -D__HIP_PLATFORM_AMD__=1, as core/src/hip/picture_hip.c relies on. */
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

/* 4 scales x 3 bands, CM accumulators then CSF-denominator accumulators. */
#define RES_BUFFER_SIZE ((size_t)4 * 3 * 2)

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

    /* ADR-0759: device copy of `buf`. The two CSF and the two CM compute
     * kernels take `const AdmBufferHip *` and read their band pointers from
     * here instead of receiving the whole struct by value on every launch.
     * Uploaded once by adm_hip_upload_buf(); `buf` does not change after
     * that, so the copy stays equal to it until close(). */
    AdmBufferHip *buf_dev;

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

/* CSF weight per scale and band (rfactor) and its fixed-point form
 * (i_rfactor): adm_csf_rfactor_scale0() for scale 0, rfactor * 2^32 for
 * scales 1-3. */
static void adm_hip_rfactors(double adm_norm_view_dist, int adm_ref_display_height,
                             int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                             float rfactor[12], uint32_t i_rfactor[12])
{
    const double pow2_32 = pow(2, 32);
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t band0 = (size_t)scale * 3u;
        const AdmCsfFactors f =
            adm_csf_factors((int)scale, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                            adm_csf_scale, adm_csf_diag_scale);
        rfactor[band0] = f.factor1;
        rfactor[band0 + 1] = f.factor1;
        rfactor[band0 + 2] = f.factor2;
        if (scale == 0) {
            uint16_t i_rf[3];
            adm_csf_rfactor_scale0(rfactor, adm_norm_view_dist, adm_ref_display_height,
                                   adm_csf_mode, i_rf);
            i_rfactor[0] = i_rf[0];
            i_rfactor[1] = i_rf[1];
            i_rfactor[2] = i_rf[2];
        } else {
            i_rfactor[band0] = (uint32_t)(rfactor[band0] * pow2_32);
            i_rfactor[band0 + 1] = (uint32_t)(rfactor[band0 + 1] * pow2_32);
            i_rfactor[band0 + 2] = (uint32_t)(rfactor[band0 + 2] * pow2_32);
        }
    }
}

/* The score helpers below only run behind the device pipeline; the
 * scaffold build (no HAVE_HIPCC) has nothing to conclude. */
#ifdef HAVE_HIPCC

/* ------------------------------------------------------------------ */
/* Score computation helpers (host-side, same as CUDA twin)            */
/* ------------------------------------------------------------------ */

static void conclude_adm_cm(const int64_t *accum, int h, int w, int scale, float noise_weight,
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

static void conclude_adm_csf_den(const uint64_t *accum, int h, int w, int scale, float *result,
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

typedef struct AdmHipNamedScore {
    const char *name;
    double value;
} AdmHipNamedScore;

/* Per-scale numerator and denominator into scores[2 * scale] and
 * scores[2 * scale + 1], and their sums over the scales that count. */
static void adm_hip_scale_scores(const AdmStateHip *s, unsigned w, unsigned h, double scores[8],
                                 double *num, double *den)
{
    const int64_t *adm_cm = (const int64_t *)s->buf.results_host;
    const uint64_t *adm_csf = &((const uint64_t *)s->buf.results_host)[RES_BUFFER_SIZE / 2];
    float num_scale;
    float den_scale;

    *num = 0;
    *den = 0;
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t band0 = (size_t)scale * 3u;
        w = (w + 1) / 2;
        h = (h + 1) / 2;

        conclude_adm_cm(&adm_cm[band0], (int)h, (int)w, (int)scale, (float)s->adm_noise_weight,
                        s->adm_p_norm, &num_scale);
        conclude_adm_csf_den(&adm_csf[band0], (int)h, (int)w, (int)scale, &den_scale,
                             &s->rfactor[band0], (float)s->adm_noise_weight);

        /* adm_skip_scale0: exclude scale 0 from num/den accumulation, mirroring
         * the CPU integer_adm.c fast-path (den_scale = 1e-10, num_scale = 0).
         * The GPU kernel still computes scale 0; suppression is host-side only. */
        if (scale == 0u && s->adm_skip_scale0) {
            scores[0] = 0.0;
            scores[1] = 1e-10;
            continue;
        }

        *num += num_scale;
        *den += den_scale;

        scores[2 * scale + 0] = num_scale;
        scores[2 * scale + 1] = den_scale;
    }
}

/* Append each score in order; returns the OR of the collector statuses. */
static int adm_hip_append_scores(VmafFeatureCollector *feature_collector, VmafDictionary *dict,
                                 const AdmHipNamedScore *list, size_t count, unsigned index)
{
    int err = 0;
    for (size_t i = 0; i < count; ++i) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, dict, list[i].name,
                                                       list[i].value, index);
    }
    return err;
}

static void write_scores(const write_score_parameters_adm_hip *params)
{
    const AdmStateHip *s = params->s;
    double scores[8];
    double num;
    double den;

    adm_hip_scale_scores(s, params->w, params->h, scores, &num, &den);

    /* CPU parity (integer_adm.c::integer_compute_adm): the precision floor
     * scales with the FULL-FRAME area, not the scale-3 area the per-scale
     * loop ends on. */
    const double numden_limit = 1e-10 * ((double)params->w * params->h) / (1920.0 * 1080.0);
    num = num < numden_limit ? 0 : num;
    den = den < numden_limit ? 0 : den;

    /* ADR-0487 clamps adm3 only: the CPU reference emits
     * VMAF_integer_feature_adm2_score unclamped (integer_adm.c::extract()
     * applies MAX(..., adm_min_val) to the adm3 expression alone). */
    const double score = (den == 0.0) ? 1.0 : num / den;

    /* AIM / adm3 are NOT emitted by this twin: the AIM contrast measure needs
     * a second device CM pass with the decouple_a / decouple_r roles swapped
     * (the CUDA twin's ADR-0746 kernels), which the HIP kernel set does not
     * have. Leaving both features out of `provided_features` routes them to
     * the CPU twin through the ADR-0530 name-based fallback, which produces
     * the correct value under the correct feature-name key. Emitting them
     * here from a hard-coded aim_num would fabricate a score. Tracked as
     * T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05 in docs/state.md. */

    const AdmHipNamedScore main_scores[] = {
        {"VMAF_integer_feature_adm2_score", score},
        {"integer_adm_scale0", scores[0] / scores[1]},
        {"integer_adm_scale1", scores[2] / scores[3]},
        {"integer_adm_scale2", scores[4] / scores[5]},
        {"integer_adm_scale3", scores[6] / scores[7]},
    };
    int err = adm_hip_append_scores(params->feature_collector, s->feature_name_dict, main_scores,
                                    sizeof(main_scores) / sizeof(main_scores[0]), params->index);

    if (s->debug) {
        const AdmHipNamedScore debug_scores[] = {
            {"integer_adm", score},
            {"integer_adm_num", num},
            {"integer_adm_den", den},
            {"integer_adm_num_scale0", scores[0]},
            {"integer_adm_den_scale0", scores[1]},
            {"integer_adm_num_scale1", scores[2]},
            {"integer_adm_den_scale1", scores[3]},
            {"integer_adm_num_scale2", scores[4]},
            {"integer_adm_den_scale2", scores[5]},
            {"integer_adm_num_scale3", scores[6]},
            {"integer_adm_den_scale3", scores[7]},
        };
        err |= adm_hip_append_scores(params->feature_collector, s->feature_name_dict, debug_scores,
                                     sizeof(debug_scores) / sizeof(debug_scores[0]), params->index);
    }
    (void)err; /* accumulated collector status intentionally discarded; void writer API */
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
        /* FEATURE_PARAM: honoured for feature-name-key parity only. dlm_weight
         * enters the arithmetic of VMAF_integer_feature_adm3_score, which this
         * twin does not emit (see write_scores). Dropping it from the table
         * would make this twin emit `integer_adm2_...` where the CPU twin emits
         * `integer_adm2_dlmw_<v>_...` for the same opts dict, and the model
         * lookup would miss. Same posture as the CPU reference, where
         * adm_dlm_weight likewise has no arithmetic effect on adm2. */
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
    const int rows_per_thread = 4;
    const int vert_out_tile_rows = 8;
    const int vert_out_tile_cols = 128;
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
                    (void *)&buf->adm_csf_den[scale]};
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_csf_den_s123_line_kernel,
        (uint32_t)DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread), (uint32_t)buffer_h, 3,
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
                    &left,          &right, &src_stride, (void *)&buf->adm_csf_den[scale]};
    hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_csf_den_scale_line_kernel,
        (uint32_t)DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread), (uint32_t)buffer_h, 3,
        (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, NULL);
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
        void *args[] = {(void *)&s->buf_dev,
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
                        &scale,
                        &buffer_h,
                        &buffer_stride,
                        (void *)&buf->tmp_accum,
                        p};
        hipError_t rc = hipModuleLaunchKernel(
            s->func_i4_adm_cm_line_kernel, (uint32_t)DIV_ROUND_UP(buffer_stride, BLOCKX),
            (uint32_t)buffer_h, 3, (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, NULL);
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
                        &buffer_h,
                        &buffer_stride,
                        (void *)&buf->tmp_accum,
                        (void *)&buf->adm_cm[scale]};
        hipError_t rc =
            hipModuleLaunchKernel(s->func_adm_cm_reduce_line_kernel_4, 1, (uint32_t)buffer_h, 3,
                                  (uint32_t)BLOCKX, 1, 1, 0, c_stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc(rc);
    }
    return 0;
}

/* Scale-0 cube and square shifts per band for a band `w` samples wide. */
static void adm_cm_warp_shift(int w, WarpShiftHip *ws)
{
    const int fixed_shift[3] = {4, 4, 3};
    const int32_t shift_xsq[3] = {29, 29, 30};
    const int32_t add_shift_xsq[3] = {268435456, 268435456, 536870912};

    for (int band = 0; band < 3; ++band) {
        ws->shift_cub[band] = (uint32_t)ceilf(log2f((float)w));
        ws->shift_cub[band] -= (uint32_t)fixed_shift[band];
        ws->shift_sq[band] = (uint32_t)shift_xsq[band];
        ws->add_shift_sq[band] = (uint32_t)add_shift_xsq[band];
        ws->add_shift_cub[band] = adm_half_shift(ws->shift_cub[band]);
    }
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

    WarpShiftHip ws;
    adm_cm_warp_shift(w, &ws);

    uint32_t shift_inner_accum = (uint32_t)ceilf(log2f((float)h));
    uint32_t add_shift_inner_accum = adm_half_shift(shift_inner_accum);

    /* fused CM + reduce kernel */
    const int rows_per_thread = 8;
    const int BLOCKX = 32;
    const int BLOCKY = 4;
    void *args[] = {(void *)&s->buf_dev,
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
                    (void *)&buf->tmp_accum,
                    p,
                    &scale,
                    (void *)&buf->adm_cm[scale],
                    &ws,
                    &shift_inner_accum,
                    &add_shift_inner_accum};
    const hipError_t rc = hipModuleLaunchKernel(
        s->func_adm_cm_line_kernel_8, 1, (uint32_t)DIV_ROUND_UP(buffer_h, BLOCKY * rows_per_thread),
        3, (uint32_t)BLOCKX, (uint32_t)BLOCKY, 1, 0, c_stream, args, NULL);
    return hip_rc(rc);
}

/* ------------------------------------------------------------------ */
/* Main per-frame computation                                           */
/* ------------------------------------------------------------------ */

/* Fixed-point parameters every ADM kernel reads, for a w x h luma plane. */
static void adm_hip_fixed_params(const AdmStateHip *s, int w, int h, double adm_enhn_gain_limit,
                                 double adm_norm_view_dist, int adm_ref_display_height,
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
    p->adm_ref_display_height = adm_ref_display_height;
    p->adm_norm_view_dist = adm_norm_view_dist;
    p->adm_enhn_gain_limit = adm_enhn_gain_limit;

    adm_hip_rfactors(adm_norm_view_dist, adm_ref_display_height, s->adm_csf_mode, s->adm_csf_scale,
                     s->adm_csf_diag_scale, p->rfactor, p->i_rfactor);
}

/* ADR-1211: stage the host-resident luma planes onto the device.
 * `VmafPicture::data[]` is HOST memory under the host-pic HIP backend
 * (ADR-0530), so handing it straight to the DWT2 kernel faults the GPU
 * ("Memory access fault ... Page not present"). Copy it across first and
 * point the kernel at the device buffer. Rows are tightly packed on the
 * device side, so the element stride the kernels see is `w`, not the
 * picture's. */
static int adm_hip_stage_luma(AdmStateHip *s, const VmafPicture *ref_pic,
                              const VmafPicture *dis_pic, int w, int h)
{
    const size_t bpp = (ref_pic->bpc > 8) ? sizeof(uint16_t) : sizeof(uint8_t);
    const size_t row_bytes = (size_t)w * bpp;
    hipError_t crc =
        hipMemcpy2DAsync(s->d_ref_luma, s->luma_pitch, ref_pic->data[0], (size_t)ref_pic->stride[0],
                         row_bytes, (size_t)h, hipMemcpyHostToDevice, s->str);
    if (crc != hipSuccess)
        return hip_rc(crc);
    crc =
        hipMemcpy2DAsync(s->d_dis_luma, s->luma_pitch, dis_pic->data[0], (size_t)dis_pic->stride[0],
                         row_bytes, (size_t)h, hipMemcpyHostToDevice, s->str);
    if (crc != hipSuccess)
        return hip_rc(crc);
    crc = hipStreamSynchronize(s->str);
    return hip_rc(crc);
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

/* Scale 0: int16 DWT, then CSF denominator, CSF and CM. `*w` / `*h` enter as
 * the luma dimensions and leave as the scale-0 band dimensions. */
static int adm_hip_scale0(AdmStateHip *s, AdmBufferHip *buf, const VmafPicture *ref_pic,
                          const VmafPicture *dis_pic, int *w, int *h, int buf_stride,
                          AdmFixedParametersHip *p)
{
    int err = adm_hip_dwt2_scale0(s, buf, ref_pic, dis_pic, *w, *h, buf_stride, p);
    if (err)
        return err;
    err = adm_hip_join_pic_stream(s);
    if (err)
        return err;

    *w = (*w + 1) / 2;
    *h = (*h + 1) / 2;

    err = adm_csf_den_scale_device_hip(s, buf, *w, *h, buf_stride, s->str);
    if (err)
        return err;
    err = adm_csf_device_hip(s, buf, *w, *h, buf_stride, p, s->str);
    if (err)
        return err;
    return adm_cm_device_hip(s, buf, *w, *h, buf_stride, buf_stride, p, s->str);
}

/* Scales 1-3: int32 DWT of the previous scale's approximation band, then CSF
 * denominator, CSF and CM. `*w` / `*h` are halved like adm_hip_scale0(). */
static int adm_hip_scale123(AdmStateHip *s, AdmBufferHip *buf, int scale, int *w, int *h,
                            int buf_stride, AdmFixedParametersHip *p)
{
    int err = adm_dwt2_s123_combined_device_hip(s, buf->i4_ref_dwt2.band_a, (int32_t *)buf->tmp_ref,
                                                buf->i4_ref_dwt2, *w, *h, buf_stride, buf_stride,
                                                scale, p, s->str);
    if (err)
        return err;
    err = adm_dwt2_s123_combined_device_hip(s, buf->i4_dis_dwt2.band_a, (int32_t *)buf->tmp_dis,
                                            buf->i4_dis_dwt2, *w, *h, buf_stride, buf_stride, scale,
                                            p, s->str);
    if (err)
        return err;

    *w = (*w + 1) / 2;
    *h = (*h + 1) / 2;

    err = adm_csf_den_s123_device_hip(s, buf, scale, *w, *h, buf_stride, s->str);
    if (err)
        return err;
    err = i4_adm_csf_device_hip(s, buf, scale, *w, *h, buf_stride, p, s->str);
    if (err)
        return err;
    return i4_adm_cm_device_hip(s, buf, *w, *h, buf_stride, buf_stride, scale, p, s->str);
}

static int integer_compute_adm_hip(AdmStateHip *s, VmafPicture *ref_pic, VmafPicture *dis_pic,
                                   AdmBufferHip *buf, double adm_enhn_gain_limit,
                                   double adm_norm_view_dist, int adm_ref_display_height)
{
    int w = (int)ref_pic->w[0];
    int h = (int)ref_pic->h[0];

    AdmFixedParametersHip p;
    adm_hip_fixed_params(s, w, h, adm_enhn_gain_limit, adm_norm_view_dist, adm_ref_display_height,
                         &p);
    memcpy(s->rfactor, p.rfactor, sizeof(p.rfactor));

    /* Zero result accumulator */
    hipError_t hip_err = hipMemsetAsync(buf->tmp_res, 0, sizeof(int64_t) * RES_BUFFER_SIZE, s->str);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);

    int err = adm_hip_stage_luma(s, ref_pic, dis_pic, w, h);
    if (err)
        return err;

    const int buf_stride = (int)(buf->ind_size_x >> 2); /* bytes → int32 elements */
    err = adm_hip_scale0(s, buf, ref_pic, dis_pic, &w, &h, buf_stride, &p);
    for (int scale = 1; scale < 4 && err == 0; ++scale) {
        err = adm_hip_scale123(s, buf, scale, &w, &h, buf_stride, &p);
    }
    if (err)
        return err;

    hip_err = hipMemcpyAsync(buf->results_host, buf->tmp_res, sizeof(int64_t) * RES_BUFFER_SIZE,
                             hipMemcpyDeviceToHost, s->str);
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

        {s->adm_csf_den_module, &s->func_adm_csf_den_scale_line_kernel,
         "adm_csf_den_scale_line_kernel_8_128"},
        {s->adm_csf_den_module, &s->func_adm_csf_den_s123_line_kernel,
         "adm_csf_den_s123_line_kernel_8_128"},

        {s->adm_cm_module, &s->func_adm_cm_reduce_line_kernel_4, "adm_cm_reduce_line_kernel_4"},
        {s->adm_cm_module, &s->func_adm_cm_line_kernel_8, "adm_cm_line_kernel_8"},
        {s->adm_cm_module, &s->func_i4_adm_cm_line_kernel, "i4_adm_cm_line_kernel"},
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
        buf_sz_one * 11 + buf_sz_one / 2 * 11,         /* data_buf */
        s->integer_stride * 4 * ((h + 1) / 2),         /* tmp_ref */
        s->integer_stride * 4 * ((h + 1) / 2),         /* tmp_dis */
        sizeof(uint64_t) * 3u * (size_t)w * (size_t)h, /* tmp_accum */
        sizeof(uint64_t) * 3u * (size_t)h,             /* tmp_accum_h */
        sizeof(uint64_t) * RES_BUFFER_SIZE,            /* tmp_res */
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
        hip_err = hipHostMalloc(&s->buf.results_host, sizeof(uint64_t) * RES_BUFFER_SIZE,
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

/* ADR-1211: staging buffers for the host-resident luma plane. Sized for the
 * full frame at this bit depth; the staged rows are tightly packed, so the
 * element stride handed to the kernel is `w`, not the picture's stride. On
 * failure, frees what it allocated. */
static int adm_hip_alloc_luma(AdmStateHip *s, unsigned w, unsigned h, unsigned bpc)
{
    s->luma_pitch = (size_t)w * ((bpc > 8u) ? sizeof(uint16_t) : sizeof(uint8_t));
    s->luma_h = h;
    hipError_t hip_err = hipMalloc(&s->d_ref_luma, s->luma_pitch * (size_t)h);
    if (hip_err != hipSuccess)
        return hip_rc(hip_err);
    hip_err = hipMalloc(&s->d_dis_luma, s->luma_pitch * (size_t)h);
    if (hip_err != hipSuccess) {
        (void)hipFree(s->d_ref_luma);
        s->d_ref_luma = NULL;
    }
    return hip_rc(hip_err);
}

static void adm_hip_free_luma(AdmStateHip *s)
{
    if (s->d_ref_luma != NULL) {
        (void)hipFree(s->d_ref_luma);
        s->d_ref_luma = NULL;
    }
    if (s->d_dis_luma != NULL) {
        (void)hipFree(s->d_dis_luma);
        s->d_dis_luma = NULL;
    }
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

/* Slice result accumulator */
static void adm_hip_slice_results(AdmStateHip *s)
{
    const size_t cm_stride = 3u * sizeof(int64_t);
    const size_t csf_stride = 3u * sizeof(uint64_t);
    uint8_t *res = (uint8_t *)s->buf.tmp_res;
    for (int i = 0; i < 4; ++i) {
        s->buf.adm_cm[i] = (int64_t *)(res + (size_t)i * cm_stride);
    }
    res += 4u * cm_stride;
    for (int i = 0; i < 4; ++i) {
        s->buf.adm_csf_den[i] = (uint64_t *)(res + (size_t)i * csf_stride);
    }
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
static int adm_hip_init_device(AdmStateHip *s, unsigned w, unsigned h, unsigned bpc)
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
    if (err == 0) {
        err = adm_hip_alloc_luma(s, w, h, bpc);
        if (err)
            adm_hip_free_buffers(s);
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

    if (s->adm_norm_view_dist * s->adm_ref_display_height <
        DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT) {
        return -EINVAL;
    }

    /* ADR-1191: reject CSF configurations the fixed-point pipeline cannot
     * represent before any device resource is claimed, so an unsupported
     * adm_csf_mode / viewing geometry fails loudly instead of wrapping.
     * Same accept/reject set as the CPU reference. */
    return adm_csf_config_check(s);
}

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

    adm_hip_rfactors(s->adm_norm_view_dist, s->adm_ref_display_height, s->adm_csf_mode,
                     s->adm_csf_scale, s->adm_csf_diag_scale, s->rfactor, s->i_rfactor);

#ifndef HAVE_HIPCC
    (void)w;
    (void)h;
    /* Scaffold: no runtime available. */
    return -ENOSYS;
#else
    const int dev_err = adm_hip_init_device(s, w, h, bpc);
    if (dev_err) {
        return dev_err;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        /* The framework never calls close() after a failed init(), so every
         * device resource is released here. */
        adm_hip_free_buf_dev(s);
        adm_hip_free_luma(s);
        adm_hip_free_buffers(s);
        adm_hip_unload_modules(s);
        adm_hip_destroy_stream(s);
        return -ENOMEM;
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
    return integer_compute_adm_hip(s, ref_pic, dist_pic, &s->buf, s->adm_enhn_gain_limit,
                                   s->adm_norm_view_dist, s->adm_ref_display_height);
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
