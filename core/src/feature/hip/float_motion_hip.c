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
 *  This TU mirrors `core/src/feature/cuda/float_motion_cuda.c`: same
 *  init/submit/collect/close lifecycle, same template helper invocations,
 *  same `flush()` host-only post-processing tail, and the same
 *  `motion_force_zero` short-circuit posture.
 *
 *  Temporal design: per plane, a `blur[2]` ping-pong of device float arrays
 *  holds the blurred current and previous frames and `ref_in` the raw plane.
 *  On each submit the blur kernel runs with `compute_sad` 0 for the first
 *  frame and 1 afterwards; its per-block float SAD partials land in
 *  `rb.device`. The host accumulates them in double, divides by `w * h`, and
 *  emits `VMAF_feature_motion_score` at `index` and
 *  `VMAF_feature_motion2_score = min(prev, cur)` at `index - 1`. The tail
 *  motion2 is emitted in `flush()`.
 *
 *  Options and outputs are the CPU float_motion.c's (ADR-1382, ADR-1404):
 *  - Every emitted `motion` / `motion2` value goes through motion_clip(): it
 *    is scaled by `motion_fps_weight` and capped at `motion_max_val`, the
 *    debug `motion` score included.
 *  - `VMAF_feature_motion3_score` is motion_blend_clip() of the same value
 *    (fps weight, the `motion_blend_factor` / `motion_blend_offset` blend,
 *    the cap), at the CPU's indices: frame 0 from the first SAD, then the
 *    blended motion2, the tail from `flush()`, and 0 for a one-frame run.
 *  - `motion_filter_size` selects the blur filter in the kernel.
 *  - `motion_add_scale1` adds the SAD of both blurred frames scaled to half
 *    size (a second kernel), and `motion_add_uv` runs the whole chain on the
 *    U and V planes too and adds their scores, as compute_motion() does.
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
#include "motion_blend_tools.h"
#include "motion_tools.h"
#include "picture_geometry.h"

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

/* CPU float_motion.c DEFAULT_MOTION_MAX_VAL. */
#define FMH_DEFAULT_MAX_VAL (10000.0)

/* Y, and U and V with motion_add_uv. */
#define FMH_MAX_PLANES 3u

/* One picture plane: its geometry, where its SAD partials sit in the
 * readback, and its device buffers. */
typedef struct FmPlaneHip {
    unsigned w;
    unsigned h;
    /* Half-size plane of the scale-1 SAD (motion_add_scale1); wg1 == 0
     * without the option. */
    unsigned sw;
    unsigned sh;
    /* Block counts of the blur + SAD kernel and of the scale-1 kernel, and
     * the index of their first partial in the readback. */
    unsigned wg0;
    unsigned wg1;
    unsigned off0;
    unsigned off1;
#ifdef HAVE_HIPCC
    /* Device-only raw plane staging buffer (HtoD copy each frame). */
    void *ref_in;
    /* Blurred frame ping-pong (float, w*h pixels each). */
    void *blur[2];
#endif /* HAVE_HIPCC */
} FmPlaneHip;

typedef struct FloatMotionStateHip {
    /* Lifecycle (private stream + submit/finished event pair) and
     * the (device per-WG SAD float partials, pinned host readback
     * slot) pair are managed by `hip/kernel_template.h`. */
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb;
    VmafHipContext *ctx;

#ifdef HAVE_HIPCC
    /* HSACO module + kernel function handles. */
    hipModule_t module;
    hipFunction_t funcbpc8;
    hipFunction_t funcbpc16;
    hipFunction_t func_scale1;
#endif /* HAVE_HIPCC */

    FmPlaneHip plane[FMH_MAX_PLANES];
    unsigned n_planes;
    /* Floats in the readback: the partials of every plane and scale. */
    unsigned partial_count;

    int cur_blur;
    unsigned index;
    unsigned bpc;
    double prev_motion_score;
    double motion_fps_weight;
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    int motion_filter_size;
    bool debug;
    bool motion_force_zero;
    bool motion_add_scale1;
    bool motion_add_uv;

    VmafDictionary *feature_name_dict;
} FloatMotionStateHip;

