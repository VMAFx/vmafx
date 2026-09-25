/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_motion feature extractor on the HIP backend — seventh
 *  consumer of `core/src/hip/kernel_template.h` (T7-10b
 *  follow-up / ADR-0273).  Real kernel promotion: T7-10b batch-2 /
 *  ADR-0373.
 *
 *  This TU mirrors `core/src/feature/cuda/float_motion_cuda.c`
 *  call-graph-for-call-graph: same private-state struct shape, same
 *  init/submit/collect/close lifecycle, same template helper
 *  invocations, same `flush()` host-only post-processing tail, and
 *  the same `motion_force_zero` short-circuit posture.
 *
 *  Temporal design: a `blur[2]` ping-pong of device float arrays holds
 *  the Gaussian-blurred current/previous frames. `ref_in` is a device
 *  buffer for the raw Y-plane copy (used by the kernel to produce
 *  `cur_blur`). On each submit, `compute_sad` is 0 for the first frame
 *  and 1 afterwards. The per-block float SAD partials land in `rb.device`
 *  (sized `wg_count * sizeof(float)`). The host accumulates them in
 *  double, divides by `w*h`, and emits `VMAF_feature_motion_score` at
 *  `index` and `VMAF_feature_motion2_score = min(prev, cur)` at
 *  `index - 1`. The tail motion2 is emitted in `flush()`.
 *
 *  When `HAVE_HIPCC` is defined (enable_hipcc=true at configure time),
 *  the real HIP Module API path is active. Without it the scaffold
 *  posture is preserved: every lifecycle helper returns -ENOSYS.
 *
 *  HIP adaptation notes vs CUDA twin:
 *  - Warp size 64 on GCN/RDNA; the kernel already accounts for this
 *    (FM_WARP_SIZE=64, FM_WARPS_PER_BLOCK=4 for a 16x16 WG).
 *  - Kernel args are raw pointers (no VmafCudaBuffer indirection).
 *  - HtoD copy uses hipMemcpy2DAsync with hipMemcpyHostToDevice because
 *    pictures arrive as CPU VmafPictures (VMAF_FEATURE_EXTRACTOR_HIP
 *    flag not yet set — same posture as all other HIP consumers).
 *  - `blur[0]` and `blur[1]` are plain hipMalloc device buffers (float,
 *    w*h), analogous to the CUDA twin's `VmafCudaBuffer *blur[2]`.
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

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"
#include "float_motion_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */
#endif /* HAVE_HIPCC */

/* Block dimensions mirror the HIP kernel's FM_BX / FM_BY. */
#define FMH_BX 16u
#define FMH_BY 16u

typedef struct FloatMotionStateHip {
    /* Lifecycle (private stream + submit/finished event pair) and
     * the (device per-WG SAD float partials, pinned host readback
     * slot) pair are managed by `hip/kernel_template.h`. */
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb;
    VmafHipContext *ctx;

#ifdef HAVE_HIPCC
    /* HSACO module + per-bpc kernel function handles. */
    hipModule_t module;
    hipFunction_t funcbpc8;
    hipFunction_t funcbpc16;
    /* Device-only raw Y-plane staging buffer (HtoD copy each frame). */
    void *ref_in;
    /* Gaussian-blurred frame ping-pong (float, w*h pixels each). */
    void *blur[2];
#endif /* HAVE_HIPCC */

    int cur_blur;
    unsigned wg_count;
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    double prev_motion_score;
    double motion_fps_weight;
    bool debug;
    bool motion_force_zero;

    VmafDictionary *feature_name_dict;
} FloatMotionStateHip;

static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(FloatMotionStateHip, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = true,
    },
    {
        .name = "motion_force_zero",
        .alias = "force_0",
        .help = "force motion score to zero",
        .offset = offsetof(FloatMotionStateHip, motion_force_zero),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_fps_weight",
        .alias = "mfw",
        .help = "fps-aware multiplicative weight/correction",
        .offset = offsetof(FloatMotionStateHip, motion_fps_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0},
};

static int extract_force_zero(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                              VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                              VmafPicture *dist_pic_90, unsigned index,
                              VmafFeatureCollector *feature_collector)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    FloatMotionStateHip *s = fex->priv;

    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion2_score", 0.0, index);
    if (s->debug && err == 0) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion_score", 0.0, index);
    }
    return err;
}

static int close_fex_hip(VmafFeatureExtractor *fex);

/* Extracted from init: motion_force_zero short-circuit.  init_fex_hip releases
 * the device objects before it returns; the regular null-safe close callback
 * remains responsible for the feature-name dictionary allocated below. */
