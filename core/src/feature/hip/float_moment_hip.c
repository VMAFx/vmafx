/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_moment feature extractor on the HIP backend — fourth consumer
 *  of `libvmaf/src/hip/kernel_template.h` (T7-10b batch-3 / ADR-0374).
 *
 *  Mirrors `libvmaf/src/feature/cuda/integer_moment_cuda.c`
 *  call-graph-for-call-graph: same private-state struct shape, same
 *  init/submit/collect/close lifecycle, same template helper
 *  invocations. Single dispatch per frame; emits all four metrics
 *  (`float_moment_ref{1st,2nd}`, `float_moment_dis{1st,2nd}`) in one
 *  kernel pass via four uint64 atomic counters — same precision posture
 *  as the CUDA twin.
 *
 *  HIP adaptation from CUDA:
 *  - `hipModuleLoadData` / `hipModuleGetFunction` / `hipModuleLaunchKernel`
 *    instead of `cuModuleLoadData` / `cuModuleGetFunction` / `cuLaunchKernel`.
 *  - Four uint64 atomic accumulators (device readback) — zeroed via
 *    `hipMemsetAsync` before each dispatch.
 *  - Luma planes copied HtoD via `hipMemcpy2DAsync` (pictures arrive as
 *    CPU VmafPictures; VMAF_FEATURE_EXTRACTOR_HIP flag cleared for T7-10b).
 *
 *  When `enable_hipcc=false` (e.g. a CI agent without ROCm), `HAVE_HIPCC`
 *  is undefined and `init()` returns -ENOSYS — same scaffold contract as
 *  the pre-runtime posture.
 *
 *  Algorithm (mirrors CUDA twin):
 *    - Four uint64 atomic counters [ref1st, dis1st, ref2nd, dis2nd]
 *      reduced per-frame, divided by `w*h` on the host to recover four
 *      mean / second-moment metrics.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "float_moment_hip.h"

/* Number of uint64 atomic counters the runtime kernel emits per
 * frame: [ref1st, dis1st, ref2nd, dis2nd]. Pinned by the CUDA twin's
 * `cuda/integer_moment_cuda.c` so the eventual cross-backend numeric
 * gate has nothing fork-specific to track. */
#define MOMENT_HIP_COUNTERS 4u

/* Block dimensions for the moment kernel (mirrors CUDA twin). */
#define MOMENT_HIP_BX 16u
#define MOMENT_HIP_BY 16u

/* ------------------------------------------------------------------ */
/* HIP-to-errno translation                                            */
/* ------------------------------------------------------------------ */

static int moment_hip_rc(hipError_t rc)
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

typedef struct MomentStateHip {
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb; /* device: 4 x uint64 accumulators;
                                * host_pinned: readback slot */
    VmafHipContext *ctx;
    /* HIP module + per-bpc kernel function handles. */
    hipModule_t module;
    hipFunction_t funcbpc8;
    hipFunction_t funcbpc16;
    /* Device-side staging buffers (luma planes, ref + dis). */
    void *ref_in;
    void *dis_in;
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    VmafDictionary *feature_name_dict;
} MomentStateHip;

static const VmafOption options[] = {{0}};

/* ------------------------------------------------------------------ */
/* HAVE_HIPCC helpers                                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC
/*
 * Load the HSACO fat binary, resolve both kernel function handles, and
 * allocate luma-plane staging buffers. Extracted to keep init_fex_hip
 * under the 60-line readability-function-size limit.
 *
 * On failure: s->module, s->ref_in, s->dis_in are left NULL / unset;
 * caller unwinds via fail_after_rb.
 */
static int moment_hip_module_load(MomentStateHip *s, unsigned bpc, unsigned w, unsigned h)
{
    hipError_t hip_rc = hipModuleLoadData(&s->module, moment_score_hsaco);
    if (hip_rc != hipSuccess)
        return moment_hip_rc(hip_rc);

    hip_rc = hipModuleGetFunction(&s->funcbpc8, s->module, "calculate_moment_hip_kernel_8bpc");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return moment_hip_rc(hip_rc);
    }
    hip_rc = hipModuleGetFunction(&s->funcbpc16, s->module, "calculate_moment_hip_kernel_16bpc");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return moment_hip_rc(hip_rc);
    }

    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t plane_bytes = (size_t)w * h * bpp;
    hip_rc = hipMalloc(&s->ref_in, plane_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return moment_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->dis_in, plane_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->ref_in);
        s->ref_in = NULL;
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return moment_hip_rc(hip_rc);
    }
    return 0;
}

