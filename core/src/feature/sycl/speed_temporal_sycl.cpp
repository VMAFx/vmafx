/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_temporal feature extractor — SYCL backend, device-resident
 *  (ADR-1358, refines ADR-0567).
 *
 *  Temporal design: the raw luma planes of the last two frames stay on the
 *  device in two slots (reference and distorted per slot). submit() uploads
 *  the current frame into slot `index % 2` and, from the second frame on,
 *  enqueues the SpEED chain of speed_sycl_pipeline.cpp on the temporal
 *  difference `previous - current` (speed.c extract(): subtract_image()),
 *  never waiting. collect() waits once and reads the frame score. Frame 0
 *  emits 0, as the CPU reference does.
 *
 *  This TU holds no device kernel; every kernel lives in
 *  speed_sycl_pipeline.cpp (core/test/test_sycl_kernel_source_contract.py).
 *
 *  Output feature: Speed_temporal_feature_speed_temporal_score.
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

constexpr uint32_t kTemporalChannels = 2u; /* reference, distorted */
constexpr uint32_t kTemporalSlots = 2u;    /* current and previous frame */

struct SpeedTemporalSyclState {
    VmafSyclState *sycl_state;
    speed_sycl::Pipeline *pipeline;
    SpeedInternalSingularTally singular_tally;
    double speed_temporal_kernelscale;
    double speed_temporal_prescale;
    char *speed_temporal_prescale_method;
    double speed_temporal_sigma_nn;
    double speed_temporal_nn_floor;
    double speed_temporal_max_val;
    bool speed_temporal_use_ref_diff;
    VmafDictionary *feature_name_dict;
};

} // namespace

namespace
{

const VmafOption kernelscale_option = {
    .name = "speed_kernelscale",
    .help = "scaling factor for the Gaussian kernel",
    .alias = "ks",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_kernelscale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.1,
    .max = 4.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption prescale_option = {
    .name = "speed_prescale",
    .help = "scaling factor for the frame",
    .alias = "ps",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_prescale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.1,
    .max = 4.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption prescale_method_option = {
    .name = "speed_prescale_method",
    .help = "scaling method [nearest, bilinear, bicubic, lanczos4]",
    .alias = "psm",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_prescale_method),
    .type = VMAF_OPT_TYPE_STRING,
    .default_val = {.s = "nearest"},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption sigma_option = {
    .name = "speed_sigma_nn",
    .help = "standard deviation of neural noise",
    .alias = "snn",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_sigma_nn),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 0.29},
    .min = 0.1,
    .max = 2.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

} // namespace

namespace
{

const VmafOption floor_option = {
    .name = "speed_nn_floor",
    .help = "neural noise floor fraction",
    .alias = "nnf",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_nn_floor),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 0.0},
    .min = 0.0,
    .max = 1.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption max_value_option = {
    .name = "speed_max_val",
    .help = "clip output to this maximum",
    .alias = "mxv",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_max_val),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1000.0},
    .min = 0.0,
    .max = 1000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption use_ref_option = {
    .name = "speed_use_ref_diff",
    .help = "use reference frame difference instead of distorted",
    .alias = "urd",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_use_ref_diff),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

const VmafOption options_temporal[] = {
    kernelscale_option, prescale_option,  prescale_method_option, sigma_option,
    floor_option,       max_value_option, use_ref_option,         {.name = nullptr},
};

} // namespace

namespace
{

int close_temporal_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<SpeedTemporalSyclState *>(fex->priv);
    speed_internal_report_singular(&s->singular_tally, "speed_temporal_sycl");
    speed_sycl::pipeline_destroy(&s->pipeline);
    if (s->feature_name_dict) {
        vmaf_dictionary_free(&s->feature_name_dict);
    }
    return 0;
}