static int init_force_zero_hip(VmafFeatureExtractor *fex, FloatMotionStateHip *s)
{
    fex->extract = extract_force_zero;
    fex->submit = NULL;
    fex->collect = NULL;
    fex->flush = NULL;
    fex->close = close_fex_hip;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        return -ENOMEM;
    }
    return 0;
}

#ifdef HAVE_HIPCC
/* Translate a HIP error code to a negative errno. */
static int fm_hip_rc(hipError_t rc)
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

/* Load the HSACO module and look up the two per-bpc kernel entry points.
 * Called once from init() when HAVE_HIPCC is defined. */
static int fm_hip_module_load(FloatMotionStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, float_motion_score_hsaco);
    if (rc != hipSuccess)
        return fm_hip_rc(rc);

    rc = hipModuleGetFunction(&s->funcbpc8, s->module, "float_motion_hip_kernel_8bpc");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->funcbpc16, s->module, "float_motion_hip_kernel_16bpc");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return fm_hip_rc(rc);
}

/* Blur + SAD kernel on `pstr`: blurs the staged frame into the current
 * ping-pong slot and, when `compute_sad` is set, writes the per-block SAD
 * against the previous slot into rb.device. */
static int fm_hip_launch_kernel(FloatMotionStateHip *s, ptrdiff_t plane_pitch, unsigned compute_sad,
                                hipStream_t pstr)
{
    const unsigned gx = (s->frame_w + FMH_BX - 1u) / FMH_BX;
    const unsigned gy = (s->frame_h + FMH_BY - 1u) / FMH_BY;
    const uint8_t *ref_dev = (const uint8_t *)s->ref_in;
    float *cur_blur = (float *)s->blur[s->cur_blur];
    const float *prev_blur = (const float *)s->blur[1 - s->cur_blur];
    float *partials_dev = (float *)s->rb.device;
    unsigned w = s->frame_w;
    unsigned h = s->frame_h;
    unsigned bpc = s->bpc;

    /* The 16bpc kernel takes `bpc` ahead of `compute_sad`. */
    void *args8[] = {
        (void *)&ref_dev,      (void *)&plane_pitch, (void *)&cur_blur, (void *)&prev_blur,
        (void *)&partials_dev, (void *)&w,           (void *)&h,        (void *)&compute_sad,
    };
    void *args16[] = {
        (void *)&ref_dev,   (void *)&plane_pitch,  (void *)&cur_blur,
        (void *)&prev_blur, (void *)&partials_dev, (void *)&w,
        (void *)&h,         (void *)&bpc,          (void *)&compute_sad,
    };
    const bool is8 = (s->bpc == 8u);
    return fm_hip_rc(hipModuleLaunchKernel(is8 ? s->funcbpc8 : s->funcbpc16, gx, gy, 1, FMH_BX,
                                           FMH_BY, 1, 0, pstr, is8 ? args8 : args16, NULL));
}

/* HtoD copy ref luma plane, launch the motion kernel, record events,
 * enqueue DtoH copy of per-block SAD partials.
 *
 * `compute_sad`: 0 for the first frame (no previous blur — partials will
 * all be 0.0 by kernel contract), 1 for subsequent frames. */
static int fm_hip_launch(FloatMotionStateHip *s, VmafPicture *ref_pic, unsigned compute_sad)
{
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    hipStream_t pstr = vmaf_hip_stream_of(0u); /* no VmafPicture stream handle yet */
    hipEvent_t submit_ev = vmaf_hip_event_of(s->lc.submit);

    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const ptrdiff_t plane_pitch = (ptrdiff_t)(s->frame_w * bpp);

    /* HtoD copy of ref luma plane into tightly-pitched staging buffer. Returns
     * once the picture is read: the caller may recycle it when submit()
     * returns (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
    const VmafHipPlaneUpload plane = {.dst = s->ref_in,
                                      .dst_pitch = (size_t)plane_pitch,
                                      .pic = ref_pic,
                                      .plane = 0u,
                                      .row_bytes = (size_t)plane_pitch,
                                      .rows = s->frame_h};
    /* Upload on the private stream, not the null stream the kernels use: a
     * null-stream copy would queue behind every other extractor's kernels of
     * this frame, and the wait would block the host on all of them. The
     * copies are complete before the kernels are enqueued, and collect() of
     * the previous frame has already drained the kernels that read these
     * buffers. */
    int err = vmaf_hip_picture_upload(&plane, 1u, s->lc.str);
    if (err == 0)
        err = fm_hip_launch_kernel(s, plane_pitch, compute_sad, pstr);
    if (err != 0)
        return err;

    /* Record submit event on picture stream, wait on private stream,
     * DtoH copy of SAD partials, then record finished event. */
    hipError_t rc = hipEventRecord(submit_ev, pstr);
    if (rc == hipSuccess)
        rc = hipStreamWaitEvent(str, submit_ev, 0);
    if (rc == hipSuccess) {
        rc = hipMemcpyAsync(s->rb.host_pinned, s->rb.device, (size_t)s->wg_count * sizeof(float),
                            hipMemcpyDeviceToHost, str);
    }
    if (rc != hipSuccess)
        return fm_hip_rc(rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

/* Allocate ref_in staging buffer and blur[0/1] ping-pong. On failure the
 * buffers already allocated stay set; the caller's fm_hip_release() frees
 * them. */
static int fm_hip_bufs_alloc(FloatMotionStateHip *s, unsigned w, unsigned h, unsigned bpc)
{
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t plane_bytes = (size_t)w * h * bpp;
    const size_t blur_bytes = (size_t)w * h * sizeof(float);

    hipError_t rc = hipMalloc(&s->ref_in, plane_bytes);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->blur[0], blur_bytes);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->blur[1], blur_bytes);
    return (rc == hipSuccess) ? 0 : -ENOMEM;
}

