/**
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  integer_motion feature extractor on the HIP backend.
 *
 *  When `HAVE_HIPCC` is defined the real HIP path is active: a raw-luma
 *  ping-pong (`pix[2]`), a pinned host staging plane and a uint64 SAD
 *  accumulator, the diff-first SAD pipeline it shares with motion_v2_hip
 *  (integer_motion_sad_hip.h), and host-side motion2 / motion3 scoring.
 *  Without `HAVE_HIPCC` the scaffold posture is preserved (-ENOSYS).
 *
 *  Provided features (the CPU `motion` set):
 *    VMAF_integer_feature_motion_sad_score  (every frame, like the CPU)
 *    VMAF_integer_feature_motion_score      (debug only; `debug` defaults
 *                                            to false, as on the CPU)
 *    VMAF_integer_feature_motion2_score
 *    VMAF_integer_feature_motion3_score
 *
 *  Temporal design (ADR-1377): `pix[2]` holds the raw luma of the current
 *  and the previous frame. Frame N stages its luma into `pix[N % 2]` and,
 *  from frame 1 on, the kernel adds sum |blur(prev - cur)| over
 *  `pix[(N + 1) % 2]` and `pix[N % 2]`: the CPU `motion`'s arithmetic
 *  (integer_motion.c::motion_score_pipeline_8/_16), the frames differenced
 *  first and each filter pass rounded. Until ADR-1377 this twin blurred each
 *  frame into a uint16 ping-pong and differenced the blurred frames, which
 *  rounds differently (T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29).
 *
 *  One host wait per frame, in collect(). submit() copies the picture's luma
 *  into the pinned staging plane on the host and enqueues the device copy,
 *  the SAD and its read-back on the private stream without waiting
 *  (vmaf_hip_picture_upload_staged(), T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19).
 *  libvmaf collects frame N - 1 before it submits frame N, so the staging
 *  plane and the ping-pong slot a submit overwrites are idle by then.
 *
 *  ADR-0530: VMAF_FEATURE_EXTRACTOR_HIP IS now set so that
 *  `compute_fex_flags()` actually selects this extractor when a HIP
 *  state has been imported via `vmaf_hip_import_state()`. Without the
 *  flag the model-driven (`vmaf_use_features_from_model`) dispatch
 *  would skip past this extractor and pick the CPU twin, which is what
 *  ADR-0519 deliberately left in place for the import-state PR.
 *
 *  HIP adaptation notes vs CUDA twin:
 *  - Kernel args are raw pointers (no VmafCudaBuffer indirection).
 *  - `pix[0]` and `pix[1]` are plain hipMalloc device buffers (w*h samples).
 *  - SAD accumulator is a plain uint64_t hipMalloc device buffer.
 *  - motion3 post-processing and motion_blend helpers are host-only scalar
 *    work, identical to the CUDA twin.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "motion_blend_tools.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"

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

/* Default upper clamp on motion/motion2/motion3 — mirrors
 * MOTION_CUDA_DEFAULT_MAX_VAL in integer_motion_cuda.c / ADR-0219. */
#define MOTION_HIP_DEFAULT_MAX_VAL (10000.0)

typedef struct MotionStateHip {
    /* Lifecycle (private stream + submit/finished event pair) and the
     * (device uint64 SAD accumulator, pinned host readback slot) pair
     * are managed by `hip/kernel_template.h`. */
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb;
    VmafHipContext *ctx;

#ifdef HAVE_HIPCC
    VmafHipMotionSad sad_kernel;
    /* Raw ref Y planes on device (uint8 or uint16, w*h each).
     * pix[index % 2] receives the current frame; pix[(index + 1) % 2]
     * still holds the previous one. */
    void *pix[2];
    /* Pinned host copy of the current frame's luma: the device copy's
     * source, so submit() never waits for the copy to read the picture. */
    void *staging;
#endif /* HAVE_HIPCC */

    size_t plane_bytes; /* bytes for one Y plane (bpc-aware) */
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;

    double score;                /* most recent motion_score (raw, normalized) */
    double prev_motion3_blended; /* for motion3 moving-average carry */
    unsigned frame_index;        /* count of frames processed */

    bool debug;
    bool motion_force_zero;
    bool motion_five_frame_window;
    bool motion_add_uv; /* rejected with -ENOTSUP — see init(); ADR-0989 */
    bool motion_moving_average;
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_fps_weight;
    double motion_max_val;

    VmafDictionary *feature_name_dict;
} MotionStateHip;

