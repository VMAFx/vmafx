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
 *  ring (`d_pix[]`); a later frame's submit reads it as "prev" (ADR-1369):
 *  the next frame with a ring of two, the frame after that with a ring of
 *  three (motion_five_frame_window, ADR-1491). No host copy, no second
 *  upload.
 *
 *  collect() publishes the CPU's motion_v2_sad_score: the normalised SAD,
 *  fps-weighted and capped at motion_max_val (integer_motion_v2.c::extract),
 *  0 for the frames without an earlier frame to difference against.
 *  advance() and flush() derive motion2_v2 and motion3_v2 from those stored
 *  scores, each frame once its window is complete (ADR-2090), with the CPU's
 *  own functions, vmaf_motion_window_advance() / _flush() (motion_window.h,
 *  ADR-1478), including its 0 / 0 for a one-frame input
 *  (T-SYCL-MOTION-V2-OPTION-PARITY-2026-09-30). The option surface was added
 *  in ADR-1108 (the cross-backend follow-up to the CUDA twin landed in
 *  #909).
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
#include "motion_window.h"
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

    /* Ring of raw ref Y planes on device, `ring` of them in use.
     * d_pix[index % ring] is the current frame's slot; d_pix[(index + 1) %
     * ring] holds the frame the SAD is taken against: the previous one with
     * a ring of two, the one two back with a ring of three
     * (motion_five_frame_window). */
    void *d_pix[3];
    unsigned ring;

    /* Single int64 SAD accumulator (device + host). */
    int64_t *d_sad;
    int64_t *h_sad;

    /* Submit/collect plumbing. */
    bool has_pending;
    unsigned pending_index;
    unsigned frame_index;

    /* fps-aware weight applied to the v2 SAD score in collect(), before
     * the motion_max_val cap, as integer_motion_v2.c::extract does. Default
     * 1.0 is a no-op. */
    double motion_fps_weight;

    /* motion3_v2 post-process options — mirror the CPU reference
     * (integer_motion_v2.c) option table byte-for-byte so a model
     * file carrying `motion_v2_sycl=motion_blend_factor=…` loads
     * and scores identically to the CPU and CUDA paths (ADR-1108). */
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    bool motion_five_frame_window;
    bool motion_moving_average;

    VmafDictionary *feature_name_dict;
    /* motion2_v2 / motion3_v2, derived as the SAD scores come in (ADR-2090). */
    VmafMotionWindowState window_state;
};

} // namespace

namespace
{

/* Option table mirrors integer_motion_v2.c (CPU reference) for the
 * subset of options the SYCL twin's host-side motion3_v2 post-process
 * consumes: name / alias / type / default / min / max / flags match
 * byte-for-byte so co-scheduled CPU+SYCL runs name features identically
 * and model files load on either path (ADR-1108). motion_force_zero is a
 * CPU-only knob (the SYCL kernel always computes the SAD) and is
 * intentionally omitted from this twin's surface — matching the CUDA twin
 * integer_motion_v2_cuda.c. */
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

static const VmafOption motion_five_frame_option = {
    .name = "motion_five_frame_window",
    .help = "use five-frame temporal window",
    .alias = "mffw",
    .offset = offsetof(MotionV2StateSycl, motion_five_frame_window),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

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
    motion_max_option,    motion_five_frame_option,   motion_average_option,
    {.name = nullptr},
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
    bool planes_ok = true;
    for (unsigned i = 0; i < s->ring; i++) {
        s->d_pix[i] = vmaf_sycl_malloc_device(s->sycl_state, s->plane_bytes);
        planes_ok = planes_ok && (s->d_pix[i] != nullptr);
    }
    s->d_sad = static_cast<int64_t *>(vmaf_sycl_malloc_device(s->sycl_state, sizeof(int64_t)));
    s->h_sad = static_cast<int64_t *>(vmaf_sycl_malloc_host(s->sycl_state, sizeof(int64_t)));
    if (!planes_ok || !s->d_sad || !s->h_sad) {
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
    /* The frame the SAD is taken against is ring - 1 frames back: the CPU's
     * min_idx (integer_motion_v2.c::extract). */
    s->ring = s->motion_five_frame_window ? 3u : 2u;

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
     * on the device. The pipeline keeps a copy as a later frame's "prev". */
    const int barrier_err = vmaf_sycl_queue_after_upload(s->sycl_state, qptr);
    if (barrier_err) {
        return barrier_err;
    }
    sycl::queue &q = *qptr;
    const unsigned cur_idx = index % s->ring;
    const motion_sycl_pipeline::SadArgs args = {.prev = s->d_pix[(index + 1u) % s->ring],
                                                .cur = cur,
                                                .cur_copy = s->d_pix[cur_idx],
                                                .sad = s->d_sad,
                                                .width = s->width,
                                                .height = s->height,
                                                .bpc = s->bpc};
    try {
        if (index >= s->ring - 1u) {
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

    /* The CPU's min_idx (integer_motion_v2.c::extract): the first frames
     * have no frame ring - 1 back and report a SAD of 0. */
    if (index < s->ring - 1u) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0.0, index);
    }

    /* The CPU's SAD score (integer_motion_v2.c::extract): normalised, then
     * fps-weighted and capped at motion_max_val. advance() and flush() derive
     * motion2_v2 and motion3_v2 from these stored values, as the CPU does. */
    const double sad_score = (double)*s->h_sad / 256.0 / ((double)s->width * (double)s->height);
    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score",
        MIN(sad_score * s->motion_fps_weight, s->motion_max_val), index);
}

} // namespace