/* Release module + device buffers.  Safe to call with NULL handles. */
static void fm_hip_bufs_free(FloatMotionStateHip *s)
{
    if (s->blur[1] != NULL) {
        (void)hipFree(s->blur[1]);
        s->blur[1] = NULL;
    }
    if (s->blur[0] != NULL) {
        (void)hipFree(s->blur[0]);
        s->blur[0] = NULL;
    }
    if (s->ref_in != NULL) {
        (void)hipFree(s->ref_in);
        s->ref_in = NULL;
    }
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
}
#endif /* HAVE_HIPCC */

/* Release the HIP resources: lifecycle first (it drains the stream, so no
 * kernel still uses a buffer), then buffers + module (best-effort, as in the
 * CUDA twin), the readback pair and the context. Every step tolerates a
 * handle that was never created. Returns the first error. */
static int fm_hip_release_device(FloatMotionStateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
#ifdef HAVE_HIPCC
    fm_hip_bufs_free(s);
#endif /* HAVE_HIPCC */
    const int err = vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0)
        rc = err;
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return rc;
}

/* Everything init() may have set up; serves a failed init() and close(). */
static int fm_hip_release(FloatMotionStateHip *s)
{
    int rc = fm_hip_release_device(s);
    if (s->feature_name_dict != NULL) {
        const int err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0)
            rc = err;
    }
    return rc;
}

/* Device-side half of init(): readback pair, module, buffers, name dict. */
static int fm_hip_init_device(VmafFeatureExtractor *fex, FloatMotionStateHip *s)
{
    /* Readback pair: device per-WG float SAD partials + pinned host slot. */
    int err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx, (size_t)s->wg_count * sizeof(float));
#ifdef HAVE_HIPCC
    if (err == 0)
        err = fm_hip_module_load(s);
    /* Staging buffer (ref_in) and blurred-frame ping-pong (blur[0/1]). */
    if (err == 0)
        err = fm_hip_bufs_alloc(s, s->frame_w, s->frame_h, s->bpc);
#endif /* HAVE_HIPCC */
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    return err;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatMotionStateHip *s = fex->priv;

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->index = 0;
    s->prev_motion_score = 0.0;
    s->cur_blur = 0;

    /* The 5-tap HIP float kernel uses reflect-101 mirror padding; fm_mirror()
     * returns 2*sup - idx - 2, which is negative when sup < 3.  Refuse
     * smaller frames up front.  Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_motion_hip: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    const unsigned gx = (w + FMH_BX - 1u) / FMH_BX;
    const unsigned gy = (h + FMH_BY - 1u) / FMH_BY;
    s->wg_count = gx * gy;

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err == 0 && s->motion_force_zero) {
        /* extract_force_zero needs the name dictionary but not the device
         * objects. Release those now; close_fex_hip() remains installed and
         * later frees the dictionary through the regular context teardown. */
        err = init_force_zero_hip(fex, s);
        (void)fm_hip_release_device(s);
        return err;
    }
    if (err == 0)
        err = fm_hip_init_device(fex, s);
    if (err != 0)
        (void)fm_hip_release(s);
    return err;
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    FloatMotionStateHip *s = fex->priv;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];

#ifdef HAVE_HIPCC
    /* First frame has no previous blurred frame — kernel writes cur_blur
     * but computes no SAD (compute_sad=0, partials all 0.0 by contract). */
    const unsigned compute_sad = (index > 0u) ? 1u : 0u;
    return fm_hip_launch(s, ref_pic, compute_sad);