/* Compact layout: clang-format would put every field on its own line and push
 * the table past the 60-line HISS-04 function-size limit. */
// clang-format off
static const VmafOption options[] = {
    {.name = "debug", .help = "debug mode: enable additional output",
     .offset = offsetof(MotionStateHip, debug), .type = VMAF_OPT_TYPE_BOOL, .default_val.b = false},
    {.name = "motion_force_zero", .alias = "force_0", .help = "forcing motion score to zero",
     .offset = offsetof(MotionStateHip, motion_force_zero), .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "motion_blend_factor", .alias = "mbf", .help = "blend motion score given an offset",
     .offset = offsetof(MotionStateHip, motion_blend_factor), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 1.0, .min = 0.0, .max = 1.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "motion_blend_offset", .alias = "mbo",
     .help = "blend motion score starting from this offset",
     .offset = offsetof(MotionStateHip, motion_blend_offset), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 40.0, .min = 0.0, .max = 1000.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "motion_fps_weight", .alias = "mfw", .help = "fps-aware multiplicative weight/correction",
     .offset = offsetof(MotionStateHip, motion_fps_weight), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 1.0, .min = 0.0, .max = 5.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "motion_max_val", .alias = "mmxv",
     .help = "maximum value allowed; larger values will be clipped to this value",
     .offset = offsetof(MotionStateHip, motion_max_val), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = MOTION_HIP_DEFAULT_MAX_VAL, .min = 0.0, .max = 10000.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "motion_five_frame_window", .alias = "mffw",
     .help = "use five-frame temporal window (NOT YET SUPPORTED on HIP — deferred)",
     .offset = offsetof(MotionStateHip, motion_five_frame_window), .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "motion_moving_average", .alias = "mma",
     .help = "use moving average for motion3 scores after first frame",
     .offset = offsetof(MotionStateHip, motion_moving_average), .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "motion_add_uv", .alias = "mau",
     .help = "include U and V plane SADs (NOT YET SUPPORTED on HIP — ADR-0989 deferred)",
     .offset = offsetof(MotionStateHip, motion_add_uv), .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0}};
// clang-format on

#ifdef HAVE_HIPCC
/* ------------------------------------------------------------------ */
/* motion3 host post-processing — mirrors integer_motion_cuda.c.      */
/* ------------------------------------------------------------------ */
static double motion3_postprocess_hip(MotionStateHip *s, double score2)
{
    /* ``score2`` already carries ``motion_fps_weight`` and the
     * ``motion_max_val`` clip: every caller applies both before handing the
     * value over, exactly as the CPU reference does once in extract()
     * (integer_motion.c:372).  Re-weighting here would square the factor
     * whenever ``motion_fps_weight != 1.0``.  ADR-1216. */
    const double blended = motion_blend(score2, s->motion_blend_factor, s->motion_blend_offset);
    const double clipped = blended < s->motion_max_val ? blended : s->motion_max_val;
    const double prev_una = s->prev_motion3_blended;
    s->prev_motion3_blended = clipped;
    /* frame_index is pre-incremented in collect() before this runs.
     * Guard matches the CUDA twin: frame_index > 2 corresponds to
     * CPU's index > 1 (cuda-reviewer 2026-05-09). */
    if (s->motion_moving_average && s->frame_index > 2u) {
        return (clipped + prev_una) / 2.0;
    }
    return clipped;
}

