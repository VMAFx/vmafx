/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_chroma feature extractor — SYCL backend, device-resident
 *  (ADR-1358, refines ADR-0567).
 *
 *  submit() copies the four chroma planes (U and V of reference and
 *  distorted) into pinned staging, enqueues one upload and the whole SpEED
 *  chain of speed_sycl_pipeline.cpp for both U and V, and returns without
 *  waiting. collect() waits once and reads one FrameResult: the U and V
 *  scores and the per-channel singularity flags. Only the U/V combination,
 *  the clamp and the collector append run on the host, on those scalars.
 *
 *  This TU holds no device kernel; every kernel lives in
 *  speed_sycl_pipeline.cpp (core/test/test_sycl_kernel_source_contract.py).
 */

#include <cerrno>
#include <cstddef>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "sycl/common.h"
#include "feature/speed_internal.h"
#include "speed_sycl_pipeline.h"

namespace
{

constexpr uint32_t kChromaChannels = 4u; /* U ref, U dis, V ref, V dis */

struct SpeedChromaSyclState {
    VmafSyclState *sycl_state;
    speed_sycl::Pipeline *pipeline;
    SpeedInternalSingularTally singular_tally;
    double speed_chroma_kernelscale;
    double speed_chroma_prescale;
    char *speed_chroma_prescale_method;
    double speed_chroma_sigma_nn;
    double speed_chroma_nn_floor;
    double speed_chroma_max_val;
    int speed_weight_var_mode;
    VmafDictionary *feature_name_dict;
};

} // namespace

namespace
{

const VmafOption option_kernelscale = {
    .name = "speed_kernelscale",
    .help = "scaling factor for the Gaussian kernel",
    .alias = "ks",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_kernelscale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.1,
    .max = 4.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption option_prescale = {
    .name = "speed_prescale",
    .help = "scaling factor for the frame",
    .alias = "ps",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_prescale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.1,
    .max = 4.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption option_prescale_method = {
    .name = "speed_prescale_method",
    .help = "scaling method",
    .alias = "psm",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_prescale_method),
    .type = VMAF_OPT_TYPE_STRING,
    .default_val = {.s = "nearest"},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption option_sigma_nn = {
    .name = "speed_sigma_nn",
    .help = "standard deviation of neural noise",
    .alias = "snn",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_sigma_nn),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 0.29},
    .min = 0.1,
    .max = 2.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

} // namespace

namespace
{

const VmafOption option_nn_floor = {
    .name = "speed_nn_floor",
    .help = "neural noise floor fraction",
    .alias = "nnf",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_nn_floor),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 0.0},
    .min = 0.0,
    .max = 1.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption option_maximum = {
    .name = "speed_max_val",
    .help = "clip output to this maximum",
    .alias = "mxv",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_max_val),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1000.0},
    .min = 0.0,
    .max = 1000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption option_weight_mode = {
    .name = "speed_weight_var_mode",
    .help = "variance weighting mode (0-6)",
    .alias = "wvm",
    .offset = offsetof(SpeedChromaSyclState, speed_weight_var_mode),
    .type = VMAF_OPT_TYPE_INT,
    .default_val = {.i = 0},
    .min = 0,
    .max = 6,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption options_chroma[] = {
    option_kernelscale, option_prescale, option_prescale_method, option_sigma_nn,
    option_nn_floor,    option_maximum,  option_weight_mode,     {.name = nullptr},
};

} // namespace

namespace
{

int close_chroma_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<SpeedChromaSyclState *>(fex->priv);
    speed_internal_report_singular(&s->singular_tally, "speed_chroma_sycl");
    speed_sycl::pipeline_destroy(&s->pipeline);
    if (s->feature_name_dict) {
        vmaf_dictionary_free(&s->feature_name_dict);
    }
    return 0;
}

int create_chroma_pipeline(SpeedChromaSyclState *s, enum VmafPixelFormat format, unsigned bpc,
                           unsigned width, unsigned height)
{
    unsigned chroma_w = 0;
    unsigned chroma_h = 0;
    int err = speed_chroma_dimensions(width, height, format, &chroma_w, &chroma_h);
    if (err) {
        return err;
    }
    const SpeedInternalOptions opt = {
        .speed_kernelscale = s->speed_chroma_kernelscale,
        .speed_prescale = s->speed_chroma_prescale,
        .speed_prescale_method = s->speed_chroma_prescale_method,
        .speed_sigma_nn = s->speed_chroma_sigma_nn,
        .speed_nn_floor = s->speed_chroma_nn_floor,
        .speed_weight_var_mode = s->speed_weight_var_mode,
    };
    SpeedInternalDimensions dim{};
    err = speed_internal_init_dimensions(&dim, (int)chroma_w, (int)chroma_h, opt.speed_prescale);
    if (err) {
        return err;
    }
    speed_sycl::PipelineConfig config{};
    err = speed_sycl::configure(dim, opt, bpc, config);
    if (err) {
        return err;
    }
    config.queue = vmaf_sycl_get_queue_ptr(s->sycl_state);
    config.channels = kChromaChannels;
    config.raw_planes = kChromaChannels;
    config.staged = kChromaChannels;
    return speed_sycl::pipeline_create(&s->pipeline, config);
}

} // namespace

