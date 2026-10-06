/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_psnr feature extractor on the HIP backend — first real kernel
 *  (T7-10b / ADR-0254). Second kernel-template consumer.
 *  enable_chroma option ported from CUDA twin (ADR-0469).
 *
 *  Mirrors `core/src/feature/cuda/float_psnr_cuda.c` call-graph-for-
 *  call-graph. The device-side kernel lives in
 *  `core/src/feature/hip/float_psnr/float_psnr_score.hip` and is
 *  loaded at init() time via the HIP module API (`hipModuleLoadData` +
 *  `hipModuleGetFunction`), the direct analog of CUDA's
 *  `cuModuleLoadData` + `cuModuleGetFunction` used by the twin.
 *
 *  When `enable_hipcc=false` (e.g. a CI agent without ROCm), `HAVE_HIPCC`
 *  is undefined and `init()` returns -ENOSYS — same scaffold contract as
 *  the pre-runtime posture (registered, runtime not ready).
 *
 *  Algorithm:
 *    - Per-pixel float (ref - dis)^2, as float_psnr.c forms it, added per
 *      block of FPSNR_BX pixels of one row as an integer in units of
 *      1 / scaler^2 (scaler = 2^(bpc - 8)): one uint64 per block.
 *    - Host forms each row's exact sum and adds the rows into a double in
 *      the CPU's order (feature/float_psnr_rows.h, ADR-1499):
 *        noise = sum / scaler^2 / (w * h)
 *        score = 10 * log10(peak^2 / max(noise, 1e-10)), clamped.
 *
 *  The score is the CPU extractor's bit for bit (ADR-1440, ADR-1499): the
 *  device's integer sums and the host's row sums of them are exact, as the
 *  CPU's row sums of float terms are, and the rows are added into a double
 *  in the CPU's order, so the adds that round past 2^53 units (16 bits only,
 *  a mean squared error above 2^37 / (w * h) on the 8-bit scale) round as
 *  the CPU's do.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <hip/hip_runtime_api.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "../../hip/common.h"
#include "../../hip/hip_handle.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "../../hip/shared_frame.h"
#include "../float_psnr_rows.h"
#include "float_psnr_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* ------------------------------------------------------------------ */
/* HIP-to-errno translation                                            */
/* ------------------------------------------------------------------ */

static int hip_err(hipError_t rc)
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

/* ------------------------------------------------------------------ */
/* Private state                                                       */
/* ------------------------------------------------------------------ */

/* One block per FPSNR_BX pixels of one row, one uint64 sum per block
 * (float_psnr_score.hip, ADR-1499). */
#define FPSNR_BX 256u
#define FPSNR_BY 1u

typedef struct FloatPsnrStateHip {
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb;
    /* This frame's luma planes on the device, ref + dis: the context's shared
     * frame, or `planes`' own buffers when there is none (ADR-1408). */
    void *ref_in;
    void *dis_in;
    VmafHipPlaneSource planes;
    VmafHipSharedFrame *hip_frame;
    VmafHipContext *ctx;
    /* HIP module + per-bpc kernel function handles. */
    hipModule_t module;
    hipFunction_t funcbpc8;
    hipFunction_t funcbpc16;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    unsigned wg_count;
    double peak;
    double psnr_max;
    /* `enable_chroma` option: when false, only luma is computed.
     * Default true mirrors CPU float_psnr.c — see ADR-0469. */
    bool enable_chroma;
    /* `uncapped` option: mirrors CPU float_psnr.c. When true, psnr_max
     * keeps only its zero-noise infinity-sentinel role and stops
     * truncating genuinely computed values. Default false keeps every
     * shipped score unchanged. See ADR-1193 / T-UPSTREAM-1109. */
    bool uncapped;
    VmafDictionary *feature_name_dict;
} FloatPsnrStateHip;

static const VmafOption options[] = {{
                                         .name = "enable_chroma",
                                         .help = "enable calculation for chroma channels",
                                         .offset = offsetof(FloatPsnrStateHip, enable_chroma),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = true,
                                     },
                                     {
                                         .name = "uncapped",
                                         .help = "report the true PSNR instead of truncating at "
                                                 "the psnr_max ceiling (a zero-noise pair still "
                                                 "reports psnr_max)",
                                         .offset = offsetof(FloatPsnrStateHip, uncapped),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                     },
                                     {0}};

/* Bit-depth → peak / clamp table (mirrors the CUDA twin). Extracted to
 * keep init_fex_hip under the 60-line readability-function-size limit. */
