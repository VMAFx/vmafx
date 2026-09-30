/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_temporal feature extractor — HIP backend, device-resident
 *  (ADR-1384, the HIP port of ADR-1358; refines ADR-0567).
 *
 *  Temporal design: the raw luma planes of the last two frames stay on the
 *  device in two slots (reference and distorted per slot). submit() uploads
 *  the current frame into slot `index % 2` and, from the second frame on,
 *  enqueues the SpEED chain of speed/speed_pipeline.hip on the temporal
 *  difference `previous - current` (speed.c extract(): subtract_image()),
 *  never waiting. collect() waits once and reads the frame score. Frame 0
 *  emits 0, as the CPU reference does.
 *
 *  This TU holds no device kernel and no SpEED arithmetic
 *  (core/test/test_hip_kernel_source_contract.py).
 *
 *  Output feature: Speed_temporal_feature_speed_temporal_score.
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
#include "hip/speed_temporal_hip.h"
#include "speed_hip_pipeline.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define ST_CHANNELS (2u) /* reference, distorted */
#define ST_SLOTS (2u)    /* current and previous frame */

#define ST_DEFAULT_SIGMA_NN (0.29)
#define ST_DEFAULT_MAX_VAL (1000.0)
#define ST_DEFAULT_NN_FLOOR (0.0)
#define ST_DEFAULT_KERNELSCALE (1.0)
#define ST_DEFAULT_PRESCALE (1.0)
#define ST_DEFAULT_PRESCALE_METHOD ("nearest")

/* ------------------------------------------------------------------ */
/* Private extractor state                                             */
/* ------------------------------------------------------------------ */

typedef struct SpeedTemporalHipState {
    SpeedHipPipeline *pipeline;

    double speed_temporal_kernelscale;
    double speed_temporal_prescale;
    char *speed_temporal_prescale_method;
    double speed_temporal_sigma_nn;
    double speed_temporal_nn_floor;
    double speed_temporal_max_val;
    bool speed_temporal_use_ref_diff;

    VmafDictionary *feature_name_dict;
    /* Singular covariance matrices are counted, not logged per solve. */
    SpeedInternalSingularTally singular_tally;
} SpeedTemporalHipState;

static const VmafOption options_temporal[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_kernelscale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_KERNELSCALE,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ks",
    },
    {
        .name = "speed_prescale",
        .help = "scaling factor for the frame",
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_prescale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_PRESCALE,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ps",
    },
    {
        .name = "speed_prescale_method",
        .help = "scaling method [nearest, bilinear, bicubic, lanczos4]",
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = ST_DEFAULT_PRESCALE_METHOD,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_sigma_nn),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_SIGMA_NN,
        .min = 0.1,
        .max = 2.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "snn",
    },
    {
        .name = "speed_nn_floor",
        .help = "neural noise floor fraction",
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_nn_floor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_NN_FLOOR,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "nnf",
    },
    {
        .name = "speed_max_val",
        .help = "clip output to this maximum",
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mxv",
    },
    {
        .name = "speed_use_ref_diff",
        .help = "use reference frame difference instead of distorted",
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_use_ref_diff),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "urd",
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC
/* The init-time part of speed_init(); speed_temporal always weights with
 * mode 0 (speed.c init()). */
static int st_configure(const SpeedTemporalHipState *s, unsigned bpc, unsigned w, unsigned h,
                        SpeedHipConfig *config)
{
    const SpeedInternalOptions opt = {
        .speed_kernelscale = s->speed_temporal_kernelscale,
        .speed_prescale = s->speed_temporal_prescale,
        .speed_prescale_method = s->speed_temporal_prescale_method,
        .speed_sigma_nn = s->speed_temporal_sigma_nn,
        .speed_nn_floor = s->speed_temporal_nn_floor,
        .speed_weight_var_mode = 0,
    };
    SpeedInternalDimensions dim;
    int err = speed_internal_init_dimensions(&dim, (int)w, (int)h, opt.speed_prescale);
    if (!err)
        err = speed_internal_gpu_configure(&dim, &opt, bpc, &config->shared);
    config->channels = ST_CHANNELS;
    config->raw_planes = ST_CHANNELS * ST_SLOTS;
    config->staged = ST_CHANNELS;
    return err;
}
#endif /* HAVE_HIPCC */

static int init_temporal_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
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
    (void)pix_fmt;
    SpeedTemporalHipState *s = fex->priv;
    SpeedHipConfig config;
    int err = st_configure(s, bpc, w, h, &config);
    if (err)
        return err;
    SpeedHipBindingSets bindings;
    speed_hip_bindings_temporal(s->speed_temporal_use_ref_diff ? 1 : 0, &bindings);
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

static int submit_temporal_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                               VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                               VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    SpeedTemporalHipState *s = fex->priv;
    const SpeedHipPlane planes[ST_CHANNELS] = {{ref_pic, 0u}, {dist_pic, 0u}};
    const uint32_t set = index % ST_SLOTS;
    const int err = speed_hip_pipeline_upload(s->pipeline, ST_CHANNELS * set, planes, ST_CHANNELS);
    if (err || index == 0u)
        return err;
    return speed_hip_pipeline_submit(s->pipeline, set);
}

static void st_tally_frame(SpeedTemporalHipState *s, const SpeedGpuFrameResult *result,
                           unsigned index)
{
    for (uint32_t ch = 0u; ch < ST_CHANNELS; ch++) {
        speed_internal_tally_solve(&s->singular_tally, result->singular[ch] != 0,
                                   "speed_temporal_hip");
        if (result->iteration_cap[ch] != 0)
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "speed_temporal_hip: eigenvalue QR iteration reached cap at frame %u, "
                     "possible non-convergence\n",
                     index);
    }
}

static int collect_temporal_hip(VmafFeatureExtractor *fex, unsigned index,
                                VmafFeatureCollector *feature_collector)
{
    SpeedTemporalHipState *s = fex->priv;
    if (index == 0u) {
        /* The upload must land before submit() reuses the staging planes. */
        const int err = speed_hip_pipeline_wait(s->pipeline);
        if (err)
            return err;
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }
    SpeedGpuFrameResult result;
    int err = speed_hip_pipeline_collect(s->pipeline, &result);
    if (err)
        return err;
    st_tally_frame(s, &result, index);
    /* speed_internal_clamp_score() refuses a non-finite score instead of
     * clamping it to speed_max_val, matching the CPU reference. */
    double clipped = 0.0;
    err = speed_internal_clamp_score(result.score[0], s->speed_temporal_max_val, index,
                                     "speed_temporal_hip", "speed_temporal", &clipped);
    if (err)
        return err;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_temporal_feature_speed_temporal_score",
                                                   clipped, index);
}

static int close_temporal_hip(VmafFeatureExtractor *fex)
{
    SpeedTemporalHipState *s = fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_temporal_hip");
    speed_hip_pipeline_destroy(&s->pipeline);
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_temporal[] = {
    "Speed_temporal_feature_speed_temporal_score",
    NULL,
};

/* ADR-1384: device-resident HIP twin of speed_temporal. TEMPORAL guarantees
 * in-order frames: the previous frame's planes stay on the device between
 * submits. */
VmafFeatureExtractor vmaf_fex_speed_temporal_hip = {
    .name = "speed_temporal_hip",
    .init = init_temporal_hip,
    .submit = submit_temporal_hip,
    .collect = collect_temporal_hip,
    .close = close_temporal_hip,
    .options = options_temporal,
    .priv_size = sizeof(SpeedTemporalHipState),
    .provided_features = provided_features_temporal,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_HIP,
};

/* NOLINTEND(modernize-use-nullptr) */