namespace
{

/* The window of this twin's options, on its state: the CPU extractor's
 * (integer_motion.c::vmaf_motion_window_advance() / _flush(), ADR-1478),
 * with the three-frame or the five-frame window. */
static VmafMotionWindow motion_v2_window_of(MotionV2StateSycl *s)
{
    const VmafMotionWindow window = {
        .sad_feature = "VMAF_integer_feature_motion_v2_sad_score",
        .motion2_feature = "VMAF_integer_feature_motion2_v2_score",
        .motion3_feature = "VMAF_integer_feature_motion3_v2_score",
        .motion_blend_factor = s->motion_blend_factor,
        .motion_blend_offset = s->motion_blend_offset,
        .motion_max_val = s->motion_max_val,
        .motion_five_frame_window = s->motion_five_frame_window,
        .motion_moving_average = s->motion_moving_average,
        .state = &s->window_state,
    };
    return window;
}

/* ADR-2090: motion2_v2 / motion3_v2 of the frames whose window the SAD scores
 * collected so far complete. */
static int advance_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);
    /* No frame reached init(): nothing was stored, nothing to derive. */
    if (s->feature_name_dict == nullptr) {
        return 0;
    }
    const VmafMotionWindow window = motion_v2_window_of(s);
    return vmaf_motion_window_advance(feature_collector, s->feature_name_dict, &window);
}

/* motion2_v2 and motion3_v2 of the frames no advance derived, from the stored
 * SAD scores. A one-frame input gets motion2_v2 = motion3_v2 = 0 at index 0,
 * as on the CPU; only an empty run emits nothing. */
static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);

    /* No frame reached init(): nothing was stored, nothing to derive. */
    if (s->feature_name_dict == nullptr) {
        return 1;
    }

    const VmafMotionWindow window = motion_v2_window_of(s);
    const int err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);
    return err ? err : 1;
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);
    if (s->sycl_state) {
        for (void *plane : s->d_pix) {
            if (plane)
                vmaf_sycl_free(s->sycl_state, plane);
        }
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

/* Runs on the zero-copy path (ADR-1688): motion_v2 reads the shared luma only; submit()
 * never reads its picture arguments. */
bool reads_shared_luma_only(const VmafFeatureExtractor * /*fex*/)
{
    return true;
}

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_integer_motion_v2_sycl = {
    .name = "motion_v2_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = flush_fex_sycl,
    .advance = advance_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_motion_v2_sycl,
    .priv_size = sizeof(MotionV2StateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_motion_v2_sycl,
    .reads_shared_luma_only = reads_shared_luma_only,
};