namespace
{

int init_chroma_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat format, unsigned bpc,
                     unsigned width, unsigned height)
{
    auto *s = static_cast<SpeedChromaSyclState *>(fex->priv);
    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "speed_chroma_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;
    const int err = create_chroma_pipeline(s, format, bpc, width, height);
    if (err) {
        (void)close_chroma_sycl(fex);
        return err;
    }
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_chroma_sycl(fex);
        return -ENOMEM;
    }
    return 0;
}

int submit_chroma_sycl(VmafFeatureExtractor *fex, VmafPicture *reference, VmafPicture *reference_90,
                       VmafPicture *distorted, VmafPicture *distorted_90, unsigned index)
{
    (void)reference_90;
    (void)distorted_90;
    (void)index;
    auto *s = static_cast<SpeedChromaSyclState *>(fex->priv);
    /* Staging order matches the channel pairs: (U ref, U dis), (V ref, V dis). */
    int err = speed_sycl::stage_plane(s->pipeline, 0u, reference, 1u);
    err |= speed_sycl::stage_plane(s->pipeline, 1u, distorted, 1u);
    err |= speed_sycl::stage_plane(s->pipeline, 2u, reference, 2u);
    err |= speed_sycl::stage_plane(s->pipeline, 3u, distorted, 2u);
    if (err) {
        return -EINVAL;
    }
    err = speed_sycl::pipeline_upload(s->pipeline, 0u, kChromaChannels);
    if (err) {
        return err;
    }
    const speed_sycl::ChannelBinding bindings[kChromaChannels] = {
        {.minuend = 0, .subtrahend = -1},
        {.minuend = 1, .subtrahend = -1},
        {.minuend = 2, .subtrahend = -1},
        {.minuend = 3, .subtrahend = -1},
    };
    return speed_sycl::pipeline_submit(s->pipeline, bindings);
}

} // namespace

namespace
{

/* uv from u and v, imputing across a singular channel exactly as
 * extract_chroma() in speed.c does. */
float combine_chroma_uv(float score_u, float score_v, bool singular_u, bool singular_v)
{
    if (singular_u && !singular_v) {
        return score_v;
    }
    if (singular_v && !singular_u) {
        return score_u;
    }
    return (score_u + score_v) * 0.5f;
}

void tally_frame(SpeedChromaSyclState *s, const speed_sycl::FrameResult &result, unsigned index)
{
    for (uint32_t ch = 0; ch < kChromaChannels; ch++) {
        speed_internal_tally_solve(&s->singular_tally, result.singular[ch] != 0,
                                   "speed_chroma_sycl");
        if (result.iteration_cap[ch] != 0) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "speed_chroma_sycl: eigenvalue QR iteration reached cap at frame %u, "
                     "possible non-convergence\n",
                     index);
        }
    }
}

} // namespace

namespace
{

int append_chroma_scores(SpeedChromaSyclState *s, VmafFeatureCollector *collector, unsigned index,
                         const speed_sycl::FrameResult &result)
{
    const bool singular_u = result.singular[0] != 0 || result.singular[1] != 0;
    const bool singular_v = result.singular[2] != 0 || result.singular[3] != 0;
    const float uv = combine_chroma_uv(result.score[0], result.score[1], singular_u, singular_v);
    /* speed_internal_clamp_score() refuses a non-finite score instead of
     * clamping it to speed_max_val, matching the CPU reference. */
    const double maximum = s->speed_chroma_max_val;
    double clamped_u = 0.0;
    double clamped_v = 0.0;
    double clamped_uv = 0.0;
    int error = speed_internal_clamp_score(result.score[0], maximum, index, "speed_chroma_sycl",
                                           "speed_chroma_u", &clamped_u);
    error = error ? error :
                    speed_internal_clamp_score(result.score[1], maximum, index, "speed_chroma_sycl",
                                               "speed_chroma_v", &clamped_v);
    error = error ? error :
                    speed_internal_clamp_score(uv, maximum, index, "speed_chroma_sycl",
                                               "speed_chroma_uv", &clamped_uv);
    if (error) {
        return error;
    }
    error |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                     "Speed_chroma_feature_speed_chroma_u_score",
                                                     clamped_u, index);
    error |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                     "Speed_chroma_feature_speed_chroma_v_score",
                                                     clamped_v, index);
    error |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                     "Speed_chroma_feature_speed_chroma_uv_score",
                                                     clamped_uv, index);
    return error;
}

int collect_chroma_sycl(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *collector)
{
    auto *s = static_cast<SpeedChromaSyclState *>(fex->priv);
    speed_sycl::FrameResult result{};
    const int err = speed_sycl::pipeline_collect(s->pipeline, &result);
    if (err) {
        return err;
    }
    tally_frame(s, result, index);
    return append_chroma_scores(s, collector, index, result);
}

} // namespace

namespace
{

const char *provided_features_chroma[] = {
    "Speed_chroma_feature_speed_chroma_u_score",
    "Speed_chroma_feature_speed_chroma_v_score",
    "Speed_chroma_feature_speed_chroma_uv_score",
    nullptr,
};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_speed_chroma_sycl = {
    .name = "speed_chroma_sycl",
    .init = init_chroma_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_chroma_sycl,
    .submit = submit_chroma_sycl,
    .collect = collect_chroma_sycl,
    .options = options_chroma,
    .priv_size = sizeof(SpeedChromaSyclState),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_chroma,
};
