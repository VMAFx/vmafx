/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_temporal feature extractor — CUDA backend, device-resident
 *  (ADR-1380, the CUDA port of ADR-1358; refines ADR-0567).
 *
 *  Temporal design: the raw luma planes of the last two frames stay on the
 *  device in two slots (reference and distorted per slot). submit() copies
 *  the current frame's luma planes, device to device, into slot `index % 2`
 *  and, from the second frame on, enqueues the SpEED chain of
 *  speed_cuda_pipeline.c on the temporal difference `previous - current`
 *  (speed.c extract(): subtract_image()) on the reference picture's stream.
 *  It never waits. collect() waits once and reads the frame score. Frame 0
 *  emits 0, as the CPU reference does.
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
#include "picture_cuda.h"

#include "cuda_helper.cuh"

#include "feature/speed_internal.h"
#include "cuda/speed_cuda_pipeline.h"
#include "cuda/speed_temporal_cuda.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define ST_CHANNELS 2u /* reference, distorted */
#define ST_SLOTS 2u    /* current and previous frame */

#define ST_DEFAULT_SIGMA_NN (0.29)
#define ST_DEFAULT_MAX_VAL (1000.0)
#define ST_DEFAULT_NN_FLOOR (0.0)
#define ST_DEFAULT_KERNELSCALE (1.0)
#define ST_DEFAULT_PRESCALE (1.0)
#define ST_DEFAULT_PRESCALE_METHOD ("nearest")

typedef struct SpeedTemporalCudaState {
    SpeedCudaPipeline *pipeline;
    SpeedInternalSingularTally singular_tally;

    double speed_temporal_kernelscale;
    double speed_temporal_prescale;
    char *speed_temporal_prescale_method;
    double speed_temporal_sigma_nn;
    double speed_temporal_nn_floor;
    double speed_temporal_max_val;
    bool speed_temporal_use_ref_diff;

    VmafDictionary *feature_name_dict;
} SpeedTemporalCudaState;

static const VmafOption options[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_kernelscale),
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
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_prescale),
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
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = ST_DEFAULT_PRESCALE_METHOD,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_sigma_nn),
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
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_nn_floor),
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
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_max_val),
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
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_use_ref_diff),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "urd",
    },
    {0},
};

static int close_fex_st(VmafFeatureExtractor *fex)
{
    SpeedTemporalCudaState *s = fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_temporal_cuda");
    int rc = speed_cuda_pipeline_close(&s->pipeline);
    if (s->feature_name_dict) {
        const int err = vmaf_dictionary_free(&s->feature_name_dict);
        rc = rc ? rc : err;
    }
    return rc;
}

static int init_fex_st(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                       unsigned w, unsigned h)
{
    (void)pix_fmt;
    SpeedTemporalCudaState *s = fex->priv;
    /* speed_temporal always weights with mode 0 (speed.c init()). */
    const SpeedInternalOptions opt = {
        .speed_kernelscale = s->speed_temporal_kernelscale,
        .speed_prescale = s->speed_temporal_prescale,
        .speed_prescale_method = s->speed_temporal_prescale_method,
        .speed_sigma_nn = s->speed_temporal_sigma_nn,
        .speed_nn_floor = s->speed_temporal_nn_floor,
        .speed_weight_var_mode = 0,
    };
    /* On failure the partial pipeline stays in s->pipeline; the engine owes
     * this extractor a close after a failed init (ADR-1336). */
    const int err = speed_cuda_pipeline_open(&s->pipeline, fex->cu_state, &opt, w, h, bpc,
                                             ST_CHANNELS, ST_CHANNELS * ST_SLOTS);
    if (err)
        return err;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    return s->feature_name_dict ? 0 : -ENOMEM;
}

/* Copy the current luma planes into slot `index % 2`; from the second frame
 * on, enqueue the chain on subtract_image(ref[prev], ref[cur]) and
 * subtract_image(dis[prev], dis[cur] or, with speed_use_ref_diff, ref[cur]).
 * The caller holds the CUDA context. */
