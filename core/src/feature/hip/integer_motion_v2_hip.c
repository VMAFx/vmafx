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
 *  Module API path is active: module load, raw-pixel ping-pong (`pix[2]`)
 *  via `hipMalloc`, per-frame HtoD copy + `hipModuleLaunchKernel`, and a
 *  host-side flush() computing both `motion2_v2 = min(cur, next)` and
 *  `motion3_v2` (per-frame blend + clip + optional moving-average).
 *  Without `HAVE_HIPCC` the scaffold posture is preserved.
 *
 *  The motion3_v2 post-process and its four-option surface mirror the
 *  CUDA twin (PR #909) and the CPU reference integer_motion_v2.c::flush
 *  byte-for-byte, bit-exact at default options. They were added on the
 *  HIP backend in ADR-1108 (cross-backend follow-up closing the GPU-twin
 *  deferral ADR-0337 left open). No GPU work is needed for the
 *  post-process; it reuses the shared motion_blend_tools.h helper.
 *
 *  Bit-exactness (ADR-0138/0139): the HIP kernel uses arithmetic right
 *  shifts on int32/int64 -- the same as the CPU reference and CUDA twin.
 *  A logical (unsigned) shift would diverge for negative signed values and
 *  was the root cause of the AVX2 srlv_epi64 divergence fixed in PR #587.
 *
 *  Unique vs other HIP consumers: TEMPORAL extractor with a raw-pixel
 *  ping-pong (`pix[2]`) stored as plain `void *` device pointers, mirroring
 *  the CUDA twin's `VmafCudaBuffer *pix[2]` allocation strategy. The template
 *  readback bundle holds only the single int64 SAD accumulator.
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

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "integer_motion_v2_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"

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
    hipModule_t module;
    hipFunction_t funcbpc8;
    hipFunction_t funcbpc16;
    /* Ping-pong of raw ref Y planes on device. pix[index%2] is the current
     * frame's slot; pix[(index+1)%2] is the previous frame's slot.
     * Outside the template's readback bundle (template models one device+host
     * pair, not a ping-pong of device-only buffers). */
    void *pix[2];
#endif /* HAVE_HIPCC */

    size_t plane_bytes;
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
    bool motion_moving_average;

    VmafDictionary *feature_name_dict;
} MotionV2StateHip;

#define MV2H_BX 16u
#define MV2H_BY 16u

/* Option table mirrors integer_motion_v2.c (CPU reference) for the
 * subset of options the HIP twin's host-side motion3_v2 post-process
 * consumes: name / alias / type / default / min / max / flags match
 * byte-for-byte so co-scheduled CPU+HIP runs name features identically
 * and model files load on either path (ADR-1108, matching the CUDA
 * twin). motion_force_zero and motion_five_frame_window are CPU-only
 * knobs (the HIP kernel always computes the SAD, and the 5-frame window
 * is unsupported per ADR-0337); they are intentionally omitted from this
 * twin's surface. */
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
/* Translate a HIP error code to a negative errno. */
static int mv2_hip_rc(hipError_t rc)
{
    if (rc == hipSuccess)
        return 0;
    switch (rc) {
    case hipErrorInvalidValue:
    case hipErrorInvalidHandle:
        return -EINVAL;
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorNoDevice:
    case hipErrorInvalidDevice:
        return -ENODEV;
    case hipErrorNotSupported:
        return -ENOSYS;
    default:
        return -EIO;
    }
}

/* Load HSACO module and resolve both kernel entry points. */
static int mv2_hip_module_load(MotionV2StateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, motion_v2_score_hsaco);
    if (rc != hipSuccess)
        return mv2_hip_rc(rc);

    rc = hipModuleGetFunction(&s->funcbpc8, s->module, "motion_v2_kernel_8bpc");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return mv2_hip_rc(rc);
    }
    rc = hipModuleGetFunction(&s->funcbpc16, s->module, "motion_v2_kernel_16bpc");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return mv2_hip_rc(rc);
    }
    return 0;
}

/* Allocate ping-pong device buffers. On failure the one already allocated
 * stays set; the caller's mv2_hip_release() frees it. */
static int mv2_hip_bufs_alloc(MotionV2StateHip *s)
{
    hipError_t rc = hipMalloc(&s->pix[0], s->plane_bytes);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->pix[1], s->plane_bytes);
    return (rc == hipSuccess) ? 0 : -ENOMEM;
}

/* Free ping-pong buffers and unload the module. Safe with NULL handles. */
static void mv2_hip_bufs_free(MotionV2StateHip *s)
{
    if (s->pix[1] != NULL) {
        (void)hipFree(s->pix[1]);
        s->pix[1] = NULL;
    }
    if (s->pix[0] != NULL) {
        (void)hipFree(s->pix[0]);
        s->pix[0] = NULL;
    }
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
}

