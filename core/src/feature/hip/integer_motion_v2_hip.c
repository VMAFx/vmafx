/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  motion_v2 feature extractor on the HIP backend -- sixth consumer
 *  of `core/src/hip/kernel_template.h` (T7-10b follow-up /
 *  ADR-0267).  Real kernel promotion: T7-10b batch-4 / ADR-0377.
 *
 *  This TU mirrors `core/src/feature/cuda/integer_motion_v2_cuda.c`
 *  call-graph-for-call-graph. When `HAVE_HIPCC` is defined the real HIP
 *  Module API path is active: the reference luma of the frame from the
 *  context's shared frame (ADR-1408), a kept copy of an earlier frame's
 *  (`prev_luma`, `hipMalloc`: the previous frame, or the frame two back with
 *  motion_five_frame_window, ADR-1491), the diff-first SAD pipeline it
 *  shares with motion_hip (integer_motion_sad_hip.h, ADR-1377), and a
 *  host-side advance() and flush() that derive `motion2_v2` and
 *  `motion3_v2` from the stored SAD scores, each frame once its window is
 *  complete (ADR-2090). Without `HAVE_HIPCC` the scaffold posture is
 *  preserved.
 *
 *  The derivation is the CPU extractor's own, vmaf_motion_window_advance()
 *  and vmaf_motion_window_flush() (motion_window.h, ADR-1478), so the twin's
 *  scores are the CPU's whenever its SADs are. The option surface was
 *  added on the HIP backend in ADR-1108 (cross-backend follow-up closing
 *  the GPU-twin deferral ADR-0337 left open). No GPU work is needed for the
 *  post-process.
 *
 *  Bit-exactness (ADR-0138/0139): the HIP kernel uses arithmetic right
 *  shifts on int32/int64 -- the same as the CPU reference and CUDA twin.
 *  A logical (unsigned) shift would diverge for negative signed values and
 *  was the root cause of the AVX2 srlv_epi64 divergence fixed in PR #587.
 *
 *  Unique vs other HIP consumers: TEMPORAL extractor. An earlier frame's
 *  raw luma is kept in a device plane of its own (`prev_luma`), filled by a
 *  device-to-device copy behind each frame's SAD, because the shared frame
 *  only keeps a frame's planes until the next but one. The template readback
 *  bundle holds only the single int64 SAD accumulator.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "motion_blend_tools.h"
#include "motion_window.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "../../hip/shared_frame.h"
#include "integer_motion_v2_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"
#include "integer_motion_sad_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */
#endif /* HAVE_HIPCC */

/* Default maximum value allowed for motion — mirrors
 * DEFAULT_MOTION_MAX_VAL in integer_motion_v2.c (the CPU reference) and
 * MOTION_V2_CUDA_DEFAULT_MAX_VAL in the CUDA twin. */
#define MOTION_V2_HIP_DEFAULT_MAX_VAL (10000.0)

typedef struct MotionV2StateHip {
    /* Lifecycle (private stream + submit/finished event pair) and the
     * (device int64 SAD accumulator, pinned host readback slot) pair
     * are managed by `hip/kernel_template.h` (T7-10b sixth consumer /
     * ADR-0267). */
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb;
    VmafHipContext *ctx;

#ifdef HAVE_HIPCC
    /* The diff-first SAD kernel motion_hip runs too (ADR-1377). */
    VmafHipMotionSad sad_kernel;
    /* Raw ref Y planes of earlier frames on device, packed, `depth` of them
     * in use. Frame n's SAD reads prev_luma[n % depth] and a device copy
     * behind the SAD replaces it with the frame's own, so the plane holds
     * the frame `depth` back: the previous frame with one plane, the frame
     * two back with two (motion_five_frame_window, ADR-1491). Outside the
     * template's readback bundle (the template models one device+host pair,
     * not a device-only buffer). */
    void *prev_luma[2];
    /* The frame's ref Y plane: the context's shared frame, or `planes`' own
     * buffer when there is none (ADR-1408). */
    VmafHipPlaneSource planes;
#endif /* HAVE_HIPCC */

    size_t plane_bytes;
    unsigned depth; /* frames between a frame and the one its SAD reads: 1 or 2 */
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    double motion_fps_weight;

    /* motion3_v2 post-process options — mirror the CPU reference
     * (integer_motion_v2.c) option table byte-for-byte so a model file
     * carrying `motion_v2_hip=motion_blend_factor=…` loads and scores
     * identically to the CPU and CUDA paths (ADR-1108). */
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    bool motion_five_frame_window;
    bool motion_moving_average;

    VmafDictionary *feature_name_dict;
    /* motion2_v2 / motion3_v2, derived as the SAD scores come in (ADR-2090). */
    VmafMotionWindowState window_state;
} MotionV2StateHip;