/* The CPU float_motion.c table: same names, aliases, defaults, ranges and
 * order. The order spells the feature names (motion3_mbf_0.5_mbo_2). */
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
    {
        .name = "motion_blend_factor",
        .alias = "mbf",
        .help = "blend motion score given an offset",
        .offset = offsetof(FloatMotionStateHip, motion_blend_factor),
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
        .offset = offsetof(FloatMotionStateHip, motion_blend_offset),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 40.0,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_add_scale1",
        .alias = "mdc",
        .help = "add motion score from scale1",
        .offset = offsetof(FloatMotionStateHip, motion_add_scale1),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_filter_size",
        .alias = "mfs",
        .help = "filtering size",
        .offset = offsetof(FloatMotionStateHip, motion_filter_size),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_MOTION_FILTER_SIZE,
        .min = 0,
        .max = 9,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_add_uv",
        .alias = "mau",
        .help = "include U and V terms",
        .offset = offsetof(FloatMotionStateHip, motion_add_uv),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .alias = "mmxv",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .offset = offsetof(FloatMotionStateHip, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = FMH_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0},
};

static int fm_hip_append(const FloatMotionStateHip *s, VmafFeatureCollector *feature_collector,
                         const char *name, double score, unsigned index)
{
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict, name,
                                                   score, index);
}

/* CPU float_motion.c::motion_append_forced_zero. */
static int extract_force_zero(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                              VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                              VmafPicture *dist_pic_90, unsigned index,
                              VmafFeatureCollector *feature_collector)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    const FloatMotionStateHip *s = fex->priv;

    int err = fm_hip_append(s, feature_collector, "VMAF_feature_motion2_score", 0.0, index);
    if (err == 0)
        err = fm_hip_append(s, feature_collector, "VMAF_feature_motion3_score", 0.0, index);
    if (s->debug && err == 0)
        err = fm_hip_append(s, feature_collector, "VMAF_feature_motion_score", 0.0, index);
    return err;
}

/* motion_force_zero keeps the asynchronous interface
 * (T-HIP-MOTION-FORCE-ZERO-NULL-SUBMIT-2026-09-30). libvmaf picks
 * submit()/collect() from the callbacks of the uninitialised context
 * (read_pictures_dispatch_one()), and vmaf_feature_extractor_context_submit()
 * runs init() only then, so an init() that cleared submit had the framework
 * call a NULL submit() on the first frame. submit() has no picture to read;
 * collect() writes the zeros extract() writes for a direct caller. */
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

static int close_fex_hip(VmafFeatureExtractor *fex);

/* Extracted from init: motion_force_zero short-circuit.  init_fex_hip releases
 * the device objects before it returns; the regular null-safe close callback
 * remains responsible for the feature-name dictionary allocated below. */
static int init_force_zero_hip(VmafFeatureExtractor *fex, FloatMotionStateHip *s)
{
    fex->extract = extract_force_zero;
    fex->submit = submit_force_zero;
    fex->collect = collect_force_zero;
    fex->flush = NULL;
    fex->close = close_fex_hip;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        return -ENOMEM;
    }
    return 0;
}

/* The blur needs filter_size / 2 + 1 samples on each axis of every plane it
 * runs on: CPU float_motion.c::motion_check_min_dim(), where only
 * motion_filter_size == 3 narrows the filter. */
static int fm_hip_check_min_dim(const FloatMotionStateHip *s, const FmPlaneHip *p, const char *name)
{
    const unsigned taps = (s->motion_filter_size == 3) ? 3u : 5u;
    const unsigned min_dim = taps / 2u + 1u;
    if (p->w >= min_dim && p->h >= min_dim)
        return 0;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "float_motion_hip: %s plane %ux%u is below the %u-tap filter minimum %ux%u\n", name,
             p->w, p->h, taps, min_dim, min_dim);
    return -EINVAL;
}

/* Block counts and readback offsets of one plane; returns the index after
 * its last partial. */
static unsigned fm_hip_plane_layout(const FloatMotionStateHip *s, FmPlaneHip *p, unsigned w,
                                    unsigned h, unsigned offset)
{
    p->w = w;
    p->h = h;
    /* motion.c::vmaf_image_sad_c(): (int)(width * 0.5 + 0.5). */
    p->sw = (unsigned)((double)w * 0.5 + 0.5);
    p->sh = (unsigned)((double)h * 0.5 + 0.5);
    p->wg0 = ((w + FMH_BX - 1u) / FMH_BX) * ((h + FMH_BY - 1u) / FMH_BY);
    p->wg1 = 0u;
    if (s->motion_add_scale1)
        p->wg1 = ((p->sw + FMH_BX - 1u) / FMH_BX) * ((p->sh + FMH_BY - 1u) / FMH_BY);
    p->off0 = offset;
    p->off1 = offset + p->wg0;
    return p->off1 + p->wg1;
}