/* SAD kernel over the two ping-pong slots, accumulating into rb.device. */
static int mv2_hip_launch_kernel(MotionV2StateHip *s, unsigned cur_idx, unsigned prev_idx,
                                 ptrdiff_t plane_pitch, hipStream_t str)
{
    const unsigned gx = (s->frame_w + MV2H_BX - 1u) / MV2H_BX;
    const unsigned gy = (s->frame_h + MV2H_BY - 1u) / MV2H_BY;
    uint8_t *prev_dev = (uint8_t *)s->pix[prev_idx];
    uint8_t *cur_dev = (uint8_t *)s->pix[cur_idx];
    uint64_t *sad_dev = (uint64_t *)s->rb.device;
    unsigned w = s->frame_w;
    unsigned h = s->frame_h;
    unsigned bpc = s->bpc;

    /* The 16bpc kernel takes one more argument than the 8bpc one: `bpc`. */
    void *args8[] = {(void *)&prev_dev,
                     (void *)&cur_dev,
                     (void *)&plane_pitch,
                     (void *)&plane_pitch,
                     (void *)&sad_dev,
                     (void *)&w,
                     (void *)&h};
    void *args16[] = {(void *)&prev_dev,    (void *)&cur_dev, (void *)&plane_pitch,
                      (void *)&plane_pitch, (void *)&sad_dev, (void *)&w,
                      (void *)&h,           (void *)&bpc};
    const bool is8 = (s->bpc == 8u);
    return mv2_hip_rc(hipModuleLaunchKernel(is8 ? s->funcbpc8 : s->funcbpc16, gx, gy, 1, MV2H_BX,
                                            MV2H_BY, 1, 0, str, is8 ? args8 : args16, NULL));
}

/* Per-frame submit: HtoD copy, optional kernel launch, event/DtoH copy. */
static int mv2_hip_launch(MotionV2StateHip *s, VmafPicture *ref_pic, unsigned index)
{
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    hipEvent_t submit_ev = vmaf_hip_event_of(s->lc.submit);
    const unsigned cur_idx = index % 2u;
    const unsigned prev_idx = (index + 1u) % 2u;
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const ptrdiff_t plane_pitch = (ptrdiff_t)(s->frame_w * bpp);

    /* HtoD copy of current ref Y plane into ping-pong slot cur_idx. Returns
     * once the picture is read: the caller may recycle it when submit()
     * returns (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
    const VmafHipPlaneUpload plane = {.dst = s->pix[cur_idx],
                                      .dst_pitch = (size_t)plane_pitch,
                                      .pic = ref_pic,
                                      .plane = 0u,
                                      .row_bytes = (size_t)plane_pitch,
                                      .rows = s->frame_h};
    int err = vmaf_hip_picture_upload(&plane, 1u, s->lc.str);
    if (err != 0)
        return err;

    /* Frame 0: nothing to diff against; record submit event so collect
     * can sync. Emit 0 in collect. */
    if (index == 0u)
        return mv2_hip_rc(hipEventRecord(submit_ev, str));

    /* Reset device int64 SAD accumulator (single uint64_t). Must run
     * before the kernel so the atomicAdd starts from 0. */
    hipError_t rc = hipMemsetAsync(s->rb.device, 0, sizeof(uint64_t), str);
    if (rc != hipSuccess)
        return mv2_hip_rc(rc);
    err = mv2_hip_launch_kernel(s, cur_idx, prev_idx, plane_pitch, str);
    if (err != 0)
        return err;

    rc = hipEventRecord(submit_ev, str);
    if (rc == hipSuccess) {
        rc = hipMemcpyAsync(s->rb.host_pinned, s->rb.device, sizeof(uint64_t),
                            hipMemcpyDeviceToHost, str);
    }
    if (rc != hipSuccess)
        return mv2_hip_rc(rc);

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

    /* The 5-tap HIP kernel uses reflect-101 mirror padding; mv2_mirror()
     * returns 2*sup - idx - 2, which is negative when sup < 3.  Refuse
     * smaller frames up front.  Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_v2_hip: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    /* Readback pair: single int64 SAD accumulator + pinned host slot. */
    if (err == 0)
        err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx, sizeof(uint64_t));
#ifdef HAVE_HIPCC
    if (err == 0)
        err = mv2_hip_module_load(s);
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

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];

#ifdef HAVE_HIPCC
    return mv2_hip_launch(s, ref_pic, index);
#else
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
    /* Frame 0: no diff was computed -- emit 0. */
    if (index == 0u) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0.0, index);
    }

    /* SAD sum -> motion_v2_sad_score = sad / 256.0 / (w*h).
     * Matches the CUDA twin's collect formula verbatim (ADR-0138/0139). */
    const uint64_t *sad_host = s->rb.host_pinned;
    const double sad_score = (double)*sad_host / 256.0 / ((double)s->frame_w * (double)s->frame_h);
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion_v2_sad_score",
                                                   sad_score, index);