/* Option table mirrors integer_motion_v2.c (CPU reference) for the
 * subset of options the HIP twin's host-side motion3_v2 post-process
 * consumes: name / alias / type / default / min / max / flags match
 * byte-for-byte so co-scheduled CPU+HIP runs name features identically
 * and model files load on either path (ADR-1108, matching the CUDA
 * twin). motion_force_zero is a CPU-only knob (the HIP kernel always
 * computes the SAD) and is intentionally omitted from this twin's
 * surface. */
static const VmafOption options[] = {
    {
        .name = "motion_fps_weight",
        .alias = "mfw",
        .help = "fps-aware multiplicative weight/correction",
        .offset = offsetof(MotionV2StateHip, motion_fps_weight),
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
        .offset = offsetof(MotionV2StateHip, motion_blend_factor),
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
        .offset = offsetof(MotionV2StateHip, motion_blend_offset),
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
        .offset = offsetof(MotionV2StateHip, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = MOTION_V2_HIP_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_five_frame_window",
        .alias = "mffw",
        .help = "use five-frame temporal window",
        .offset = offsetof(MotionV2StateHip, motion_five_frame_window),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .alias = "mma",
        .help = "smooth motion3 with a 2-frame moving average",
        .offset = offsetof(MotionV2StateHip, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}};

#ifdef HAVE_HIPCC
/* Allocate the planes that keep the luma of earlier frames. On failure the
 * caller's mv2_hip_release() frees whatever is set. */
static int mv2_hip_bufs_alloc(MotionV2StateHip *s)
{
    for (unsigned i = 0; i < s->depth; i++) {
        if (hipMalloc(&s->prev_luma[i], s->plane_bytes) != hipSuccess)
            return -ENOMEM;
    }
    return 0;
}

/* Free the buffers, let go of the frame's planes and unload the module. Safe
 * with NULL handles; the caller has drained the stream, so no copy or kernel
 * still uses them. */
static void mv2_hip_bufs_free(MotionV2StateHip *s)
{
    vmaf_hip_plane_source_close(&s->planes);
    for (unsigned i = 0; i < 2u; i++) {
        if (s->prev_luma[i] == NULL)
            continue;
        /* Best-effort teardown. */
        const hipError_t rc = hipFree(s->prev_luma[i]);
        (void)rc;
        s->prev_luma[i] = NULL;
    }
    vmaf_hip_motion_sad_unload(&s->sad_kernel);
}

/* Per-frame work. The reference luma comes from the context's shared frame
 * (ADR-1408): uploaded by whichever twin asks first, with the wait that
 * keeps the picture from being recycled under the copy
 * (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18), and reused by the others. The
 * SAD against the frame `depth` back (from frame `depth` on), the copy that
 * keeps this frame's luma and the DtoH copy are enqueued on the private
 * stream; collect() waits for them. */
static int mv2_hip_launch(MotionV2StateHip *s, VmafHipSharedFrame *shared, VmafPicture *ref_pic,
                          unsigned index)
{
    void *cur = NULL;
    int err = vmaf_hip_plane_source_acquire_luma(&s->planes, shared, ref_pic, NULL, s->lc.str, &cur,
                                                 NULL);
    if (err != 0)
        return err;
    const bool have_prev = index >= s->depth;
    const VmafHipMotionSadFrame frame = {.cur = cur,
                                         .keep = s->prev_luma[index % s->depth],
                                         .have_prev = have_prev,
                                         .sad = (uint64_t *)s->rb.device,
                                         .width = s->frame_w,
                                         .height = s->frame_h,
                                         .bpc = s->bpc};
    err = vmaf_hip_motion_sad_submit(&s->sad_kernel, &frame, s->lc.str);
    if (err != 0)
        return err;

    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    hipError_t rc = hipEventRecord(vmaf_hip_event_of(s->lc.submit), str);
    /* No frame `depth` back: nothing to diff against; collect() emits 0. */
    if (rc == hipSuccess && have_prev) {
        rc = hipMemcpyAsync(s->rb.host_pinned, s->rb.device, sizeof(uint64_t),
                            hipMemcpyDeviceToHost, str);
    }
    if (rc != hipSuccess)
        return vmaf_hip_rc_to_errno(rc);
    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}
#endif /* HAVE_HIPCC */

/* Tear down everything init() may have set up. Every step tolerates a handle
 * that was never created, so this serves both a failed init() and close().
 * The stream is drained first, so no kernel still uses a buffer. Returns the
 * first error; freeing the buffers and the module is best-effort. */
static int mv2_hip_release(MotionV2StateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
#ifdef HAVE_HIPCC
    /* mv2_hip_bufs_free also unloads the module. */
    mv2_hip_bufs_free(s);
#endif /* HAVE_HIPCC */
    int err = vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0)
        rc = err;
    if (s->feature_name_dict != NULL) {
        err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0)
            rc = err;
    }
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return rc;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    MotionV2StateHip *s = fex->priv;

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u);
    /* The CPU's min_idx (integer_motion_v2.c::extract): the SAD of frame n
     * is taken against frame n - depth. */
    s->depth = s->motion_five_frame_window ? 2u : 1u;

    /* The 5-tap HIP kernel reflects once (reflect-101); a consumed tap stays
     * in the plane only from 3x3 up, as on the CPU. Refuse smaller frames
     * up front.  Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_v2_hip: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    int err = vmaf_hip_context_new(&s->ctx, fex->hip_device_index);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    /* Readback pair: single int64 SAD accumulator + pinned host slot. */
    if (err == 0)
        err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx, sizeof(uint64_t));