static int float_psnr_hip_resolve_peak_clamp(FloatPsnrStateHip *s, unsigned bpc)
{
    if (bpc < 8u || bpc > 16u) {
        return -EINVAL;
    }
    /* The CPU's expression (float_psnr.c): (2^bpc - 1) / 2^(bpc - 8) and 6 * bpc + 12, every depth
     * the engine reads. The doubles equal the former per-depth literals at 8, 10, 12 and 16. */
    s->peak = (double)((1u << bpc) - 1u) / (double)(1u << (bpc - 8u));
    s->psnr_max = 6.0 * (double)bpc + 12.0;
    return 0;
}

/* Size of the block-sum buffer on the device and on the host. */
static size_t float_psnr_hip_partials_bytes(const FloatPsnrStateHip *s)
{
    return (size_t)s->wg_count * sizeof(uint64_t);
}

#ifdef HAVE_HIPCC
/* Load the HSACO fat binary and resolve both kernel function handles. On
 * failure the module is unloaded again and `s->module` is NULL. */
static int float_psnr_hip_module_load(FloatPsnrStateHip *s)
{
    hipError_t hip_rc = hipModuleLoadData(&s->module, float_psnr_score_hsaco);
    if (hip_rc != hipSuccess)
        return hip_err(hip_rc);

    hip_rc = hipModuleGetFunction(&s->funcbpc8, s->module, "float_psnr_kernel_8bpc");
    if (hip_rc == hipSuccess)
        hip_rc = hipModuleGetFunction(&s->funcbpc16, s->module, "float_psnr_kernel_16bpc");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return hip_err(hip_rc);
}

/* Launch the per-bpc kernel on `str`. */
static int float_psnr_hip_launch_kernel(FloatPsnrStateHip *s, ptrdiff_t plane_pitch,
                                        hipStream_t str)
{
    const unsigned gx = (s->frame_w + FPSNR_BX - 1u) / FPSNR_BX;
    const unsigned gy = (s->frame_h + FPSNR_BY - 1u) / FPSNR_BY;
    void *partials_dev = s->rb.device;
    /* The 16bpc kernel takes one more argument than the 8bpc one: `bpc`. */
    void *args8[] = {(void *)&s->ref_in,   (void *)&s->dis_in,    (void *)&plane_pitch,
                     (void *)&plane_pitch, (void *)&partials_dev, (void *)&s->frame_w,
                     (void *)&s->frame_h};
    void *args16[] = {(void *)&s->ref_in,   (void *)&s->dis_in,    (void *)&plane_pitch,
                      (void *)&plane_pitch, (void *)&partials_dev, (void *)&s->frame_w,
                      (void *)&s->frame_h,  (void *)&s->bpc};
    const bool is8 = (s->bpc == 8u);
    return hip_err(hipModuleLaunchKernel(is8 ? s->funcbpc8 : s->funcbpc16, gx, gy, 1, FPSNR_BX,
                                         FPSNR_BY, 1, 0, str, is8 ? args8 : args16, NULL));
}

/*
 * Per-frame submit body: HtoD copies, zero the partials, kernel launch,
 * submit-event record, DtoH copy.
 */
static int float_psnr_hip_launch(FloatPsnrStateHip *s, VmafPicture *ref_pic, VmafPicture *dist_pic)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const ptrdiff_t plane_pitch = (ptrdiff_t)(s->frame_w * bpp);
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);

    /* Returns once both pictures are read: the caller recycles them when
     * submit() returns (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
    int err = vmaf_hip_plane_source_acquire_luma(&s->planes, s->hip_frame, ref_pic, dist_pic,
                                                 s->lc.str, &s->ref_in, &s->dis_in);
    /* The partials are cleared after the upload, like every HIP twin's
     * buffers (ADR-1427). The kernel writes every partial, so the frame
     * does not depend on this clear. */
    if (err == 0) {
        err = hip_err(hipMemsetAsync(s->rb.device, 0, float_psnr_hip_partials_bytes(s), str));
    }
    if (err == 0)
        err = float_psnr_hip_launch_kernel(s, plane_pitch, str);
    if (err != 0)
        return err;

    /* Record submit event, DtoH copy of partials, record finished event. */
    hipError_t hip_rc = hipEventRecord(vmaf_hip_event_of(s->lc.submit), str);
    if (hip_rc == hipSuccess) {
        hip_rc = hipMemcpyAsync(s->rb.host_pinned, s->rb.device, float_psnr_hip_partials_bytes(s),
                                hipMemcpyDeviceToHost, str);
    }
    if (hip_rc != hipSuccess)
        return hip_err(hip_rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

/* Let go of the planes and unload the module. Safe with NULL handles.
 * Returns the first error. */
static int float_psnr_hip_module_free(FloatPsnrStateHip *s)
{
    vmaf_hip_plane_source_close(&s->planes);
    s->ref_in = NULL;
    s->dis_in = NULL;
    int rc = 0;
    if (s->module != NULL) {
        const int e = hip_err(hipModuleUnload(s->module));
        s->module = NULL;
        if (rc == 0)
            rc = e;
    }
    return rc;
}
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* init / close                                                        */
/* ------------------------------------------------------------------ */

/* Tear down everything init() may have set up. Every step tolerates a handle
 * that was never created, so this serves both a failed init() and close().
 * The stream is drained first, so no kernel still uses a buffer. Returns the
 * first error. */
static int float_psnr_hip_release(FloatPsnrStateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    int e = 0;
#ifdef HAVE_HIPCC
    e = float_psnr_hip_module_free(s);
    if (rc == 0)
        rc = e;
#endif /* HAVE_HIPCC */
    e = vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
    if (rc == 0)
        rc = e;
    if (s->feature_name_dict != NULL) {
        e = vmaf_dictionary_free(&s->feature_name_dict);
        if (rc == 0)
            rc = e;
    }
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return rc;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    FloatPsnrStateHip *s = fex->priv;
    /* float_psnr operates on luma only; chroma planes are not used.
     * `enable_chroma` is wired here so callers can set it (ADR-0469)
     * and the option is not silently dropped. Suppress unused-param. */
    (void)pix_fmt;

    int err = float_psnr_hip_resolve_peak_clamp(s, bpc);
    if (err != 0)
        return err;

    s->bpc = bpc;
    s->frame_w = w;
    s->frame_h = h;
    s->wg_count = ((w + FPSNR_BX - 1u) / FPSNR_BX) * ((h + FPSNR_BY - 1u) / FPSNR_BY);

    err = vmaf_hip_context_new(&s->ctx, fex->hip_device_index);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err == 0)
        err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx, float_psnr_hip_partials_bytes(s));