int create_temporal_pipeline(SpeedTemporalSyclState *s, unsigned bpc, unsigned width,
                             unsigned height)
{
    /* speed_temporal always weights with mode 0 (speed.c init()). */
    const SpeedInternalOptions opt = {
        .speed_kernelscale = s->speed_temporal_kernelscale,
        .speed_prescale = s->speed_temporal_prescale,
        .speed_prescale_method = s->speed_temporal_prescale_method,
        .speed_sigma_nn = s->speed_temporal_sigma_nn,
        .speed_nn_floor = s->speed_temporal_nn_floor,
        .speed_weight_var_mode = 0,
    };
    SpeedInternalDimensions dim{};
    int err = speed_internal_init_dimensions(&dim, static_cast<int>(width),
                                             static_cast<int>(height), opt.speed_prescale);
    if (err) {
        return err;
    }
    speed_sycl::PipelineConfig config{};
    err = speed_sycl::configure(dim, opt, bpc, config);
    if (err) {
        return err;
    }
    config.queue = vmaf_sycl_get_queue_ptr(s->sycl_state);
    config.channels = kTemporalChannels;
    config.raw_planes = kTemporalChannels * kTemporalSlots;
    config.staged = kTemporalChannels;
    return speed_sycl::pipeline_create(&s->pipeline, config);
}

} // namespace

namespace
{

int init_temporal_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                       unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<SpeedTemporalSyclState *>(fex->priv);
    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "speed_temporal_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;
    const int err = create_temporal_pipeline(s, bpc, w, h);
    if (err) {
        (void)close_temporal_sycl(fex);
        return err;
    }
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_temporal_sycl(fex);
        return -ENOMEM;
    }
    return 0;
}

int submit_temporal_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                         VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<SpeedTemporalSyclState *>(fex->priv);
    int err = speed_sycl::stage_plane(s->pipeline, 0u, ref_pic, 0u);
    err |= speed_sycl::stage_plane(s->pipeline, 1u, dist_pic, 0u);
    if (err) {
        return -EINVAL;
    }
    const auto current = static_cast<int32_t>(kTemporalChannels * (index % kTemporalSlots));
    const auto previous = static_cast<int32_t>(kTemporalChannels * ((index + 1u) % kTemporalSlots));
    err =
        speed_sycl::pipeline_upload(s->pipeline, static_cast<uint32_t>(current), kTemporalChannels);
    if (err || index == 0u) {
        return err;
    }
    /* subtract_image(ref[prev], ref[cur]) and subtract_image(dis[prev],
     * dis[cur] or, with speed_use_ref_diff, ref[cur]). */
    const int32_t dis_subtrahend = s->speed_temporal_use_ref_diff ? current : current + 1;
    const speed_sycl::ChannelBinding bindings[kTemporalChannels] = {
        {.minuend = previous, .subtrahend = current},
        {.minuend = previous + 1, .subtrahend = dis_subtrahend},
    };
    return speed_sycl::pipeline_submit(s->pipeline, bindings);
}

} // namespace

namespace
{

int collect_temporal_sycl(VmafFeatureExtractor *fex, unsigned index,
                          VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<SpeedTemporalSyclState *>(fex->priv);
    if (index == 0u) {
        /* The upload must land before submit() reuses the staging planes. */
        const int err = speed_sycl::pipeline_wait(s->pipeline);
        if (err) {
            return err;
        }
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }
    speed_sycl::FrameResult result{};
    int err = speed_sycl::pipeline_collect(s->pipeline, &result);
    if (err) {
        return err;
    }
    for (uint32_t ch = 0; ch < kTemporalChannels; ch++) {
        speed_internal_tally_solve(&s->singular_tally, result.singular[ch] != 0,
                                   "speed_temporal_sycl");
        if (result.iteration_cap[ch] != 0) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "speed_temporal_sycl: eigenvalue QR iteration reached cap at frame %u, "
                     "possible non-convergence\n",
                     index);
        }
    }
    /* speed_internal_clamp_score() refuses a non-finite score instead of
     * clamping it to speed_max_val, matching the CPU reference. */
    double clipped = 0.0;
    err = speed_internal_clamp_score(result.score[0], s->speed_temporal_max_val, index,
                                     "speed_temporal_sycl", "speed_temporal", &clipped);
    if (err) {
        return err;
    }
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_temporal_feature_speed_temporal_score",
                                                   clipped, index);
}

const char *provided_features_temporal[] = {
    "Speed_temporal_feature_speed_temporal_score",
    nullptr,
};

} // namespace

/* TEMPORAL guarantees in-order frames: the previous frame's planes stay on the
 * device between submits. */
extern "C" VmafFeatureExtractor vmaf_fex_speed_temporal_sycl = {
    .name = "speed_temporal_sycl",
    .init = init_temporal_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_temporal_sycl,
    .submit = submit_temporal_sycl,
    .collect = collect_temporal_sycl,
    .options = options_temporal,
    .priv_size = sizeof(SpeedTemporalSyclState),
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_temporal,
};