static int st_enqueue_frame(SpeedTemporalCudaState *s, CudaFunctions *cu_f, VmafPicture *ref_pic,
                            VmafPicture *dist_pic, unsigned index)
{
    CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                              CU_EVENT_WAIT_DEFAULT));
    const uint32_t current = ST_CHANNELS * (index % ST_SLOTS);
    const uint32_t previous = ST_CHANNELS * ((index + 1u) % ST_SLOTS);
    int err = speed_cuda_pipeline_stage(s->pipeline, current, ref_pic, 0u, stream);
    if (!err)
        err = speed_cuda_pipeline_stage(s->pipeline, current + 1u, dist_pic, 0u, stream);
    if (err)
        return err;
    if (index == 0u)
        return speed_cuda_pipeline_fence(s->pipeline, stream);
    const int32_t dis_subtrahend =
        (int32_t)(s->speed_temporal_use_ref_diff ? current : current + 1u);
    const SpeedGpuChannelBinding bindings[ST_CHANNELS] = {
        {.minuend = (int32_t)previous, .subtrahend = (int32_t)current},
        {.minuend = (int32_t)previous + 1, .subtrahend = dis_subtrahend},
    };
    return speed_cuda_pipeline_submit(s->pipeline, bindings, stream);
}

static int submit_fex_st(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                         VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    SpeedTemporalCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    const int err = st_enqueue_frame(s, cu_f, ref_pic, dist_pic, index);
    const CUresult pop = cu_f->cuCtxPopCurrent(NULL);
    return err ? err : vmaf_cuda_result_to_errno((int)pop);
}

static void st_tally_frame(SpeedTemporalCudaState *s, const SpeedGpuFrameResult *result,
                           unsigned index)
{
    for (uint32_t ch = 0; ch < ST_CHANNELS; ch++) {
        speed_internal_tally_solve(&s->singular_tally, result->singular[ch] != 0,
                                   "speed_temporal_cuda");
        if (result->iteration_cap[ch] != 0) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "speed_temporal_cuda: eigenvalue QR iteration reached cap at frame %u, "
                     "possible non-convergence\n",
                     index);
        }
    }
}

static int collect_fex_st(VmafFeatureExtractor *fex, unsigned index,
                          VmafFeatureCollector *feature_collector)
{
    SpeedTemporalCudaState *s = fex->priv;
    if (index == 0u) {
        /* Frame 0 only stages its planes (slot 0); its one wait retires the
         * copies fenced by speed_cuda_pipeline_fence() before frame 1's
         * submit reads them as the previous frame. */
        const int err = speed_cuda_pipeline_wait(s->pipeline);
        if (err)
            return err;
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }
    SpeedGpuFrameResult result;
    int err = speed_cuda_pipeline_collect(s->pipeline, &result);
    if (err)
        return err;
    st_tally_frame(s, &result, index);
    /* speed_internal_clamp_score() refuses a non-finite score instead of
     * clamping it to speed_max_val, matching the CPU reference. */
    double clipped = 0.0;
    err = speed_internal_clamp_score(result.score[0], s->speed_temporal_max_val, index,
                                     "speed_temporal_cuda", "speed_temporal", &clipped);
    if (err)
        return err;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_temporal_feature_speed_temporal_score",
                                                   clipped, index);
}

static const char *provided_features[] = {
    "Speed_temporal_feature_speed_temporal_score",
    NULL,
};

/* TEMPORAL guarantees in-order frames: the previous frame's planes stay on
 * the device between submits (ADR-1380). */
VmafFeatureExtractor vmaf_fex_speed_temporal_cuda = {
    .name = "speed_temporal_cuda",
    .init = init_fex_st,
    .submit = submit_fex_st,
    .collect = collect_fex_st,
    .close = close_fex_st,
    .options = options,
    .priv_size = sizeof(SpeedTemporalCudaState),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_CUDA,
};

/* NOLINTEND(modernize-use-nullptr) */