/* The CPU's per-frame score (integer_motion.c::extract): the SAD score
 * scaled by motion_fps_weight, then capped at motion_max_val. Every score
 * this twin emits goes through it, the debug motion score included. */
static double motion_clip_hip(const MotionStateHip *s, double score)
{
    const double weighted = score * s->motion_fps_weight;
    return weighted < s->motion_max_val ? weighted : s->motion_max_val;
}

static double normalize_and_scale_sad(uint64_t sad, unsigned w, unsigned h)
{
    return (double)(sad / 256.) / ((double)w * (double)h);
}

/* Idempotent append (dict-aware) — suppresses duplicate-write warning
 * when flush's collect already wrote the same (feature, index) pair.
 * ADR-0530: routes through the feature_name_dict so the encoded key
 * (`vmaf_feature_name_from_options`) matches what the predict layer
 * looks up via `model->predict_feature_names[i]`. Without the dict
 * the literal key would be written and the predict step would emit
 * "no feature 'VMAF_integer_feature_motion2_score' at index N". */
static int append_if_unwritten(VmafFeatureCollector *fc, VmafDictionary *dict, const char *feature,
                               double value, unsigned index)
{
    /* Resolve through the dict so the get-side query uses the same
     * encoded key the append-side write produces. */
    VmafDictionaryEntry *entry = vmaf_dictionary_get(&dict, feature, 0);
    const char *fn = entry ? entry->val : feature;
    double existing;
    if (vmaf_feature_collector_get_score(fc, fn, &existing, index) == 0)
        return 0;
    return vmaf_feature_collector_append(fc, fn, value, index);
}
#endif /* HAVE_HIPCC */

static int extract_force_zero(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                              VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                              VmafPicture *dist_pic_90, unsigned index,
                              VmafFeatureCollector *feature_collector)
{
    MotionStateHip *s = fex->priv;
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;

    int err =
        vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                "VMAF_integer_feature_motion_sad_score", 0., index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion2_score", 0., index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion3_score", 0., index);
    if (!s->debug)
        return err;
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion_score", 0., index);
    return err;
}

/* The asynchronous half of motion_force_zero (msh_init_force_zero()):
 * submit() has no picture to read, collect() writes extract()'s zeros. */
static int submit_force_zero(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                             VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                             VmafPicture *dist_pic_90, unsigned index)
{
    (void)fex;
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    (void)index;
    return 0;
}

static int collect_force_zero(VmafFeatureExtractor *fex, unsigned index,
                              VmafFeatureCollector *feature_collector)
{
    return extract_force_zero(fex, NULL, NULL, NULL, NULL, index, feature_collector);
}

#ifdef HAVE_HIPCC

/* Allocate the raw-luma ping-pong and the pinned staging plane. On failure
 * the buffers already allocated stay set; the caller's msh_release() frees
 * them. */
static int msh_bufs_alloc(MotionStateHip *s)
{
    hipError_t rc = hipMalloc(&s->pix[0], s->plane_bytes);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->pix[1], s->plane_bytes);
    if (rc != hipSuccess)
        return -ENOMEM;
    return vmaf_hip_picture_staging_alloc(&s->staging, s->plane_bytes);
}

/* Free the buffers and unload the module. Safe with NULL; the caller has
 * drained the stream, so no copy or kernel still uses them. */
static void msh_bufs_free(MotionStateHip *s)
{
    vmaf_hip_picture_staging_free(s->staging);
    s->staging = NULL;
    for (unsigned i = 0; i < 2u; i++) {
        if (s->pix[i] != NULL) {
            /* Best-effort teardown. */
            const hipError_t rc = hipFree(s->pix[i]);
            (void)rc;
            s->pix[i] = NULL;
        }
    }
    vmaf_hip_motion_sad_unload(&s->sad_kernel);
}