#ifdef HAVE_HIPCC
    if (err == 0)
        err = vmaf_hip_motion_sad_load(&s->sad_kernel);
    if (err == 0)
        err = mv2_hip_bufs_alloc(s);
#endif /* HAVE_HIPCC */
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)mv2_hip_release(s);
    return err;
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    MotionV2StateHip *s = fex->priv;

    /* The geometry stays the one init() sized the buffers for; libvmaf
     * rejects a picture of any other size before it reaches submit(). */
    s->index = index;

#ifdef HAVE_HIPCC
    return mv2_hip_launch(s, fex->hip_frame, ref_pic, index);
#else
    (void)ref_pic;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    MotionV2StateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0) {
        return err;
    }

#ifdef HAVE_HIPCC
    /* No frame `depth` back: no diff was computed -- emit 0. */
    if (index < s->depth) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0.0, index);
    }

    /* SAD sum -> sad / 256.0 / (w*h), stored as the CPU stores it
     * (integer_motion_v2.c::extract): scaled by motion_fps_weight, then capped
     * at motion_max_val. advance() and flush() fold motion2_v2 / motion3_v2
     * from the stored value and do not weight it again. */
    const uint64_t *sad_host = s->rb.host_pinned;
    const double sad_score = (double)*sad_host / 256.0 / ((double)s->frame_w * (double)s->frame_h);
    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score",
        MIN(sad_score * s->motion_fps_weight, s->motion_max_val), index);
#else
    (void)feature_collector;
    (void)index;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

/* The window of this twin's options, on its state: the CPU extractor's
 * (integer_motion.c::vmaf_motion_window_advance() / _flush(), ADR-1478),
 * with the three-frame or the five-frame window. */
static VmafMotionWindow mv2_hip_window_of(MotionV2StateHip *s)
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
static int advance_fex_hip(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionV2StateHip *s = fex->priv;
    /* No frame reached init(): nothing was stored, nothing to derive. */
    if (s->feature_name_dict == NULL)
        return 0;
    const VmafMotionWindow window = mv2_hip_window_of(s);
    return vmaf_motion_window_advance(feature_collector, s->feature_name_dict, &window);
}

/* motion2_v2 and motion3_v2 of the frames no advance derived, from the stored
 * SAD scores. A one-frame run gets motion2_v2 = motion3_v2 = 0, as on the
 * CPU; an empty run nothing. */
static int flush_fex_hip(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)feature_collector;
    return 1;
#else
    MotionV2StateHip *s = fex->priv;

    /* No frame reached init(): nothing was stored, nothing to derive. */
    if (s->feature_name_dict == NULL)
        return 1;

    const VmafMotionWindow window = mv2_hip_window_of(s);
    const int err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);
    return err ? err : 1;
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return mv2_hip_release(fex->priv);
}

static const char *provided_features[] = {"VMAF_integer_feature_motion_v2_sad_score",
                                          "VMAF_integer_feature_motion2_v2_score",
                                          "VMAF_integer_feature_motion3_v2_score", NULL};

/* Load-bearing: the feature extractor is registered via
 * `extern VmafFeatureExtractor vmaf_fex_integer_motion_v2_hip;` in
 * `core/src/feature/feature_extractor.cpp`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern every CUDA / SYCL / Vulkan feature extractor uses (see
 * e.g. `vmaf_fex_integer_motion_v2_cuda` in
 * `core/src/feature/cuda/integer_motion_v2_cuda.c`). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_motion_v2_hip = {
    .name = "motion_v2_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .flush = flush_fex_hip,
    .advance = advance_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(MotionV2StateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