/* Geometry of every plane the options ask for, checked as the CPU checks it
 * (motion_check_min_dim_all_planes()). Touches no device object. */
static int fm_hip_init_geometry(FloatMotionStateHip *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                unsigned h)
{
    s->n_planes = 1u;
    s->partial_count = fm_hip_plane_layout(s, &s->plane[0], w, h, 0u);
    int err = fm_hip_check_min_dim(s, &s->plane[0], "luma");
    if (err != 0 || !s->motion_add_uv)
        return err;

    if (pix_fmt != VMAF_PIX_FMT_YUV420P && pix_fmt != VMAF_PIX_FMT_YUV422P &&
        pix_fmt != VMAF_PIX_FMT_YUV444P) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_motion_hip: motion_add_uv needs a pixel format with chroma planes\n");
        return -EINVAL;
    }
    const unsigned cw = vmaf_chroma_extent(w, pix_fmt != VMAF_PIX_FMT_YUV444P);
    const unsigned ch = vmaf_chroma_extent(h, pix_fmt == VMAF_PIX_FMT_YUV420P);
    for (unsigned c = 1u; c < FMH_MAX_PLANES; c++)
        s->partial_count = fm_hip_plane_layout(s, &s->plane[c], cw, ch, s->partial_count);
    s->n_planes = FMH_MAX_PLANES;
    return fm_hip_check_min_dim(s, &s->plane[1], "chroma");
}

#ifdef HAVE_HIPCC
/* CPU float_motion.c::motion_clip: fps weight, then the motion_max_val cap. */
static double fm_hip_motion_clip(const FloatMotionStateHip *s, double score)
{
    return MIN(score * s->motion_fps_weight, s->motion_max_val);
}

/* CPU float_motion.c::motion_blend_clip (motion3): fps weight, the blend,
 * then the motion_max_val cap. */
static double fm_hip_motion_blend_clip(const FloatMotionStateHip *s, double score)
{
    return MIN(
        motion_blend(score * s->motion_fps_weight, s->motion_blend_factor, s->motion_blend_offset),
        s->motion_max_val);
}

/* Load the HSACO module and look up the kernel entry points.
 * Called once from init() when HAVE_HIPCC is defined. */
static int fm_hip_module_load(FloatMotionStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, float_motion_score_hsaco);
    if (rc != hipSuccess)
        return vmaf_hip_rc_to_errno(rc);

    rc = hipModuleGetFunction(&s->funcbpc8, s->module, "float_motion_hip_kernel_8bpc");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->funcbpc16, s->module, "float_motion_hip_kernel_16bpc");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->func_scale1, s->module, "float_motion_hip_scale1_sad");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return vmaf_hip_rc_to_errno(rc);
}

static size_t fm_hip_bytes_per_sample(const FloatMotionStateHip *s)
{
    return (s->bpc <= 8u) ? 1u : 2u;
}

/* Blur + SAD kernel of plane `p` on `pstr`: blurs the staged plane into the
 * current ping-pong slot and, when `compute_sad` is set, writes the per-block
 * SAD against the previous slot at the plane's scale-0 partials. */