/*
 * Per-frame submit body: zero the four uint64 accumulators, HtoD copies
 * of both luma planes, kernel launch, DtoH readback. Extracted to keep
 * submit_fex_hip under the 60-line readability-function-size limit.
 */
static int moment_hip_launch(MomentStateHip *s, VmafPicture *ref_pic, VmafPicture *dist_pic)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const ptrdiff_t row_w = (ptrdiff_t)(s->frame_w * bpp);
    const hipStream_t str = (hipStream_t)s->lc.str;

    /* Zero the four uint64 atomic accumulators before dispatch. */
    hipError_t hip_rc =
        hipMemsetAsync(s->rb.device, 0, (size_t)MOMENT_HIP_COUNTERS * sizeof(uint64_t), str);
    if (hip_rc != hipSuccess)
        return moment_hip_rc(hip_rc);

    hip_rc =
        hipMemcpy2DAsync(s->ref_in, (size_t)row_w, ref_pic->data[0], (size_t)ref_pic->stride[0],
                         (size_t)row_w, (size_t)s->frame_h, hipMemcpyHostToDevice, str);
    if (hip_rc != hipSuccess)
        return moment_hip_rc(hip_rc);

    hip_rc =
        hipMemcpy2DAsync(s->dis_in, (size_t)row_w, dist_pic->data[0], (size_t)dist_pic->stride[0],
                         (size_t)row_w, (size_t)s->frame_h, hipMemcpyHostToDevice, str);
    if (hip_rc != hipSuccess)
        return moment_hip_rc(hip_rc);

    const unsigned gx = (s->frame_w + MOMENT_HIP_BX - 1u) / MOMENT_HIP_BX;
    const unsigned gy = (s->frame_h + MOMENT_HIP_BY - 1u) / MOMENT_HIP_BY;
    void *sums_dev = s->rb.device;
    if (s->bpc == 8u) {
        void *args[] = {
            &s->ref_in, &s->dis_in,  (void *)&row_w, (void *)&row_w,
            &sums_dev,  &s->frame_w, &s->frame_h,
        };
        hip_rc = hipModuleLaunchKernel(s->funcbpc8, gx, gy, 1u, MOMENT_HIP_BX, MOMENT_HIP_BY, 1u, 0,
                                       str, args, NULL);
    } else {
        /* 16bpc kernel has the same 7-arg signature as 8bpc — no bpc arg.
         * The kernel reads raw uint16_t pixels regardless of bpc depth;
         * the host already sized ref_in/dis_in at 2 bytes per pixel. */
        void *args[] = {
            &s->ref_in, &s->dis_in,  (void *)&row_w, (void *)&row_w,
            &sums_dev,  &s->frame_w, &s->frame_h,
        };
        hip_rc = hipModuleLaunchKernel(s->funcbpc16, gx, gy, 1u, MOMENT_HIP_BX, MOMENT_HIP_BY, 1u,
                                       0, str, args, NULL);
    }
    if (hip_rc != hipSuccess)
        return moment_hip_rc(hip_rc);

    /* Record submit event, DtoH copy of the four uint64 accumulators,
     * record finished event. */
    hip_rc = hipEventRecord((hipEvent_t)s->lc.submit, str);
    if (hip_rc != hipSuccess)
        return moment_hip_rc(hip_rc);

    hip_rc =
        hipMemcpyAsync(s->rb.host_pinned, s->rb.device,
                       (size_t)MOMENT_HIP_COUNTERS * sizeof(uint64_t), hipMemcpyDeviceToHost, str);
    if (hip_rc != hipSuccess)
        return moment_hip_rc(hip_rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}
#endif /* HAVE_HIPCC */

/* Free staging buffers + module. Safe to call with NULL handles.
 * Defined outside HAVE_HIPCC so init_fex_hip's fail_after_mod label
 * can call it unconditionally (mirrors float_psnr_hip_module_free). */
