/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_chroma feature extractor — HIP backend, device-resident (ADR-1384,
 *  the HIP port of ADR-1358; refines ADR-0567).
 *
 *  submit() copies the four chroma planes (U and V of reference and
 *  distorted) into pinned staging and enqueues their upload
 *  (vmaf_hip_picture_upload_staged()) and the whole SpEED chain of
 *  speed/speed_pipeline.hip for both U and V, and returns without waiting. collect() waits once and reads one SpeedGpuFrameResult: the U and
 *  V scores and the per-channel singularity flags. Only the U/V combination,
 *  the clamp and the collector append run on the host, on those scalars.
 *
 *  This TU holds no device kernel and no SpEED arithmetic
 *  (core/test/test_hip_kernel_source_contract.py).
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "picture.h"

#include "feature/speed_internal.h"
#include "hip/speed_chroma_hip.h"
#include "speed_hip_pipeline.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define SC_CHANNELS (4u) /* U ref, U dis, V ref, V dis */

#define SC_DEFAULT_SIGMA_NN (0.29)
#define SC_DEFAULT_MAX_VAL (1000.0)
#define SC_DEFAULT_NN_FLOOR (0.0)
#define SC_DEFAULT_KERNELSCALE (1.0)
#define SC_DEFAULT_PRESCALE (1.0)
#define SC_DEFAULT_PRESCALE_METHOD ("nearest")

/* ------------------------------------------------------------------ */
/* Private extractor state                                             */
/* ------------------------------------------------------------------ */

typedef struct SpeedChromaHipState {
    SpeedHipPipeline *pipeline;

    /* User options. */
    double speed_chroma_kernelscale;
    double speed_chroma_prescale;
    char *speed_chroma_prescale_method;
    double speed_chroma_sigma_nn;
    double speed_chroma_nn_floor;
    double speed_chroma_max_val;
    int speed_weight_var_mode;

    VmafDictionary *feature_name_dict;
    /* Singular covariance matrices are counted, not logged per solve. */
    SpeedInternalSingularTally singular_tally;
} SpeedChromaHipState;

static const VmafOption options_chroma[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedChromaHipState, speed_chroma_kernelscale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_KERNELSCALE,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ks",
    },
    {
        .name = "speed_prescale",
        .help = "scaling factor for the frame",
        .offset = offsetof(SpeedChromaHipState, speed_chroma_prescale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_PRESCALE,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ps",
    },
    {
        .name = "speed_prescale_method",
        .help = "scaling method [nearest, bilinear, bicubic, lanczos4]",
        .offset = offsetof(SpeedChromaHipState, speed_chroma_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = SC_DEFAULT_PRESCALE_METHOD,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedChromaHipState, speed_chroma_sigma_nn),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_SIGMA_NN,
        .min = 0.1,
        .max = 2.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "snn",
    },
    {
        .name = "speed_nn_floor",
        .help = "neural noise floor fraction",
        .offset = offsetof(SpeedChromaHipState, speed_chroma_nn_floor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_NN_FLOOR,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "nnf",
    },
    {
        .name = "speed_max_val",
        .help = "clip output to this maximum",
        .offset = offsetof(SpeedChromaHipState, speed_chroma_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mxv",
    },
    {
        .name = "speed_weight_var_mode",
        .help = "variance weighting mode (0-6)",
        .offset = offsetof(SpeedChromaHipState, speed_weight_var_mode),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.d = 0,
        .min = 0,
        .max = 6,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "wvm",
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC
/* The init-time part of speed_init() for the chroma planes; the device
 * pipeline is created from it. */
static int sc_configure(SpeedChromaHipState *s, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h, SpeedHipConfig *config)
{
    unsigned cw = 0u;
    unsigned ch = 0u;
    int err = speed_chroma_dimensions(w, h, pix_fmt, &cw, &ch);
    if (err)
        return err;
    const SpeedInternalOptions opt = {
        .speed_kernelscale = s->speed_chroma_kernelscale,
        .speed_prescale = s->speed_chroma_prescale,
        .speed_prescale_method = s->speed_chroma_prescale_method,
        .speed_sigma_nn = s->speed_chroma_sigma_nn,
        .speed_nn_floor = s->speed_chroma_nn_floor,
        .speed_weight_var_mode = s->speed_weight_var_mode,
    };
    SpeedInternalDimensions dim;
    err = speed_internal_init_dimensions(&dim, (int)cw, (int)ch, opt.speed_prescale);
    if (!err)
        err = speed_internal_gpu_configure(&dim, &opt, bpc, &config->shared);
    config->channels = SC_CHANNELS;
    config->raw_planes = SC_CHANNELS;
    config->staged = SC_CHANNELS;
    return err;
}
#endif /* HAVE_HIPCC */

static int init_chroma_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                           unsigned w, unsigned h)
{
#ifndef HAVE_HIPCC
    /* Scaffold posture: -ENOSYS and nothing else (ADR-1264). */
    (void)fex;
    (void)pix_fmt;
    (void)bpc;
    (void)w;
    (void)h;
    return -ENOSYS;
#else
    SpeedChromaHipState *s = fex->priv;
    SpeedHipConfig config;
    int err = sc_configure(s, pix_fmt, bpc, w, h, &config);
    if (err)
        return err;
    /* Channel c reads raw plane c, no difference. */
    SpeedHipBindingSets bindings;
    speed_hip_bindings_chroma(&bindings);
    err = speed_hip_pipeline_create(&s->pipeline, &config, &bindings);
    if (err)
        return err;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        speed_hip_pipeline_destroy(&s->pipeline);
        return -ENOMEM;
    }
    return 0;
#endif /* HAVE_HIPCC */
}

static int submit_chroma_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                             VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                             VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    SpeedChromaHipState *s = fex->priv;
    /* Raw-plane order matches the channel pairs: (U ref, U dis), (V ref, V dis). */
    const SpeedHipPlane planes[SC_CHANNELS] = {
        {ref_pic, 1u},
        {dist_pic, 1u},
        {ref_pic, 2u},
        {dist_pic, 2u},
    };
    int err = speed_hip_pipeline_upload(s->pipeline, 0u, planes, SC_CHANNELS);
    if (!err)
        err = speed_hip_pipeline_submit(s->pipeline, 0u);
    return err;
}

/* uv from u and v, imputing across a singular channel exactly as
 * extract_chroma() in speed.c does. */
static float combine_chroma_uv(float score_u, float score_v, bool singular_u, bool singular_v)
{
    if (singular_u && !singular_v)
        return score_v;
    if (singular_v && !singular_u)
        return score_u;
    return (score_u + score_v) * 0.5f;
}

static void sc_tally_frame(SpeedChromaHipState *s, const SpeedGpuFrameResult *result,
                           unsigned index)
{
    for (uint32_t ch = 0u; ch < SC_CHANNELS; ch++) {
        speed_internal_tally_solve(&s->singular_tally, result->singular[ch] != 0,
                                   "speed_chroma_hip");
        if (result->iteration_cap[ch] != 0)
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "speed_chroma_hip: eigenvalue QR iteration reached cap at frame %u, "
                     "possible non-convergence\n",
                     index);
    }
}

