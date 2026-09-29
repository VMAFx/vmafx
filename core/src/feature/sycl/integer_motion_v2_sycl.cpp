/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  motion_v2 feature kernel on the SYCL backend (T7-23 / batch 3 part
 *  1c — ADR-0192 / ADR-0193). SYCL twin of motion_v2_vulkan (PR #146)
 *  and motion_v2_cuda (this PR's part 1b).
 *
 *  Stateless variant of `motion_sycl`: exploits convolution linearity
 *  (`SAD(blur(prev), blur(cur)) == sum(|blur(prev - cur)|)`) so each
 *  frame computes its score in one kernel launch over (prev_ref - cur_ref)
 *  without storing blurred frames across submits.
 *
 *  Self-contained submit / collect — does NOT register with
 *  vmaf_sycl_graph_register because motion_v2 needs the previous
 *  frame's raw ref pixels which the shared_frame buffer doesn't
 *  preserve across calls. Each submit reads the current ref Y plane
 *  from the shared frame, already uploaded once per frame for every
 *  twin, and the SAD pipeline copies it device-to-device into a private
 *  ping-pong (`d_pix[2]`); the next frame's submit reads it as "prev"
 *  (ADR-1369). No host copy, no second upload.
 *
 *  motion2_v2_score = min(score[i], score[i+1]) and motion3_v2_score
 *  (per-frame blend + clip + optional moving-average) are both emitted
 *  host-side in flush() — mirrors CPU integer_motion_v2.c::flush and the
 *  CUDA twin integer_motion_v2_cuda.c::flush_fex_cuda. The motion3_v2
 *  post-process and its option surface were added in ADR-1108 (the
 *  cross-backend follow-up to the CUDA twin landed in #909).
 *
 *  The SAD kernel is the motion pipeline shared with `motion_sycl`
 *  (integer_motion_pipeline_sycl.h): difference first, then the blur with
 *  the CPU's per-pass rounding and reflect-101 borders
 *  (`2 * size - idx - 2` for idx >= size), matching CPU
 *  `integer_motion_v2.c::motion_score_pipeline_8/_16` bit for bit.
 */

#include <sycl/sycl.hpp>

#include "integer_motion_pipeline_sycl.h"

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "config.h"
#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "motion_blend_tools.h"
#include "picture.h"
#include "sycl/common.h"

/* Default maximum value allowed for motion — mirrors
 * DEFAULT_MOTION_MAX_VAL in integer_motion_v2.c (the CPU reference). */
#define MOTION_V2_SYCL_DEFAULT_MAX_VAL (10000.0)

namespace
{

struct MotionV2StateSycl {
    /* Frame geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    size_t plane_bytes;

    /* SYCL state back-pointer. */
    VmafSyclState *sycl_state;

    /* Ping-pong of raw ref Y planes on device. d_pix[index%2] is the
     * current frame's slot; d_pix[(index+1)%2] is the previous. */
    void *d_pix[2];

    /* Single int64 SAD accumulator (device + host). */
    int64_t *d_sad;
    int64_t *h_sad;

    /* Submit/collect plumbing. */
    bool has_pending;
    unsigned pending_index;
    unsigned frame_index;

    /* fps-aware weight applied to the v2 SAD score in flush().
     * Default 1.0 is a no-op. Mirrors motion_sycl and motion_cuda
     * (ADR-0192 / PR #851). */
    double motion_fps_weight;

    /* motion3_v2 post-process options — mirror the CPU reference
     * (integer_motion_v2.c) option table byte-for-byte so a model
     * file carrying `motion_v2_sycl=motion_blend_factor=…` loads
     * and scores identically to the CPU and CUDA paths (ADR-1108). */
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    bool motion_moving_average;

    VmafDictionary *feature_name_dict;
};

} // namespace

namespace
{

/* Option table mirrors integer_motion_v2.c (CPU reference) for the
 * subset of options the SYCL twin's host-side motion3_v2 post-process
 * consumes: name / alias / type / default / min / max / flags match
 * byte-for-byte so co-scheduled CPU+SYCL runs name features identically
 * and model files load on either path (ADR-1108). motion_force_zero and
 * motion_five_frame_window are CPU-only knobs (the SYCL kernel always
 * computes the SAD, and the 5-frame window is unsupported per ADR-0337);
 * they are intentionally omitted from this twin's surface — matching the
 * CUDA twin integer_motion_v2_cuda.c. */
static const VmafOption motion_weight_option = {
    .name = "motion_fps_weight",
    .help = "fps-aware multiplicative weight/correction",
    .alias = "mfw",
    .offset = offsetof(MotionV2StateSycl, motion_fps_weight),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.0,
    .max = 5.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption motion_blend_factor_option = {
    .name = "motion_blend_factor",
    .help = "blend motion score given an offset",
    .alias = "mbf",
    .offset = offsetof(MotionV2StateSycl, motion_blend_factor),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.0,
    .max = 1.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

} // namespace

namespace
{

static const VmafOption motion_blend_offset_option = {
    .name = "motion_blend_offset",
    .help = "blend motion score starting from this offset",
    .alias = "mbo",
    .offset = offsetof(MotionV2StateSycl, motion_blend_offset),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 40.0},
    .min = 0.0,
    .max = 1000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption motion_max_option = {
    .name = "motion_max_val",
    .help = "maximum value allowed; larger values will be clipped to this value",
    .alias = "mmxv",
    .offset = offsetof(MotionV2StateSycl, motion_max_val),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = MOTION_V2_SYCL_DEFAULT_MAX_VAL},
    .min = 0.0,
    .max = 10000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

} // namespace

namespace
{

static const VmafOption motion_average_option = {
    .name = "motion_moving_average",
    .help = "smooth motion3 with a 2-frame moving average",
    .alias = "mma",
    .offset = offsetof(MotionV2StateSycl, motion_moving_average),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption options_motion_v2_sycl[] = {
    motion_weight_option, motion_blend_factor_option, motion_blend_offset_option,
    motion_max_option,    motion_average_option,      {.name = nullptr},
};

} // namespace

namespace
{

static int allocate_motion_v2(MotionV2StateSycl *s)
{
    /* The current frame comes from the shared frame (same packed layout);
     * idempotent when the read path or another twin already set it up. */
    const int shared_err = vmaf_sycl_shared_frame_init(s->sycl_state, s->width, s->height, s->bpc);
    if (shared_err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_v2_sycl: shared frame unavailable (%d)\n",
                 shared_err);
        return shared_err;
    }
    s->plane_bytes = (size_t)s->width * s->height * (s->bpc <= 8 ? 1u : 2u);
    s->d_pix[0] = vmaf_sycl_malloc_device(s->sycl_state, s->plane_bytes);
    s->d_pix[1] = vmaf_sycl_malloc_device(s->sycl_state, s->plane_bytes);
    s->d_sad = static_cast<int64_t *>(vmaf_sycl_malloc_device(s->sycl_state, sizeof(int64_t)));
    s->h_sad = static_cast<int64_t *>(vmaf_sycl_malloc_host(s->sycl_state, sizeof(int64_t)));
    if (!s->d_pix[0] || !s->d_pix[1] || !s->d_sad || !s->h_sad) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_v2_sycl: USM allocation failed\n");
        return -ENOMEM;
    }
    return 0;
}

} // namespace

namespace
{
static int close_fex_sycl(VmafFeatureExtractor *fex);

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);

    /* The 5-tap SYCL motion_v2 kernel uses reflect-101 mirror padding; the
     * reflection 2*sup - idx - 2 is negative when sup < 3.
     * Refuse smaller frames up front to prevent out-of-bounds device reads.
     * Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_v2_sycl: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->frame_index = 0;
    s->has_pending = false;

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_v2_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;
    const int alloc_err = allocate_motion_v2(s);
    if (alloc_err) {
        (void)close_fex_sycl(fex);
        return alloc_err;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }

    return 0;
}

} // namespace

namespace
{

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    const void *cur = vmaf_sycl_get_shared_plane(s->sycl_state, 1, 0);
    if (!qptr || !cur) {
        return -EINVAL;
    }
    /* This frame's ref luma is on the device already; wait for its upload
     * on the device. The pipeline keeps a copy as the next frame's "prev". */
    const int barrier_err = vmaf_sycl_queue_after_upload(s->sycl_state, qptr);
    if (barrier_err) {
        return barrier_err;
    }
    sycl::queue &q = *qptr;
    const unsigned cur_idx = index % 2u;
    const motion_sycl_pipeline::SadArgs args = {.prev = s->d_pix[(index + 1u) % 2u],
                                                .cur = cur,
                                                .cur_copy = s->d_pix[cur_idx],
                                                .sad = s->d_sad,
                                                .width = s->width,
                                                .height = s->height,
                                                .bpc = s->bpc};
    try {
        if (index > 0) {
            q.memset(s->d_sad, 0, sizeof(int64_t));
            motion_sycl_pipeline::enqueue_sad(q, args);
            q.memcpy(s->h_sad, s->d_sad, sizeof(int64_t));
        } else {
            motion_sycl_pipeline::enqueue_copy(q, args);
        }
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_v2_sycl: submitting frame %u: %s\n", index,
                 e.what());
        return -EIO;
    }

    s->pending_index = index;
    s->has_pending = true;
    s->frame_index = index + 1u;
    return 0;
}

} // namespace

namespace
{

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    qptr->wait();

    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0.0, index);
    }

    const double sad_score = (double)*s->h_sad / 256.0 / ((double)s->width * (double)s->height);
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion_v2_sad_score",
                                                   sad_score, index);
}

} // namespace