#else
    /* Scaffold posture: surface -ENOSYS via the pre-launch helper so the
     * feature engine sees "runtime not ready". */
    int err = vmaf_hip_kernel_submit_pre_launch(&s->lc, s->ctx, &s->rb,
                                                /* picture_stream */ 0,
                                                /* dist_ready_event */ 0);
    if (err != 0)
        return err;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

#ifdef HAVE_HIPCC
/* Emit the scores that frame `index`'s motion_score completes: motion2 =
 * min(prev, cur) at index - 1 and, in debug mode, motion_score at index.
 * Same order as the CUDA twin's collect_fex_cuda. */
static int fm_hip_emit(FloatMotionStateHip *s, VmafFeatureCollector *feature_collector,
                       unsigned index, double motion_score)
{
    int err = 0;
    if (index == 0u) {
        /* First frame: no previous, emit 0 for both scores. The CUDA
         * twin defers motion2 for the first frame to the next collect;
         * here we match that behaviour by emitting 0 directly. */
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion2_score", 0.0, index);
        if (s->debug && err == 0) {
            err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                          "VMAF_feature_motion_score", 0.0, index);
        }
        s->prev_motion_score = 0.0;
        return err;
    }

    if (index > 1u) {
        /* Apply fps weight to both operands before the min so the weight
         * scales the motion2 output; identity when motion_fps_weight = 1.0.
         * motion2 at index 0 was already written by the index == 0 branch,
         * so index == 1 emits motion_score only. */
        const double w_cur = motion_score * s->motion_fps_weight;
        const double w_prev = s->prev_motion_score * s->motion_fps_weight;
        const double motion2 = (w_cur < w_prev) ? w_cur : w_prev;
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion2_score", motion2,
                                                      index - 1u);
    }
    if (s->debug && err == 0) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion_score", motion_score,
                                                      index);
    }
    s->prev_motion_score = motion_score;
    return err;
}
#endif /* HAVE_HIPCC */

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    FloatMotionStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

#ifdef HAVE_HIPCC
    /* Accumulate per-block float SAD partials in double and compute the
     * per-frame motion score. Mirrors the CUDA twin's cross-block
     * reduction precision posture. */
    const float *partials = (const float *)s->rb.host_pinned;
    double total_sad = 0.0;
    for (unsigned i = 0; i < s->wg_count; i++)
        total_sad += (double)partials[i];

    const double n_pixels = (double)s->frame_w * (double)s->frame_h;
    const double motion_score = total_sad / n_pixels;

    /* Advance blur ping-pong. */
    s->cur_blur = 1 - s->cur_blur;

    return fm_hip_emit(s, feature_collector, index, motion_score);
#else
    (void)feature_collector;
    (void)index;
    /* Advance ping-pong even in scaffold so state stays consistent. */
    s->cur_blur = 1 - s->cur_blur;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int flush_fex_hip(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)feature_collector;
    /* Scaffold: no scores collected; return 1 ("done") to avoid an
     * infinite flush loop in the feature engine. */
    return 1;
#else
    FloatMotionStateHip *s = fex->priv;

    if (s->index == 0u)
        return 1;

    static const char feature_name[] = "VMAF_feature_motion2_score";
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&s->feature_name_dict, feature_name, 0);
    const char *resolved_name = entry ? entry->val : feature_name;
    double existing;
    if (vmaf_feature_collector_get_score(feature_collector, resolved_name, &existing, s->index) ==
        0)
        return 1;

    /* Emit the tail motion2 = prev_motion_score * fps_weight at the last
     * frame index. A pending collect can already have written the same
     * option-derived feature/index, so the probe above makes flush idempotent. */
    int err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, feature_name,
        s->prev_motion_score * s->motion_fps_weight, s->index);
    return (err != 0) ? err : 1;
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return fm_hip_release(fex->priv);
}

static const char *provided_features[] = {"VMAF_feature_motion_score", "VMAF_feature_motion2_score",
                                          NULL};

/* Load-bearing: the feature extractor is registered via
 * `extern VmafFeatureExtractor vmaf_fex_float_motion_hip;` in
 * `core/src/feature/feature_extractor.cpp`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern every CUDA / SYCL / Vulkan feature extractor uses (see
 * e.g. `vmaf_fex_float_motion_cuda` in
 * `core/src/feature/cuda/float_motion_cuda.c`). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_motion_hip = {
    .name = "float_motion_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .flush = flush_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(FloatMotionStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
