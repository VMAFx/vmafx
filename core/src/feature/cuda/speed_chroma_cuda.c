/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_chroma feature extractor — CUDA backend, device-resident
 *  (ADR-1380, the CUDA port of ADR-1358; refines ADR-0567).
 *
 *  submit() copies the four chroma planes (U and V of reference and
 *  distorted) the engine already uploaded into the pipeline's raw-plane
 *  slots, device to device, and enqueues the whole SpEED chain of
 *  speed_cuda_pipeline.c for both U and V on the reference picture's stream.
 *  It never waits. collect() waits once and reads one SpeedGpuFrameResult:
 *  the U and V scores and the per-channel singularity flags. Only the U/V
 *  combination, the clamp and the collector append run on the host, on
 *  those scalars.
 *
 *  Output features: Speed_chroma_feature_speed_chroma_{u,v,uv}_score.
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
#include "picture_cuda.h"

#include "cuda_helper.cuh"

#include "feature/speed_internal.h"
#include "cuda/speed_chroma_cuda.h"
#include "cuda/speed_cuda_pipeline.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define SC_CHANNELS 4u /* U ref, U dis, V ref, V dis */

#define SC_DEFAULT_SIGMA_NN (0.29)
#define SC_DEFAULT_MAX_VAL (1000.0)
#define SC_DEFAULT_NN_FLOOR (0.0)
#define SC_DEFAULT_KERNELSCALE (1.0)
#define SC_DEFAULT_PRESCALE (1.0)
#define SC_DEFAULT_PRESCALE_METHOD ("nearest")

typedef struct SpeedChromaCudaState {
    SpeedCudaPipeline *pipeline;
    SpeedInternalSingularTally singular_tally;

    double speed_chroma_kernelscale;
    double speed_chroma_prescale;
    char *speed_chroma_prescale_method;
    double speed_chroma_sigma_nn;
    double speed_chroma_nn_floor;
    double speed_chroma_max_val;
    int speed_weight_var_mode;

    VmafDictionary *feature_name_dict;
} SpeedChromaCudaState;

static const VmafOption options[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_kernelscale),
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
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_prescale),
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
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = SC_DEFAULT_PRESCALE_METHOD,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_sigma_nn),
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
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_nn_floor),
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
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_max_val),
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
        .offset = offsetof(SpeedChromaCudaState, speed_weight_var_mode),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.d = 0,
        .min = 0,
        .max = 6,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "wvm",
    },
    {0},
};

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    SpeedChromaCudaState *s = fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_chroma_cuda");
    int rc = speed_cuda_pipeline_close(&s->pipeline);
    if (s->feature_name_dict) {
        const int err = vmaf_dictionary_free(&s->feature_name_dict);
        rc = rc ? rc : err;
    }
    return rc;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    SpeedChromaCudaState *s = fex->priv;
    unsigned chroma_w = 0u;
    unsigned chroma_h = 0u;
    int err = speed_chroma_dimensions(w, h, pix_fmt, &chroma_w, &chroma_h);
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
    /* On failure the partial pipeline stays in s->pipeline; the engine owes
     * this extractor a close after a failed init (ADR-1336). */
    err = speed_cuda_pipeline_open(&s->pipeline, fex->cu_state, &opt, chroma_w, chroma_h, bpc,
                                   SC_CHANNELS, SC_CHANNELS);
    if (err)
        return err;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    return s->feature_name_dict ? 0 : -ENOMEM;
}

/* Stage (U ref, U dis, V ref, V dis) and enqueue the chain, both on the
 * reference picture's stream after the distorted upload. The caller holds
 * the CUDA context. */