#else
    (void)feature_collector;
    (void)index;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

#ifdef HAVE_HIPCC
/* Number of consecutive frames, from 0, that carry a SAD score. */
static unsigned mv2_hip_count_frames(VmafFeatureCollector *fc, const char *sad_name)
{
    unsigned n_frames = 0;
    double dummy;
    while (!vmaf_feature_collector_get_score(fc, sad_name, &dummy, n_frames))
        n_frames++;
    return n_frames;
}

/* motion3_v2 seeding — mirrors integer_motion_v2.c::flush exactly.
 * stamp_value blends the *raw SAD* at min_idx, clipped to motion_max_val;
 * it is emitted for all indices i < min_idx. */
static double mv2_hip_stamp_value(const MotionV2StateHip *s, VmafFeatureCollector *fc,
                                  const char *sad_name, unsigned n_frames, unsigned min_idx)
{
    double stamp_value = 0.;
    if (n_frames > min_idx) {
        double sad_at_min_idx;
        if (!vmaf_feature_collector_get_score(fc, sad_name, &sad_at_min_idx, min_idx)) {
            stamp_value =
                MIN(motion_blend(sad_at_min_idx, s->motion_blend_factor, s->motion_blend_offset),
                    s->motion_max_val);
        }
    }
    return stamp_value;
}

/* motion2_v2 of frame i: the smaller fps-weighted SAD of frames i and i + 1,
 * or frame i's alone for the last frame. Mirrors CPU integer_motion_v2.c
 * flush; bit-exact when motion_fps_weight = 1.0 (default). */
static double mv2_hip_motion2(const MotionV2StateHip *s, VmafFeatureCollector *fc,
                              const char *sad_name, unsigned i, unsigned n_frames)
{
    double score_cur;
    double score_next;
    vmaf_feature_collector_get_score(fc, sad_name, &score_cur, i);
    score_cur *= s->motion_fps_weight;
    if (i + 1u >= n_frames)
        return score_cur;
    vmaf_feature_collector_get_score(fc, sad_name, &score_next, i + 1u);
    score_next *= s->motion_fps_weight;
    return score_cur < score_next ? score_cur : score_next;
}

/* motion3_v2 of frame i: per-frame blend + clip + optional moving-average,
 * or the stamp value below min_idx. `*prev_processed` carries the moving
 * average across frames. Mirrors integer_motion_v2.c::flush byte-for-byte. */
static double mv2_hip_motion3(const MotionV2StateHip *s, double motion2, unsigned i,
                              unsigned min_idx, double stamp_value, double *prev_processed)
{
    if (i < min_idx) {
        *prev_processed = stamp_value;
        return stamp_value;
    }
    const double processed = MIN(
        motion_blend(motion2, s->motion_blend_factor, s->motion_blend_offset), s->motion_max_val);
    const double motion3 =
        s->motion_moving_average ? (processed + *prev_processed) / 2.0 : processed;
    *prev_processed = processed;
    return motion3;
}
#endif /* HAVE_HIPCC */

static int flush_fex_hip(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)feature_collector;
    return 1;
#else
    MotionV2StateHip *s = fex->priv;

    /* Host-only post-pass: motion2_v2 = min(score[i], score[i+1]) and
     * motion3_v2 = per-frame blend + clip + optional moving-average.
     * Mirrors the CUDA twin's flush_fex_cuda shape and the CPU reference
     * integer_motion_v2.c::flush byte-for-byte (ADR-1108). */

    /* Resolve the (possibly renamed, for sfr/hfr co-schedule) SAD feature
     * name from the dict — mirrors integer_motion_v2.c::flush. */
    VmafDictionaryEntry *e_sad =
        vmaf_dictionary_get(&s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score", 0);
    const char *sad_name = e_sad ? e_sad->val : "VMAF_integer_feature_motion_v2_sad_score";

    const unsigned n_frames = mv2_hip_count_frames(feature_collector, sad_name);
    if (n_frames < 2u)
        return 1;

    /* 3-frame mode only (min_idx = 1; the 5-frame window is unsupported on
     * motion_v2, ADR-0337). */
    const unsigned min_idx = 1;
    const double stamp_value =
        mv2_hip_stamp_value(s, feature_collector, sad_name, n_frames, min_idx);

    double prev_processed = 0.;
    for (unsigned i = 0; i < n_frames; i++) {
        const double motion2 = mv2_hip_motion2(s, feature_collector, sad_name, i, n_frames);
        int append_err = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion2_v2_score",
            motion2, i);
        if (append_err)
            return append_err;

        const double motion3 =
            mv2_hip_motion3(s, motion2, i, min_idx, stamp_value, &prev_processed);
        append_err = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion3_v2_score",
            motion3, i);
        if (append_err)
            return append_err;
    }

    return 1;
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