#ifdef HAVE_HIPCC
    if (err == 0)
        err = float_psnr_hip_module_load(s);
#else
    if (err == 0)
        err = -ENOSYS;
#endif
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)float_psnr_hip_release(s);
    return err;
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return float_psnr_hip_release(fex->priv);
}

/* ------------------------------------------------------------------ */
/* submit / collect                                                    */
/* ------------------------------------------------------------------ */

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;

#ifndef HAVE_HIPCC
    (void)fex;
    (void)ref_pic;
    (void)dist_pic;
    return -ENOSYS;
#else
    FloatPsnrStateHip *s = fex->priv;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    s->hip_frame = fex->hip_frame;
    /* Pictures arrive as host VmafPictures (ADR-0530).
     * float_psnr_hip_launch() gets the luma planes on the device, launches
     * the kernel, copies partials device->host, and records the finished
     * event. */
    return float_psnr_hip_launch(s, ref_pic, dist_pic);
#endif /* HAVE_HIPCC */
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    FloatPsnrStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

    /* Every block sum is the exact integer sum of a segment of one row, in
     * units of 1 / scaler^2 (ADR-1440); the CPU's sum is those rows' exact
     * sums added row after row into a double, past 2^53 units too
     * (ADR-1499). */
    const unsigned per_row = (s->frame_w + FPSNR_BX - 1u) / FPSNR_BX;
    const double total =
        vmaf_float_psnr_row_noise((const uint64_t *)s->rb.host_pinned, s->frame_h, per_row);
    const double scaler = (double)(1u << (s->bpc - 8u));

    const double n_pix = (double)s->frame_w * (double)s->frame_h;
    const double noise = (total / (scaler * scaler)) / n_pix;
    /* Match CPU float_psnr.c — a zero-noise pair reports psnr_max as the
     * infinity sentinel; the truncation at psnr_max applies only when
     * `uncapped` is false. See ADR-1193 / T-UPSTREAM-1109. */
    const double eps = 1e-10;
    const double max_noise = noise > eps ? noise : eps;
    double score;
    if (!s->uncapped) {
        /* Pre-ADR-1193 expression verbatim — bit-identical default. */
        score = 10.0 * log10(s->peak * s->peak / max_noise);
        if (score > s->psnr_max)
            score = s->psnr_max;
    } else if (noise <= 0.0) {
        score = s->psnr_max; /* infinity sentinel */
    } else {
        score = 10.0 * log10(s->peak * s->peak / max_noise);
    }

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "float_psnr", score, index);
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"float_psnr", NULL};

/* Load-bearing: declared `extern` in feature_extractor.c's
 * `feature_extractor_list[]` under `#if HAVE_HIP`. Making this static
 * would unlink the extractor from the registry. Same pattern as every
 * CUDA / SYCL / Vulkan extractor (see vmaf_fex_float_psnr_cuda). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_psnr_hip = {
    .name = "float_psnr_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(FloatPsnrStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