namespace
{

static unsigned motion_frame_count(VmafFeatureCollector *collector, const char *sad_name)
{
    unsigned count = 0;
    double unused;
    while (!vmaf_feature_collector_get_score(collector, sad_name, &unused, count)) {
        count++;
    }
    return count;
}

static double motion_stamp_value(VmafFeatureCollector *collector, const MotionV2StateSycl *s,
                                 const char *sad_name, unsigned frame_count)
{
    constexpr unsigned min_index = 1;
    double value = 0.0;
    double sad;
    if (frame_count > min_index &&
        !vmaf_feature_collector_get_score(collector, sad_name, &sad, min_index)) {
        value = MIN(motion_blend(sad, s->motion_blend_factor, s->motion_blend_offset),
                    s->motion_max_val);
    }
    return value;
}

} // namespace

namespace
{

static int append_motion_frame(VmafFeatureCollector *collector, MotionV2StateSycl *s,
                               const char *sad_name, unsigned index, unsigned frame_count,
                               double stamp_value, double &previous)
{
    double score_current;
    vmaf_feature_collector_get_score(collector, sad_name, &score_current, index);
    score_current *= s->motion_fps_weight;
    double motion2 = score_current;
    if (index + 1 < frame_count) {
        double score_next;
        vmaf_feature_collector_get_score(collector, sad_name, &score_next, index + 1);
        score_next *= s->motion_fps_weight;
        motion2 = score_current < score_next ? score_current : score_next;
    }
    int err = vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "VMAF_integer_feature_motion2_v2_score", motion2, index);
    if (err) {
        return err;
    }
    double motion3;
    if (index < 1) {
        motion3 = stamp_value;
        previous = stamp_value;
    } else {
        double const processed =
            MIN(motion_blend(motion2, s->motion_blend_factor, s->motion_blend_offset),
                s->motion_max_val);
        motion3 = s->motion_moving_average ? (processed + previous) / 2.0 : processed;
        previous = processed;
    }
    err = vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "VMAF_integer_feature_motion3_v2_score", motion3, index);
    return err;
}

} // namespace