static int fm_hip_launch_blur(FloatMotionStateHip *s, const FmPlaneHip *p, unsigned compute_sad,
                              hipStream_t pstr)
{
    const unsigned gx = (p->w + FMH_BX - 1u) / FMH_BX;
    const unsigned gy = (p->h + FMH_BY - 1u) / FMH_BY;
    const uint8_t *ref_dev = (const uint8_t *)p->ref_in;
    ptrdiff_t plane_pitch = (ptrdiff_t)((size_t)p->w * fm_hip_bytes_per_sample(s));
    float *cur_blur = (float *)p->blur[s->cur_blur];
    const float *prev_blur = (const float *)p->blur[1 - s->cur_blur];
    float *partials_dev = (float *)s->rb.device + p->off0;
    unsigned w = p->w;
    unsigned h = p->h;
    unsigned bpc = s->bpc;
    unsigned filter_size = (unsigned)s->motion_filter_size;

    /* The 16bpc kernel takes `bpc` ahead of `filter_size`. */
    void *args8[] = {
        (void *)&ref_dev,   (void *)&plane_pitch,  (void *)&cur_blur,
        (void *)&prev_blur, (void *)&partials_dev, (void *)&w,
        (void *)&h,         (void *)&filter_size,  (void *)&compute_sad,
    };
    void *args16[] = {
        (void *)&ref_dev,      (void *)&plane_pitch, (void *)&cur_blur, (void *)&prev_blur,
        (void *)&partials_dev, (void *)&w,           (void *)&h,        (void *)&bpc,
        (void *)&filter_size,  (void *)&compute_sad,
    };
    const bool is8 = (s->bpc == 8u);
    return vmaf_hip_rc_to_errno(hipModuleLaunchKernel(is8 ? s->funcbpc8 : s->funcbpc16, gx, gy, 1,
                                                      FMH_BX, FMH_BY, 1, 0, pstr,
                                                      is8 ? args8 : args16, NULL));
}

/* Scale-1 SAD kernel of plane `p` on `pstr`, after its blur kernel: reads
 * both ping-pong slots and writes the plane's scale-1 partials. */
static int fm_hip_launch_scale1(FloatMotionStateHip *s, const FmPlaneHip *p, hipStream_t pstr)
{
    const unsigned gx = (p->sw + FMH_BX - 1u) / FMH_BX;
    const unsigned gy = (p->sh + FMH_BY - 1u) / FMH_BY;
    const float *cur_blur = (const float *)p->blur[s->cur_blur];
    const float *prev_blur = (const float *)p->blur[1 - s->cur_blur];
    float *partials_dev = (float *)s->rb.device + p->off1;
    unsigned w = p->w;
    unsigned h = p->h;
    unsigned sw = p->sw;
    unsigned sh = p->sh;
    void *args[] = {
        (void *)&cur_blur, (void *)&prev_blur, (void *)&partials_dev, (void *)&w,
        (void *)&h,        (void *)&sw,        (void *)&sh,
    };
    return vmaf_hip_rc_to_errno(
        hipModuleLaunchKernel(s->func_scale1, gx, gy, 1, FMH_BX, FMH_BY, 1, 0, pstr, args, NULL));
}

/* Every kernel of the frame, plane by plane, on `pstr`. The scale-1 SAD
 * needs a previous frame, so frame 0 runs the blur only. */
static int fm_hip_launch_kernels(FloatMotionStateHip *s, unsigned compute_sad, hipStream_t pstr)
{
    int err = 0;
    for (unsigned c = 0u; c < s->n_planes && err == 0; c++) {
        const FmPlaneHip *p = &s->plane[c];
        err = fm_hip_launch_blur(s, p, compute_sad, pstr);
        if (err == 0 && p->wg1 != 0u && compute_sad != 0u)
            err = fm_hip_launch_scale1(s, p, pstr);
    }
    return err;
}

/* HtoD copy of every plane the extractor reads into its tightly-pitched
 * staging buffer. Returns once the picture is read: the caller may recycle
 * it when submit() returns (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18).
 *
 * Upload on the private stream, not the null stream the kernels use: a
 * null-stream copy would queue behind every other extractor's kernels of
 * this frame, and the wait would block the host on all of them. The copies
 * are complete before the kernels are enqueued, and collect() of the
 * previous frame has already drained the kernels that read these buffers. */
static int fm_hip_upload(const FloatMotionStateHip *s, const VmafPicture *ref_pic)
{
    VmafHipPlaneUpload planes[FMH_MAX_PLANES];
    for (unsigned c = 0u; c < s->n_planes; c++) {
        const size_t row_bytes = (size_t)s->plane[c].w * fm_hip_bytes_per_sample(s);
        planes[c] = (VmafHipPlaneUpload){.dst = s->plane[c].ref_in,
                                         .dst_pitch = row_bytes,
                                         .pic = ref_pic,
                                         .plane = c,
                                         .row_bytes = row_bytes,
                                         .rows = s->plane[c].h};
    }
    return vmaf_hip_picture_upload(planes, s->n_planes, s->lc.str);
}

/* HtoD copy of the reference planes, launch the motion kernels, record
 * events, enqueue DtoH copy of per-block SAD partials.
 *
 * `compute_sad`: 0 for the first frame (no previous blur — partials will
 * all be 0.0 by kernel contract), 1 for subsequent frames. */