/* Per-frame work, all enqueued on the private stream: stage the luma into
 * pix[index % 2]; from frame 1 on, the SAD against pix[(index + 1) % 2] and
 * its DtoH copy. collect() is the only wait. */
static int msh_launch(MotionStateHip *s, VmafPicture *ref_pic, unsigned index)
{
    const VmafHipMotionSadFrame frame = {.pic = ref_pic,
                                         .staging = s->staging,
                                         .staging_bytes = s->plane_bytes,
                                         .cur = s->pix[index % 2u],
                                         .prev = (index > 0u) ? s->pix[(index + 1u) % 2u] : NULL,
                                         .sad = (uint64_t *)s->rb.device,
                                         .width = s->frame_w,
                                         .height = s->frame_h,
                                         .bpc = s->bpc};
    const int err = vmaf_hip_motion_sad_submit(&s->sad_kernel, &frame, s->lc.str);
    if (err != 0)
        return err;

    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    hipError_t rc = hipEventRecord(vmaf_hip_event_of(s->lc.submit), str);
    /* Frame 0 has no previous frame and no SAD to read back. */
    if (rc == hipSuccess && index > 0u) {
        rc = hipMemcpyAsync(s->rb.host_pinned, s->rb.device, sizeof(uint64_t),
                            hipMemcpyDeviceToHost, str);
    }
    if (rc != hipSuccess)
        return vmaf_hip_rc_to_errno(rc);
    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

#endif /* HAVE_HIPCC */

/* Reject the options and frame sizes this twin does not support. */
static int msh_check_config(const MotionStateHip *s, unsigned w, unsigned h)
{
    /* Reject 5-frame window: same as CUDA twin (ADR-0219). */
    if (s->motion_five_frame_window) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "motion_hip: motion_five_frame_window=true is not yet supported on HIP "
                 "(T3-15(c) deferred). Use the CPU extractor `motion` instead.\n");
        return -ENOTSUP;
    }

    /* motion_add_uv kernel port deferred — ADR-0989. SYCL is the lead
     * backend; HIP will follow in a subsequent PR. */
    if (s->motion_add_uv) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "motion_hip: motion_add_uv=true is not yet supported on HIP "
                 "(ADR-0989 deferred). Use the SYCL extractor `motion_sycl` instead.\n");
        return -ENOTSUP;
    }

    /* The 5-tap kernel reflects once (reflect-101); a consumed tap stays in
     * the plane only from 3x3 up, as on the CPU. Refuse smaller frames. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_hip: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }
    return 0;
}

/* Tear down the device objects init() may have set up. Every step tolerates
 * a handle that was never created, so this serves a failed init(), close()
 * (through msh_release()) and the motion_force_zero switch, which keeps the
 * name dictionary. The stream is drained first, so no copy or kernel still
 * uses a buffer. Returns the first error; freeing the buffers and the module
 * is best-effort. */
static int msh_release_device(MotionStateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
#ifdef HAVE_HIPCC
    /* msh_bufs_free also unloads the module. */
    msh_bufs_free(s);
#endif
    const int err_rb = vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
    if (err_rb != 0 && rc == 0)
        rc = err_rb;
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return rc;
}

/* msh_release_device() plus the name dictionary: a failed init() and close(). */
static int msh_release(MotionStateHip *s)
{
    int rc = msh_release_device(s);
    if (s->feature_name_dict != NULL) {
        const int err_dict = vmaf_dictionary_free(&s->feature_name_dict);
        if (err_dict != 0 && rc == 0)
            rc = err_dict;
    }
    return rc;
}

/* motion_force_zero writes zeros: from extract() for a direct caller and from
 * collect() under libvmaf's asynchronous dispatch, which picks submit() /
 * collect() from the callbacks of the uninitialised context
 * (read_pictures_dispatch_one()) and runs init() only inside
 * vmaf_feature_extractor_context_submit(). Clearing submit here had the
 * framework call a NULL submit() on the first frame
 * (T-HIP-MOTION-FORCE-ZERO-NULL-SUBMIT-2026-09-30). The device objects go
 * now; close() stays and frees the name dictionary extract_force_zero()
 * writes through. */
