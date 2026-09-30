/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  motion_v2 feature kernel on the CUDA backend (T7-23 / batch 3
 *  part 1b — ADR-0192 / ADR-0193). CUDA twin of motion_v2_vulkan
 *  (PR #146).
 *
 *  Each frame computes its score in one kernel launch over
 *  (prev_ref - cur_ref), blurring the difference with the CPU's
 *  rounding, without storing blurred frames across submits. A
 *  raw-pixel ping-pong (`pix[2]`) caches the current ref Y plane with
 *  one D2D copy per submit so the next frame can read it as "prev".
 *  The kernel, its launch and the copy are the shared motion SAD
 *  pipeline (integer_motion_sad_cuda.h), which motion_cuda runs too
 *  (ADR-1372).
 *
 *  collect() publishes the CPU's motion_v2_sad_score: the normalised SAD,
 *  fps-weighted and capped at motion_max_val (integer_motion_v2.c::extract).
 *  motion2_v2_score = min(score[i], score[i+1]) of those stored scores and
 *  motion3_v2_score (per-frame blend + clip + optional moving-average) are
 *  emitted host-side in flush(), with the CPU flush's formula and its 0 / 0
 *  for a one-frame input (ADR-1373). No GPU work is needed for the
 *  post-process. The motion3_v2 post-process and its option surface were
 *  added in ADR-1108 (closing the GPU-twin deferral ADR-0337 left open).
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "common.h"
#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"

#include "cuda/integer_motion_sad_cuda.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "motion_blend_tools.h"
#include "picture.h"
#include "picture_cuda.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Default maximum value allowed for motion — mirrors
 * DEFAULT_MOTION_MAX_VAL in integer_motion_v2.c (the CPU reference). */
#define MOTION_V2_CUDA_DEFAULT_MAX_VAL (10000.0)

typedef struct MotionV2StateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). */
    VmafCudaKernelLifecycle lc;
    /* Single int64 atomic accumulator per frame: device + pinned
     * host. Owned by the template's readback bundle. */
    VmafCudaKernelReadback rb;

    /* The shared motion SAD module and kernels (ADR-1372), unloaded in
     * close so the PTX backing store does not leak per vmaf_close(). */
    MotionSadCuda sad;

    /* Ping-pong of raw ref Y planes (uint8 for bpc<=8, uint16 for
     * bpc>8 — bytes_per_pixel * w * h). pix[index%2] is the current
     * frame's slot; pix[(index+1)%2] is the previous frame's slot.
     * Kept outside the template's readback bundle because the
     * template models a single device+host pair, not a ping-pong
     * of device-only buffers. */
    VmafCudaBuffer *pix[2];

    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    size_t plane_bytes;
    double motion_fps_weight;

    /* motion3_v2 post-process options — mirror the CPU reference
     * (integer_motion_v2.c) option table byte-for-byte so a model
     * file carrying `motion_v2_cuda=motion_blend_factor=…` loads
     * and scores identically to the CPU path (ADR-1108). */
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    bool motion_moving_average;

    VmafDictionary *feature_name_dict;
} MotionV2StateCuda;

/* Option table mirrors integer_motion_v2.c (CPU reference) for the
 * subset of options the CUDA twin's host-side motion3_v2 post-process
 * consumes: name / alias / type / default / min / max / flags match
 * byte-for-byte so co-scheduled CPU+CUDA runs name features identically
 * and model files load on either path (ADR-1108). motion_force_zero and
 * motion_five_frame_window are CPU-only knobs (the CUDA kernel always
 * computes the SAD, and the 5-frame window is unsupported per ADR-0337);
 * they are intentionally omitted from this twin's surface. */