static int fm_hip_launch(FloatMotionStateHip *s, const VmafPicture *ref_pic, unsigned compute_sad)
{
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    hipStream_t pstr = vmaf_hip_stream_of(0u); /* no VmafPicture stream handle yet */
    hipEvent_t submit_ev = vmaf_hip_event_of(s->lc.submit);

    int err = fm_hip_upload(s, ref_pic);
    if (err == 0)
        err = fm_hip_launch_kernels(s, compute_sad, pstr);
    if (err != 0)
        return err;

    /* Record submit event on picture stream, wait on private stream,
     * DtoH copy of SAD partials, then record finished event. */
    hipError_t rc = hipEventRecord(submit_ev, pstr);
    if (rc == hipSuccess)
        rc = hipStreamWaitEvent(str, submit_ev, 0);
    if (rc == hipSuccess) {
        rc = hipMemcpyAsync(s->rb.host_pinned, s->rb.device,
                            (size_t)s->partial_count * sizeof(float), hipMemcpyDeviceToHost, str);
    }
    if (rc != hipSuccess)
        return vmaf_hip_rc_to_errno(rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

/* Allocate the staging buffer and the blur ping-pong of every plane, and
 * zero the partials: frame 0 runs no scale-1 kernel, and its read-back must
 * not carry uninitialised device memory. On failure the buffers already
 * allocated stay set; the caller's fm_hip_release() frees them. */
static int fm_hip_bufs_alloc(FloatMotionStateHip *s)
{
    hipError_t rc = hipMemset(s->rb.device, 0, (size_t)s->partial_count * sizeof(float));
    for (unsigned c = 0u; c < s->n_planes && rc == hipSuccess; c++) {
        FmPlaneHip *p = &s->plane[c];
        const size_t pixels = (size_t)p->w * p->h;
        rc = hipMalloc(&p->ref_in, pixels * fm_hip_bytes_per_sample(s));
        if (rc == hipSuccess)
            rc = hipMalloc(&p->blur[0], pixels * sizeof(float));
        if (rc == hipSuccess)
            rc = hipMalloc(&p->blur[1], pixels * sizeof(float));
    }
    return vmaf_hip_rc_to_errno(rc);
}

/* Release module + device buffers.  Safe to call with NULL handles. */
static void fm_hip_bufs_free(FloatMotionStateHip *s)
{
    for (unsigned c = 0u; c < FMH_MAX_PLANES; c++) {
        void **bufs[] = {&s->plane[c].blur[1], &s->plane[c].blur[0], &s->plane[c].ref_in};
        for (unsigned i = 0u; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
            if (*bufs[i] != NULL)
                (void)hipFree(*bufs[i]);
            *bufs[i] = NULL;
        }
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
    int err =
        vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx, (size_t)s->partial_count * sizeof(float));
#ifdef HAVE_HIPCC
    if (err == 0)
        err = fm_hip_module_load(s);
    /* Staging buffers (ref_in) and blurred-frame ping-pongs (blur[0/1]). */
    if (err == 0)
        err = fm_hip_bufs_alloc(s);
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
    FloatMotionStateHip *s = fex->priv;

    s->bpc = bpc;
    s->index = 0;
    s->prev_motion_score = 0.0;
    s->cur_blur = 0;

    /* The blur mirrors past the plane (reflect-101); refuse planes below the
     * filter's minimum up front, as the CPU does. */
    int err = fm_hip_init_geometry(s, pix_fmt, w, h);
    if (err != 0)
        return err;

    err = vmaf_hip_context_new(&s->ctx, 0);
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

#ifdef HAVE_HIPCC
    /* First frame has no previous blurred frame — kernel writes cur_blur
     * but computes no SAD (compute_sad=0, partials all 0.0 by contract). */
    const unsigned compute_sad = (index > 0u) ? 1u : 0u;
    return fm_hip_launch(s, ref_pic, compute_sad);
#else
    (void)ref_pic;
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
/* Sum of `count` float partials starting at `first`, in double. */
static double fm_hip_sum_partials(const float *partials, unsigned first, unsigned count)
{
    double total = 0.0;
    for (unsigned i = 0u; i < count; i++)
        total += (double)partials[first + i];
    return total;
}

/* The frame's SAD score from the read-back partials: per plane the mean
 * absolute difference, plus the scale-1 mean with motion_add_scale1; the
 * planes add up (CPU float_motion.c::motion_score_pair()). */
static double fm_hip_frame_score(const FloatMotionStateHip *s)
{
    const float *partials = (const float *)s->rb.host_pinned;
    double score = 0.0;
    for (unsigned c = 0u; c < s->n_planes; c++) {
        const FmPlaneHip *p = &s->plane[c];
        score += fm_hip_sum_partials(partials, p->off0, p->wg0) / ((double)p->w * (double)p->h);
        if (p->wg1 != 0u) {
            score +=
                fm_hip_sum_partials(partials, p->off1, p->wg1) / ((double)p->sw * (double)p->sh);
        }
    }
    return score;
}

/* Emit the scores that frame `index`'s motion_score completes, in the CPU
 * float_motion.c::extract() order: the debug motion score at `index`,
 * motion3 at 0 from the first SAD alone, then motion2 / motion3 =
 * min(prev, cur) at `index - 1`. */
static int fm_hip_emit(FloatMotionStateHip *s, VmafFeatureCollector *feature_collector,
                       unsigned index, double motion_score)
{
    if (index == 0u) {
        /* First frame: no previous frame, motion2 and the debug score are 0;
         * motion3 at 0 waits for the second frame or for flush(). */
        s->prev_motion_score = 0.0;
        int err = fm_hip_append(s, feature_collector, "VMAF_feature_motion2_score", 0.0, index);
        if (s->debug && err == 0)
            err = fm_hip_append(s, feature_collector, "VMAF_feature_motion_score", 0.0, index);
        return err;
    }

    int err = 0;
    if (s->debug) {
        /* The CPU emits the debug score motion_clip()ped, fps weight included. */
        err = fm_hip_append(s, feature_collector, "VMAF_feature_motion_score",
                            fm_hip_motion_clip(s, motion_score), index);
    }
    /* The smaller of the previous frame's two SADs; at index 1 there is one. */
    const double motion2 =
        (index > 1u && s->prev_motion_score < motion_score) ? s->prev_motion_score : motion_score;
    if (index > 1u && err == 0) {
        err = fm_hip_append(s, feature_collector, "VMAF_feature_motion2_score",
                            fm_hip_motion_clip(s, motion2), index - 1u);
    }
    if (err == 0) {
        err = fm_hip_append(s, feature_collector, "VMAF_feature_motion3_score",
                            fm_hip_motion_blend_clip(s, motion2), index - 1u);
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
    const double motion_score = fm_hip_frame_score(s);

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

#ifdef HAVE_HIPCC
/* Append `name` at `index` unless it is already there. A pending collect can
 * have written the same option-derived feature and index, so the probe makes
 * flush idempotent. */
static int fm_hip_append_once(FloatMotionStateHip *s, VmafFeatureCollector *feature_collector,
                              const char *name, double score, unsigned index)
{
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&s->feature_name_dict, name, 0);
    const char *resolved_name = entry ? entry->val : name;
    double existing = 0.0;
    if (vmaf_feature_collector_get_score(feature_collector, resolved_name, &existing, index) == 0)
        return 0;
    return fm_hip_append(s, feature_collector, name, score, index);
}
#endif /* HAVE_HIPCC */

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

    /* CPU float_motion.c::flush: a one-frame run has no SAD, and its motion3
     * is 0; otherwise the tail motion2 / motion3 are motion_clip() /
     * motion_blend_clip() of the last SAD at the last frame index. */
    if (s->index == 0u) {
        const int err =
            fm_hip_append_once(s, feature_collector, "VMAF_feature_motion3_score", 0.0, 0u);
        return (err != 0) ? err : 1;
    }
    int err = fm_hip_append_once(s, feature_collector, "VMAF_feature_motion2_score",
                                 fm_hip_motion_clip(s, s->prev_motion_score), s->index);
    if (err == 0) {
        err = fm_hip_append_once(s, feature_collector, "VMAF_feature_motion3_score",
                                 fm_hip_motion_blend_clip(s, s->prev_motion_score), s->index);
    }
    return (err != 0) ? err : 1;
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return fm_hip_release(fex->priv);
}

static const char *provided_features[] = {"VMAF_feature_motion_score", "VMAF_feature_motion2_score",
                                          "VMAF_feature_motion3_score", NULL};

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