static int sc_enqueue_frame(SpeedChromaCudaState *s, CudaFunctions *cu_f, VmafPicture *ref_pic,
                            VmafPicture *dist_pic)
{
    CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                              CU_EVENT_WAIT_DEFAULT));
    int err = speed_cuda_pipeline_stage(s->pipeline, 0u, ref_pic, 1u, stream);
    if (!err)
        err = speed_cuda_pipeline_stage(s->pipeline, 1u, dist_pic, 1u, stream);
    if (!err)
        err = speed_cuda_pipeline_stage(s->pipeline, 2u, ref_pic, 2u, stream);
    if (!err)
        err = speed_cuda_pipeline_stage(s->pipeline, 3u, dist_pic, 2u, stream);
    if (err)
        return err;
    const SpeedGpuChannelBinding bindings[SC_CHANNELS] = {
        {.minuend = 0, .subtrahend = -1},
        {.minuend = 1, .subtrahend = -1},
        {.minuend = 2, .subtrahend = -1},
        {.minuend = 3, .subtrahend = -1},
    };
    return speed_cuda_pipeline_submit(s->pipeline, bindings, stream);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    SpeedChromaCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    const int err = sc_enqueue_frame(s, cu_f, ref_pic, dist_pic);
    const CUresult pop = cu_f->cuCtxPopCurrent(NULL);
    return err ? err : vmaf_cuda_result_to_errno((int)pop);
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

static void tally_frame(SpeedChromaCudaState *s, const SpeedGpuFrameResult *result, unsigned index)
{
    for (uint32_t ch = 0; ch < SC_CHANNELS; ch++) {
        speed_internal_tally_solve(&s->singular_tally, result->singular[ch] != 0,
                                   "speed_chroma_cuda");
        if (result->iteration_cap[ch] != 0) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "speed_chroma_cuda: eigenvalue QR iteration reached cap at frame %u, "
                     "possible non-convergence\n",
                     index);
        }
    }
}

/* speed_internal_clamp_score() refuses a non-finite score instead of
 * clamping it to speed_max_val, matching the CPU reference. */
static int append_chroma_scores(SpeedChromaCudaState *s, VmafFeatureCollector *collector,
                                unsigned index, const SpeedGpuFrameResult *result)
{
    const bool singular_u = result->singular[0] != 0 || result->singular[1] != 0;
    const bool singular_v = result->singular[2] != 0 || result->singular[3] != 0;
    const float uv = combine_chroma_uv(result->score[0], result->score[1], singular_u, singular_v);
    const double maximum = s->speed_chroma_max_val;
    double clamped_u = 0.0;
    double clamped_v = 0.0;
    double clamped_uv = 0.0;
    int err = speed_internal_clamp_score(result->score[0], maximum, index, "speed_chroma_cuda",
                                         "speed_chroma_u", &clamped_u);
    if (!err) {
        err = speed_internal_clamp_score(result->score[1], maximum, index, "speed_chroma_cuda",
                                         "speed_chroma_v", &clamped_v);
    }
    if (!err) {
        err = speed_internal_clamp_score(uv, maximum, index, "speed_chroma_cuda", "speed_chroma_uv",
                                         &clamped_uv);
    }
    if (err)
        return err;
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_u_score",
                                                   clamped_u, index);
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_v_score",
                                                   clamped_v, index);
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_uv_score",
                                                   clamped_uv, index);
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    SpeedChromaCudaState *s = fex->priv;
    SpeedGpuFrameResult result;
    const int err = speed_cuda_pipeline_collect(s->pipeline, &result);
    if (err)
        return err;
    tally_frame(s, &result, index);
    return append_chroma_scores(s, feature_collector, index, &result);
}

static const char *provided_features[] = {
    "Speed_chroma_feature_speed_chroma_u_score",
    "Speed_chroma_feature_speed_chroma_v_score",
    "Speed_chroma_feature_speed_chroma_uv_score",
    NULL,
};

/* ADR-1380: device-resident; the whole per-frame SpEED chain, including the
 * 25x25 linear algebra, runs on the device and one result block comes back
 * per frame. */
VmafFeatureExtractor vmaf_fex_speed_chroma_cuda = {
    .name = "speed_chroma_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(SpeedChromaCudaState),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
};

/* NOLINTEND(modernize-use-nullptr) */