static const VmafOption options[] = {
    {
        .name = "motion_fps_weight",
        .alias = "mfw",
        .help = "fps-aware multiplicative weight/correction",
        .offset = offsetof(MotionV2StateCuda, motion_fps_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_factor",
        .alias = "mbf",
        .help = "blend motion score given an offset",
        .offset = offsetof(MotionV2StateCuda, motion_blend_factor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_offset",
        .alias = "mbo",
        .help = "blend motion score starting from this offset",
        .offset = offsetof(MotionV2StateCuda, motion_blend_offset),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 40.0,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .alias = "mmxv",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .offset = offsetof(MotionV2StateCuda, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = MOTION_V2_CUDA_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .alias = "mma",
        .help = "smooth motion3 with a 2-frame moving average",
        .offset = offsetof(MotionV2StateCuda, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}};

/* ------------------------------------------------------------------ */
/* motion_v2_init_unwind - the single teardown path for init_fex_cuda.
 *
 * HISS-01: lifted verbatim from the former `free_buffers` label. The same
 * resources are released in the same order on every exit path, and the
 * value returned is the one the label returned.
 */
static int motion_v2_init_unwind(VmafFeatureExtractor *fex, MotionV2StateCuda *s, int ret)
{
    int rc = ret;
    const int phase_rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (phase_rc)
        return rc ? rc : phase_rc;

    int e = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->pix[0]);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->pix[1]);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    if (e && !rc)
        rc = e;
    e = vmaf_dictionary_free(&s->feature_name_dict);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_motion_sad_unload(fex->cu_state, &s->sad);
    if (e && !rc)
        rc = e;
    return rc;
}

/* motion_v2_alloc_buffers - the two pixel planes, the readback slot, the dict.
 *
 * HISS-04: the allocation tail of init_fex_cuda, moved whole. Every failure
 * routes through motion_v2_init_unwind with the exact allocation error.
 */
static int motion_v2_alloc_buffers(VmafFeatureExtractor *fex, MotionV2StateCuda *s)
{
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->pix[0], s->plane_bytes);
    if (ret)
        return motion_v2_init_unwind(fex, s, ret);
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->pix[1], s->plane_bytes);
    if (ret)
        return motion_v2_init_unwind(fex, s, ret);

    ret = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state, sizeof(uint64_t));
    if (ret)
        return motion_v2_init_unwind(fex, s, ret);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        ret = -ENOMEM;
        return motion_v2_init_unwind(fex, s, ret);
    }

    return 0;
}

/* motion_v2_check_frame_size - refuse frames below the 5-tap minimum.
 *
 * HISS-04: the entry guard of init_fex_cuda, moved whole - same condition,
 * same message, same -EINVAL.
 */
static int motion_v2_check_frame_size(unsigned w, unsigned h)
{
    /* The 5-tap CUDA motion_v2 kernel uses reflect-101 mirror padding;
     * mirror() returns 2*sup - idx - 2, which is negative when sup < 3.
     * Refuse smaller frames up front to prevent out-of-bounds device reads.
     * Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_v2_cuda: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }
    return 0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    MotionV2StateCuda *s = fex->priv;

    const int size_err = motion_v2_check_frame_size(w, h);
    if (size_err)
        return size_err;

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->plane_bytes = vmaf_cuda_motion_sad_plane_bytes(w, h, bpc);

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return motion_v2_init_unwind(fex, s, err);

    err = vmaf_cuda_motion_sad_load(fex->cu_state, &s->sad);
    if (err)
        return motion_v2_init_unwind(fex, s, err);

    return motion_v2_alloc_buffers(fex, s);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    MotionV2StateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);

    /* pix[index % 2] caches this frame's luma for the next frame's `prev`.
     * Every frame after the first also zeroes the accumulator and fills it
     * on pic_stream, ahead of the D2H copy on lc.str that waits for it
     * below. The previous frame's lc.submit orders both ping-pong slots on
     * the device (integer_motion_sad_cuda.h). */
    const bool has_prev = index > 0u;
    const MotionSadFrame frame = {
        .pic = ref_pic,
        .cur = s->pix[index % 2u]->data,
        .prev = has_prev ? s->pix[(index + 1u) % 2u]->data : 0,
        .sad = s->rb.device->data,
        .prev_done = has_prev ? s->lc.submit : NULL,
        .width = s->frame_w,
        .height = s->frame_h,
        .bpc = s->bpc,
    };
    const int launch_err = vmaf_cuda_motion_sad_submit(&s->sad, cu_f, pic_stream, &frame);
    if (launch_err)
        return launch_err;

    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));

    /* Frame 0: nothing more to do — emit 0 in collect. */
    if (!has_prev)
        return 0;

    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rb.host_pinned, (CUdeviceptr)s->rb.device->data,
                                              s->rb.bytes, s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    MotionV2StateCuda *s = fex->priv;

    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;

    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0.0, index);
    }

    /* The CPU's SAD score (integer_motion_v2.c::extract): normalised, then
     * fps-weighted and capped at motion_max_val. flush() derives motion2_v2
     * and motion3_v2 from these stored values, as the CPU does. */
    const uint64_t *sad_host = s->rb.host_pinned;
    const double sad_score = (double)*sad_host / 256.0 / ((double)s->frame_w * s->frame_h);
    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score",
        MIN(sad_score * s->motion_fps_weight, s->motion_max_val), index);
}

/* motion_v2_stamp_value - the motion3_v2 seed emitted for indices < min_idx.
 *
 * HISS-04: the seeding block of flush_fex_cuda, moved whole. The
 * MIN(motion_blend(...), motion_max_val) expression is copied character for
 * character and stays one statement, so no operand crosses a call boundary and
 * the compiler contracts it exactly as it did inline.
 */