namespace
{

static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);

    /* Resolve the (possibly renamed, for sfr/hfr co-schedule) SAD feature
     * name from the dict — mirrors integer_motion_v2.c::flush and the CUDA
     * twin flush_fex_cuda. */
    VmafDictionaryEntry const *e_sad =
        vmaf_dictionary_get(&s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score", 0);
    const char *sad_name = e_sad ? e_sad->val : "VMAF_integer_feature_motion_v2_sad_score";

    const unsigned n_frames = motion_frame_count(feature_collector, sad_name);
    if (n_frames < 2) {
        return 1;
    }
    const double stamp_value = motion_stamp_value(feature_collector, s, sad_name, n_frames);
    double prev_processed = 0.;
    for (unsigned i = 0; i < n_frames; i++) {
        const int err = append_motion_frame(feature_collector, s, sad_name, i, n_frames,
                                            stamp_value, prev_processed);
        if (err) {
            return err;
        }
    }

    return 1;
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);
    if (s->sycl_state) {
        if (s->d_pix[0])
            vmaf_sycl_free(s->sycl_state, s->d_pix[0]);
        if (s->d_pix[1])
            vmaf_sycl_free(s->sycl_state, s->d_pix[1]);
        if (s->d_sad)
            vmaf_sycl_free(s->sycl_state, s->d_sad);
        if (s->h_sad)
            vmaf_sycl_free(s->sycl_state, s->h_sad);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_motion_v2_sycl[] = {
    "VMAF_integer_feature_motion_v2_sad_score", "VMAF_integer_feature_motion2_v2_score",
    "VMAF_integer_feature_motion3_v2_score", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_integer_motion_v2_sycl = {
    .name = "motion_v2_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = flush_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_motion_v2_sycl,
    .priv_size = sizeof(MotionV2StateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_motion_v2_sycl,
};