static int msh_init_force_zero(VmafFeatureExtractor *fex, MotionStateHip *s)
{
    fex->extract = extract_force_zero;
    fex->submit = submit_force_zero;
    fex->collect = collect_force_zero;
    fex->flush = NULL;
    return msh_release_device(s);
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    MotionStateHip *s = fex->priv;

    int err = msh_check_config(s, w, h);
    if (err != 0)
        return err;

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u);
    s->score = 0.0;
    s->frame_index = 0;
    s->prev_motion3_blended = 0.0;

    err = vmaf_hip_context_new(&s->ctx, 0);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    /* Readback pair: single uint64_t SAD accumulator + pinned host slot. */
    if (err == 0)
        err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx, sizeof(uint64_t));
#ifdef HAVE_HIPCC
    if (err == 0)
        err = vmaf_hip_motion_sad_load(&s->sad_kernel);
    if (err == 0)
        err = msh_bufs_alloc(s);
#endif /* HAVE_HIPCC */

    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err == 0 && s->motion_force_zero)
        err = msh_init_force_zero(fex, s);
    if (err != 0)
        (void)msh_release(s);
    return err;
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    MotionStateHip *s = fex->priv;

    /* The geometry stays the one init() sized the buffers for; libvmaf
     * rejects a picture of any other size before it reaches submit(). */
    s->index = index;

#ifdef HAVE_HIPCC
    return msh_launch(s, ref_pic, index);
#else
    (void)ref_pic;
    return -ENOSYS;
#endif
}

#ifdef HAVE_HIPCC
/* motion2 / motion3 of frame index - 1, now that frame index's SAD is known.
 * Mirrors integer_motion_cuda.c collect logic exactly. */
static int msh_emit_prev_frame(MotionStateHip *s, VmafFeatureCollector *feature_collector,
                               unsigned index, double score_prev)
{
    int e = 0;
    if (index == 1u) {
        const double motion3_score = motion3_postprocess_hip(s, motion_clip_hip(s, s->score));
        e |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                     "VMAF_integer_feature_motion3_score",
                                                     motion3_score, index - 1u);
    }

    if (index > 1u) {
        const double motion2_raw = score_prev < s->score ? score_prev : s->score;
        const double motion2_clipped = motion_clip_hip(s, motion2_raw);
        e |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                     "VMAF_integer_feature_motion2_score",
                                                     motion2_clipped, index - 1u);
        const double motion3_score = motion3_postprocess_hip(s, motion2_clipped);
        e |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                     "VMAF_integer_feature_motion3_score",
                                                     motion3_score, index - 1u);
    }
    return e;
}

/* Frame 0: no previous frame, so motion2 and the debug score are 0. */
static int msh_emit_first_frame(MotionStateHip *s, VmafFeatureCollector *feature_collector)
{
    /* ADR-0530: route writes through s->feature_name_dict so the collector
     * keys match what `vmaf_predict_score_at_index` looks up (encoded
     * option-aware key, not the literal). */
    int e = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                    "VMAF_integer_feature_motion_sad_score", 0., 0);
    e |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                 "VMAF_integer_feature_motion2_score", 0., 0);
    if (s->debug) {
        e |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                     "VMAF_integer_feature_motion_score", 0., 0);
    }
    s->frame_index++;
    return e;
}
#endif /* HAVE_HIPCC */

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    MotionStateHip *s = fex->priv;

    /* The one host wait of the frame: the staged copy, the SAD and its
     * read-back all ran on this stream. */
    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

#ifndef HAVE_HIPCC
    (void)feature_collector;
    (void)index;
    return -ENOSYS;