static void moment_hip_module_free(MomentStateHip *s)
{
#ifdef HAVE_HIPCC
    if (s->dis_in != NULL) {
        (void)hipFree(s->dis_in);
        s->dis_in = NULL;
    }
    if (s->ref_in != NULL) {
        (void)hipFree(s->ref_in);
        s->ref_in = NULL;
    }
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
#else
    (void)s;
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* init / close                                                        */
/* ------------------------------------------------------------------ */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    MomentStateHip *s = fex->priv;

    s->bpc = bpc;
    s->frame_w = w;
    s->frame_h = h;

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err != 0)
        return err;

    err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0)
        goto fail_after_ctx;

    err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx,
                                         (size_t)MOMENT_HIP_COUNTERS * sizeof(uint64_t));
    if (err != 0)
        goto fail_after_lc;

#ifdef HAVE_HIPCC
    err = moment_hip_module_load(s, bpc, w, h);
#else
    err = -ENOSYS;
#endif
    if (err != 0)
        goto fail_after_rb;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        err = -ENOMEM;
        goto fail_after_mod;
    }
    return 0;

fail_after_mod:
    moment_hip_module_free(s);
fail_after_rb:
    (void)vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
fail_after_lc:
    (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
fail_after_ctx:
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    MomentStateHip *s = fex->priv;
    int rc = 0;

#ifdef HAVE_HIPCC
    if (s->dis_in != NULL) {
        int e = moment_hip_rc(hipFree(s->dis_in));
        s->dis_in = NULL;
        if (rc == 0)
            rc = e;
    }
    if (s->ref_in != NULL) {
        int e = moment_hip_rc(hipFree(s->ref_in));
        s->ref_in = NULL;
        if (rc == 0)
            rc = e;
    }
    if (s->module != NULL) {
        int e = moment_hip_rc(hipModuleUnload(s->module));
        s->module = NULL;
        if (rc == 0)
            rc = e;
    }
#endif /* HAVE_HIPCC */

    int e = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    if (rc == 0)
        rc = e;
    e = vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
    if (rc == 0)
        rc = e;
    if (s->feature_name_dict != NULL) {
        e = vmaf_dictionary_free(&s->feature_name_dict);
        if (rc == 0)
            rc = e;
    }
    if (s->ctx != NULL) {
        vmaf_hip_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* submit / collect                                                    */
/* ------------------------------------------------------------------ */

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

#ifndef HAVE_HIPCC
    (void)fex;
    (void)ref_pic;
    (void)dist_pic;
    (void)index;
    return -ENOSYS;
#else
    MomentStateHip *s = fex->priv;
    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    /* VMAF_FEATURE_EXTRACTOR_HIP flag is not yet set (T7-10b posture),
     * so pictures arrive as CPU VmafPictures. moment_hip_launch()
     * copies luma planes host->device, launches the kernel, copies
     * four uint64 accumulators device->host, and records the finished
     * event. */
    return moment_hip_launch(s, ref_pic, dist_pic);
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
    MomentStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

    /* Read the four uint64 accumulators from the pinned host buffer,
     * divide by the pixel count to recover means / second moments.
     * Mirrors the CUDA twin's collect path. */
    const uint64_t *sums = (const uint64_t *)s->rb.host_pinned;
    const double n_pix = (double)s->frame_w * (double)s->frame_h;
    const double ref1st = (double)sums[0] / n_pix;
    const double dis1st = (double)sums[1] / n_pix;
    const double ref2nd = (double)sums[2] / n_pix;
    const double dis2nd = (double)sums[3] / n_pix;

    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "float_moment_ref1st", ref1st, index);
    if (err != 0)
        return err;
    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "float_moment_dis1st", dis1st, index);
    if (err != 0)
        return err;
    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "float_moment_ref2nd", ref2nd, index);
    if (err != 0)
        return err;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "float_moment_dis2nd", dis2nd, index);
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
    NULL,
};

/* Load-bearing: the feature extractor is registered via
 * `extern VmafFeatureExtractor vmaf_fex_float_moment_hip;` in
 * `libvmaf/src/feature/feature_extractor.c`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern every CUDA / SYCL / Vulkan feature extractor uses (see
 * e.g. `vmaf_fex_float_moment_cuda` in
 * `libvmaf/src/feature/cuda/integer_moment_cuda.c`). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_moment_hip = {
    .name = "float_moment_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(MomentStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    /* 1 dispatch/frame, reduction-dominated; AUTO + 1080p area
     * matches the CUDA twin's profile. */
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