static double motion_v2_stamp_value(const MotionV2StateCuda *s,
                                    VmafFeatureCollector *feature_collector, const char *sad_name,
                                    unsigned n_frames, unsigned min_idx)
{
    double stamp_value = 0.;
    if (n_frames > min_idx) {
        double sad_at_min_idx;
        if (!vmaf_feature_collector_get_score(feature_collector, sad_name, &sad_at_min_idx,
                                              min_idx)) {
            stamp_value =
                MIN(motion_blend(sad_at_min_idx, s->motion_blend_factor, s->motion_blend_offset),
                    s->motion_max_val);
        }
    }
    return stamp_value;
}

/* motion_v2_emit_frame - emit motion2_v2 and motion3_v2 for one frame index.
 *
 * The body of CPU integer_motion_v2.c::flush's loop: the stored SAD scores
 * already carry motion_fps_weight and the motion_max_val cap (collect), so
 * motion2_v2 is their plain minimum and motion3_v2 blends it. The moving
 * average reads the previous `processed` before overwriting it;
 * `prev_processed` is the loop-carried accumulator, passed by pointer.
 */
static int motion_v2_emit_frame(MotionV2StateCuda *s, VmafFeatureCollector *feature_collector,
                                const char *sad_name, unsigned i, unsigned n_frames,
                                unsigned min_idx, double stamp_value, double *prev_processed)
{
    double score_cur;
    double score_next;
    vmaf_feature_collector_get_score(feature_collector, sad_name, &score_cur, i);

    double motion2;
    if (i + 1 < n_frames) {
        vmaf_feature_collector_get_score(feature_collector, sad_name, &score_next, i + 1);
        motion2 = score_cur < score_next ? score_cur : score_next;
    } else {
        motion2 = score_cur;
    }

    int append_err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion2_v2_score", motion2,
        i);
    if (append_err)
        return append_err;

    /* motion3_v2_score: per-frame blend + clip + optional moving-average.
     * Mirrors integer_motion_v2.c::flush lines 466-481 byte-for-byte. */
    double motion3;
    if (i < min_idx) {
        motion3 = stamp_value;
        *prev_processed = stamp_value;
    } else {
        double processed =
            MIN(motion_blend(motion2, s->motion_blend_factor, s->motion_blend_offset),
                s->motion_max_val);
        motion3 = s->motion_moving_average ? (processed + *prev_processed) / 2.0 : processed;
        *prev_processed = processed;
    }

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion3_v2_score", motion3,
                                                   i);
}

static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionV2StateCuda *s = fex->priv;

    /* Resolve the (possibly renamed, for sfr/hfr co-schedule) SAD feature
     * name from the dict — mirrors integer_motion_v2.c::flush. */
    VmafDictionaryEntry *e_sad =
        vmaf_dictionary_get(&s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score", 0);
    const char *sad_name = e_sad ? e_sad->val : "VMAF_integer_feature_motion_v2_sad_score";

    unsigned n_frames = 0;
    double dummy;
    while (!vmaf_feature_collector_get_score(feature_collector, sad_name, &dummy, n_frames))
        n_frames++;

    /* A one-frame input still gets motion2_v2 = motion3_v2 = 0 at index 0,
     * as the CPU flush emits them. */
    if (n_frames == 0)
        return 1;

    /* motion3_v2 seeding — mirrors integer_motion_v2.c::flush exactly.
     * 3-frame mode only (min_idx = 1; the 5-frame window is unsupported
     * on motion_v2, ADR-0337). stamp_value blends the stored (weighted and
     * capped) SAD at min_idx, clipped to motion_max_val; it is emitted for
     * all indices i < min_idx. */
    const unsigned min_idx = 1;
    const double stamp_value =
        motion_v2_stamp_value(s, feature_collector, sad_name, n_frames, min_idx);

    double prev_processed = 0.;
    for (unsigned i = 0; i < n_frames; i++) {
        const int emit_err = motion_v2_emit_frame(s, feature_collector, sad_name, i, n_frames,
                                                  min_idx, stamp_value, &prev_processed);
        if (emit_err)
            return emit_err;
    }

    return 1;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    MotionV2StateCuda *s = fex->priv;
    return motion_v2_init_unwind(fex, s, 0);
}

static const char *provided_features[] = {"VMAF_integer_feature_motion_v2_sad_score",
                                          "VMAF_integer_feature_motion2_v2_score",
                                          "VMAF_integer_feature_motion3_v2_score", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_integer_motion_v2_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_motion_v2_cuda = {
    .name = "motion_v2_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .flush = flush_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(MotionV2StateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_CUDA,
};

/* NOLINTEND(modernize-use-nullptr) */