#else
    if (index == 0u)
        return msh_emit_first_frame(s, feature_collector);

    double score_prev = s->score;
    const uint64_t *sad_host = (const uint64_t *)s->rb.host_pinned;
    s->score = normalize_and_scale_sad(*sad_host, s->frame_w, s->frame_h);
    s->frame_index++;

    /* The CPU's per-frame score (integer_motion.c::extract), stored as
     * motion_sad_score on every frame and repeated as the debug motion
     * score: the SAD score weighted and capped (motion_clip_hip()). */
    int e = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                    "VMAF_integer_feature_motion_sad_score",
                                                    motion_clip_hip(s, s->score), index);
    if (s->debug) {
        e |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                     "VMAF_integer_feature_motion_score",
                                                     motion_clip_hip(s, s->score), index);
    }
    e |= msh_emit_prev_frame(s, feature_collector, index, score_prev);
    return e;
#endif /* HAVE_HIPCC */
}

#ifdef HAVE_HIPCC
/* Scores of the last frame, which no later frame completes: motion2 is its
 * own score, as on the CPU. A one-frame run gets motion3[0] = 0, the CPU's
 * stamp value when there is no second frame. */
static int msh_flush_tail(MotionStateHip *s, VmafFeatureCollector *feature_collector)
{
    if (s->index == 0u) {
        return append_if_unwritten(feature_collector, s->feature_name_dict,
                                   "VMAF_integer_feature_motion3_score", 0., 0u);
    }
    const double last_motion2 = motion_clip_hip(s, s->score);
    int err = append_if_unwritten(feature_collector, s->feature_name_dict,
                                  "VMAF_integer_feature_motion2_score", last_motion2, s->index);
    if (err >= 0) {
        const double motion3_score = motion3_postprocess_hip(s, last_motion2);
        const int e3 =
            append_if_unwritten(feature_collector, s->feature_name_dict,
                                "VMAF_integer_feature_motion3_score", motion3_score, s->index);
        if (e3 < 0)
            err = e3;
    }
    return err;
}
#endif /* HAVE_HIPCC */

static int flush_fex_hip(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)feature_collector;
    return 1;
#else
    MotionStateHip *s = fex->priv;
    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;
    if (s->frame_index == 0u)
        return 1;
    err = msh_flush_tail(s, feature_collector);
    return (err < 0) ? err : 1;
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return msh_release(fex->priv);
}

static const char *provided_features[] = {
    "VMAF_integer_feature_motion_sad_score", "VMAF_integer_feature_motion_score",
    "VMAF_integer_feature_motion2_score", "VMAF_integer_feature_motion3_score", NULL};

/*
 * Load-bearing: registered via `extern VmafFeatureExtractor
 * vmaf_fex_integer_motion_hip;` in `feature_extractor.c`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern as every CUDA / SYCL / Vulkan / Metal feature extractor
 * (e.g. `vmaf_fex_integer_motion_cuda` in
 * `feature/cuda/integer_motion_cuda.c`).
 */
// NOLINTNEXTLINE(misc-use-internal-linkage) -- ADR-0278 registry pattern
VmafFeatureExtractor vmaf_fex_integer_motion_hip = {
    .name = "motion_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .flush = flush_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(MotionStateHip),
    .provided_features = provided_features,
    /* TEMPORAL flag is mandatory: motion needs the previous-frame
     * carry, so the feature engine drives collect before the next submit.
     * Mirrors the CUDA twin verbatim.
     *
     * ADR-0530: VMAF_FEATURE_EXTRACTOR_HIP is now set so the
     * model-driven dispatch (`compute_fex_flags()` in libvmaf.c)
     * actually selects this extractor when a HIP state is imported.
     * Pictures still arrive as VMAF_PICTURE_BUFFER_TYPE_HOST and
     * msh_launch() stages them itself; the dispatch check in
     * `feature_extractor.c` allows HOST buffers for HIP-flagged
     * extractors per the same ADR. */
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