/* Emits the three chroma features with the configured max-value ceiling.
 * speed_internal_clamp_score() refuses a non-finite score instead of clamping
 * it to speed_max_val, matching the CPU reference. */
static int sc_emit_scores(const SpeedChromaHipState *s, VmafFeatureCollector *feature_collector,
                          unsigned index, const SpeedGpuFrameResult *result)
{
    const bool singular_u = result->singular[0] != 0 || result->singular[1] != 0;
    const bool singular_v = result->singular[2] != 0 || result->singular[3] != 0;
    const float score_uv =
        combine_chroma_uv(result->score[0], result->score[1], singular_u, singular_v);
    const double mxv = s->speed_chroma_max_val;
    double clamped_u = 0.0;
    double clamped_v = 0.0;
    double clamped_uv = 0.0;
    int err = speed_internal_clamp_score(result->score[0], mxv, index, "speed_chroma_hip",
                                         "speed_chroma_u", &clamped_u);
    if (!err)
        err = speed_internal_clamp_score(result->score[1], mxv, index, "speed_chroma_hip",
                                         "speed_chroma_v", &clamped_v);
    if (!err)
        err = speed_internal_clamp_score(score_uv, mxv, index, "speed_chroma_hip",
                                         "speed_chroma_uv", &clamped_uv);
    if (err)
        return err;
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_u_score",
                                                   clamped_u, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_v_score",
                                                   clamped_v, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_uv_score",
                                                   clamped_uv, index);
    return err;
}

static int collect_chroma_hip(VmafFeatureExtractor *fex, unsigned index,
                              VmafFeatureCollector *feature_collector)
{
    SpeedChromaHipState *s = fex->priv;
    SpeedGpuFrameResult result;
    const int err = speed_hip_pipeline_collect(s->pipeline, &result);
    if (err)
        return err;
    sc_tally_frame(s, &result, index);
    return sc_emit_scores(s, feature_collector, index, &result);
}

static int close_chroma_hip(VmafFeatureExtractor *fex)
{
    SpeedChromaHipState *s = fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_chroma_hip");
    speed_hip_pipeline_destroy(&s->pipeline);
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_chroma[] = {
    "Speed_chroma_feature_speed_chroma_u_score",
    "Speed_chroma_feature_speed_chroma_v_score",
    "Speed_chroma_feature_speed_chroma_uv_score",
    NULL,
};

/* ADR-1384: device-resident HIP twin of speed_chroma. */
VmafFeatureExtractor vmaf_fex_speed_chroma_hip = {
    .name = "speed_chroma_hip",
    .init = init_chroma_hip,
    .submit = submit_chroma_hip,
    .collect = collect_chroma_hip,
    .close = close_chroma_hip,
    .options = options_chroma,
    .priv_size = sizeof(SpeedChromaHipState),
    .provided_features = provided_features_chroma,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
};

/* NOLINTEND(modernize-use-nullptr) */
