/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Real integer_ssim feature extractor on the HIP backend (ADR-0564).
 *
 *  Replaces the prior float-arithmetic implementation that was misnamed
 *  "integer_ssim" but used an 11-tap floating-point Gaussian kernel — a
 *  different metric than the CPU integer_ssim (9-tap int64 moments).
 *
 *  This version is bit-exact with the CPU vmaf_fex_ssim:
 *    - 9-tap integer Gaussian: [2,9,28,55,68,55,28,9,2] (sigma=1.5, sum=256).
 *    - int64_t accumulators for all 5 moments (mux, muy, x2, xy, y2) + w.
 *    - Boundary-truncation: kernel window trimmed at image edges.
 *    - Final SSIM formula in double from int64 moments.
 *
 *  Two-pass design:
 *    Pass 1 (integer_ssim_hip_horiz_{8,16}bpc): horizontal 9-tap int64
 *      moment accumulation. Writes 6 x (W x H) int64 intermediate arrays.
 *    Pass 2 (integer_ssim_hip_vert_combine): vertical 9-tap int64 accumulation
 *      + SSIM double formula + per-block double/int64 partial sums.
 *  Host: ssim = sum(partials) / sum(partial_weights).
 *
 *  HIP specifics:
 *    - Wavefront-64 (GCN/RDNA): the kernel uses ISSIM_WARP_SIZE=64 shuffles.
 *    - int64 atomic: not needed, block-level reduce in shared memory.
 *    - No per-thread float atomics anywhere (determinism requirement).
 *
 *  When enable_hipcc=false (CI without ROCm), HAVE_HIPCC is undefined and
 *  init() returns -ENOSYS, same scaffold contract as other HIP consumers.
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
#include "log.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "integer_ssim_hip.h"

/* ------------------------------------------------------------------ */
/* Block geometry constants (must match integer_ssim_score.hip)        */
/* ------------------------------------------------------------------ */

#define ISSIM_HIP_BLOCK_X 16u
#define ISSIM_HIP_BLOCK_Y 8u

/* ------------------------------------------------------------------ */
/* HIP-to-errno translation                                            */
/* ------------------------------------------------------------------ */

static int issim_hip_rc(hipError_t rc)
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

typedef struct IssimStateHip {
    VmafHipKernelLifecycle lc;
    /* Two readback slots: double partials + int64 partial weights. */
    VmafHipKernelReadback rb_ssim; /* double partials */
    VmafHipKernelReadback rb_wgt;  /* int64 partial weights */
    VmafHipContext *ctx;

    /* Six int64 intermediate device buffers for horizontal moment pass.
     * Sized (width * height * sizeof(int64_t)) each. */
    void *d_mux;
    void *d_muy;
    void *d_x2;
    void *d_xy;
    void *d_y2;
    void *d_w;

    /* Staging buffers: CPU luma planes -> device (HtoD). One per
     * ref/cmp (luma-only). Sized width * height * bpp. */
    void *ref_in;
    void *cmp_in;

    /* HIP module + per-bpc horiz kernel + vert-combine kernel handles. */
    hipModule_t module;
    hipFunction_t func_horiz_8;
    hipFunction_t func_horiz_16;
    hipFunction_t func_vert;

    unsigned block_count;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned grid_x;
    unsigned grid_y;

    unsigned index;
    VmafDictionary *feature_name_dict;
} IssimStateHip;

static const VmafOption options[] = {
    {0},
};

/* ------------------------------------------------------------------ */
/* HAVE_HIPCC helpers                                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC

static int issim_hip_module_load(IssimStateHip *s)
{
    hipError_t hip_rc = hipModuleLoadData(&s->module, integer_ssim_score_hsaco);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    hip_rc = hipModuleGetFunction(&s->func_horiz_8, s->module, "integer_ssim_hip_horiz_8bpc");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipModuleGetFunction(&s->func_horiz_16, s->module, "integer_ssim_hip_horiz_16bpc");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipModuleGetFunction(&s->func_vert, s->module, "integer_ssim_hip_vert_combine");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return issim_hip_rc(hip_rc);
    }
    return 0;
}

static int issim_hip_bufs_alloc(IssimStateHip *s)
{
    const size_t int64_plane_bytes = (size_t)s->width * s->height * sizeof(int64_t);
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const size_t stage_bytes = (size_t)s->width * s->height * bpp;

    hipError_t hip_rc;
    hip_rc = hipMalloc(&s->d_mux, int64_plane_bytes);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);
    hip_rc = hipMalloc(&s->d_muy, int64_plane_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_mux);
        s->d_mux = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->d_x2, int64_plane_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_muy);
        s->d_muy = NULL;
        (void)hipFree(s->d_mux);
        s->d_mux = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->d_xy, int64_plane_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_x2);
        s->d_x2 = NULL;
        (void)hipFree(s->d_muy);
        s->d_muy = NULL;
        (void)hipFree(s->d_mux);
        s->d_mux = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->d_y2, int64_plane_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_xy);
        s->d_xy = NULL;
        (void)hipFree(s->d_x2);
        s->d_x2 = NULL;
        (void)hipFree(s->d_muy);
        s->d_muy = NULL;
        (void)hipFree(s->d_mux);
        s->d_mux = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->d_w, int64_plane_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_y2);
        s->d_y2 = NULL;
        (void)hipFree(s->d_xy);
        s->d_xy = NULL;
        (void)hipFree(s->d_x2);
        s->d_x2 = NULL;
        (void)hipFree(s->d_muy);
        s->d_muy = NULL;
        (void)hipFree(s->d_mux);
        s->d_mux = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->ref_in, stage_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_w);
        s->d_w = NULL;
        (void)hipFree(s->d_y2);
        s->d_y2 = NULL;
        (void)hipFree(s->d_xy);
        s->d_xy = NULL;
        (void)hipFree(s->d_x2);
        s->d_x2 = NULL;
        (void)hipFree(s->d_muy);
        s->d_muy = NULL;
        (void)hipFree(s->d_mux);
        s->d_mux = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->cmp_in, stage_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->ref_in);
        s->ref_in = NULL;
        (void)hipFree(s->d_w);
        s->d_w = NULL;
        (void)hipFree(s->d_y2);
        s->d_y2 = NULL;
        (void)hipFree(s->d_xy);
        s->d_xy = NULL;
        (void)hipFree(s->d_x2);
        s->d_x2 = NULL;
        (void)hipFree(s->d_muy);
        s->d_muy = NULL;
        (void)hipFree(s->d_mux);
        s->d_mux = NULL;
        return issim_hip_rc(hip_rc);
    }
    return 0;
}

static void issim_hip_bufs_free(IssimStateHip *s)
{
    if (s->cmp_in != NULL) {
        (void)hipFree(s->cmp_in);
        s->cmp_in = NULL;
    }
    if (s->ref_in != NULL) {
        (void)hipFree(s->ref_in);
        s->ref_in = NULL;
    }
    if (s->d_w != NULL) {
        (void)hipFree(s->d_w);
        s->d_w = NULL;
    }
    if (s->d_y2 != NULL) {
        (void)hipFree(s->d_y2);
        s->d_y2 = NULL;
    }
    if (s->d_xy != NULL) {
        (void)hipFree(s->d_xy);
        s->d_xy = NULL;
    }
    if (s->d_x2 != NULL) {
        (void)hipFree(s->d_x2);
        s->d_x2 = NULL;
    }
    if (s->d_muy != NULL) {
        (void)hipFree(s->d_muy);
        s->d_muy = NULL;
    }
    if (s->d_mux != NULL) {
        (void)hipFree(s->d_mux);
        s->d_mux = NULL;
    }
}

static int issim_hip_launch_horiz(IssimStateHip *s, hipStream_t str)
{
    const ptrdiff_t ref_stride = (ptrdiff_t)(s->width * ((s->bpc <= 8u) ? 1u : 2u));
    void *args[] = {
        &s->ref_in, (void *)&ref_stride,
        &s->cmp_in, (void *)&ref_stride,
        &s->d_mux,  &s->d_muy,
        &s->d_x2,   &s->d_xy,
        &s->d_y2,   &s->d_w,
        &s->width,  &s->height,
    };
    hipFunction_t func = (s->bpc == 8u) ? s->func_horiz_8 : s->func_horiz_16;
    hipError_t hip_rc = hipModuleLaunchKernel(func, s->grid_x, s->grid_y, 1u, ISSIM_HIP_BLOCK_X,
                                              ISSIM_HIP_BLOCK_Y, 1u, 0, str, args, NULL);
    return issim_hip_rc(hip_rc);
}

static int issim_hip_launch_vert_readback(IssimStateHip *s, hipStream_t str)
{
    const int64_t samplemax = (int64_t)((1u << s->bpc) - 1u);
    void *args2[] = {
        &s->d_mux,          &s->d_muy,         &s->d_x2,  &s->d_xy,   &s->d_y2,           &s->d_w,
        &s->rb_ssim.device, &s->rb_wgt.device, &s->width, &s->height, (void *)&samplemax,
    };
    hipError_t hip_rc =
        hipModuleLaunchKernel(s->func_vert, s->grid_x, s->grid_y, 1u, ISSIM_HIP_BLOCK_X,
                              ISSIM_HIP_BLOCK_Y, 1u, 0, str, args2, NULL);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    hip_rc = hipEventRecord((hipEvent_t)s->lc.submit, str);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    const size_t ssim_bytes = (size_t)s->block_count * sizeof(double);
    hip_rc = hipMemcpyAsync(s->rb_ssim.host_pinned, s->rb_ssim.device, ssim_bytes,
                            hipMemcpyDeviceToHost, str);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    const size_t wgt_bytes = (size_t)s->block_count * sizeof(int64_t);
    hip_rc = hipMemcpyAsync(s->rb_wgt.host_pinned, s->rb_wgt.device, wgt_bytes,
                            hipMemcpyDeviceToHost, str);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* init / close                                                        */
/* ------------------------------------------------------------------ */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    IssimStateHip *s = fex->priv;

    if (w < 1u || h < 1u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_hip: zero-dimension input %ux%u\n", w, h);
        return -EINVAL;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->grid_x = (w + ISSIM_HIP_BLOCK_X - 1u) / ISSIM_HIP_BLOCK_X;
    s->grid_y = (h + ISSIM_HIP_BLOCK_Y - 1u) / ISSIM_HIP_BLOCK_Y;
    s->block_count = s->grid_x * s->grid_y;

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err != 0)
        return err;

    err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0)
        goto fail_after_ctx;

    err = vmaf_hip_kernel_readback_alloc(&s->rb_ssim, s->ctx,
                                         (size_t)s->block_count * sizeof(double));
    if (err != 0)
        goto fail_after_lc;

    err = vmaf_hip_kernel_readback_alloc(&s->rb_wgt, s->ctx,
                                         (size_t)s->block_count * sizeof(int64_t));
    if (err != 0)
        goto fail_after_rb_ssim;

#ifdef HAVE_HIPCC
    err = issim_hip_module_load(s);
    if (err != 0)
        goto fail_after_rb_wgt;
    err = issim_hip_bufs_alloc(s);
    if (err != 0)
        goto fail_after_module;
#else
    err = -ENOSYS;
    if (err != 0)
        goto fail_after_rb_wgt;
#endif

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        err = -ENOMEM;
#ifdef HAVE_HIPCC
        issim_hip_bufs_free(s);
        goto fail_after_module;
#else
        goto fail_after_rb_wgt;
#endif
    }
    return 0;

#ifdef HAVE_HIPCC
fail_after_module:
    (void)hipModuleUnload(s->module);
    s->module = NULL;
#endif
fail_after_rb_wgt:
    (void)vmaf_hip_kernel_readback_free(&s->rb_wgt, s->ctx);
fail_after_rb_ssim:
    (void)vmaf_hip_kernel_readback_free(&s->rb_ssim, s->ctx);
fail_after_lc:
    (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
fail_after_ctx:
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    IssimStateHip *s = fex->priv;
    int rc = 0;

#ifdef HAVE_HIPCC
    issim_hip_bufs_free(s);
    if (s->module != NULL) {
        int e = issim_hip_rc(hipModuleUnload(s->module));
        s->module = NULL;
        if (rc == 0)
            rc = e;
    }
#endif

    int e = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    if (rc == 0)
        rc = e;
    e = vmaf_hip_kernel_readback_free(&s->rb_wgt, s->ctx);
    if (rc == 0)
        rc = e;
    e = vmaf_hip_kernel_readback_free(&s->rb_ssim, s->ctx);
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
    IssimStateHip *s = fex->priv;
    s->index = index;

    const hipStream_t str = (hipStream_t)s->lc.str;
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const ptrdiff_t row_w = (ptrdiff_t)(s->width * bpp);

    hipError_t hip_rc =
        hipMemcpy2DAsync(s->ref_in, (size_t)row_w, ref_pic->data[0], (size_t)ref_pic->stride[0],
                         (size_t)row_w, (size_t)s->height, hipMemcpyHostToDevice, str);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    hip_rc =
        hipMemcpy2DAsync(s->cmp_in, (size_t)row_w, dist_pic->data[0], (size_t)dist_pic->stride[0],
                         (size_t)row_w, (size_t)s->height, hipMemcpyHostToDevice, str);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    int err = issim_hip_launch_horiz(s, str);
    if (err != 0)
        return err;

    return issim_hip_launch_vert_readback(s, str);
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
    IssimStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

    const double *ssim_partials = (const double *)s->rb_ssim.host_pinned;
    const int64_t *wgt_partials = (const int64_t *)s->rb_wgt.host_pinned;

    double total_ssim = 0.0;
    int64_t total_wgt = 0LL;
    for (unsigned i = 0; i < s->block_count; i++) {
        total_ssim += ssim_partials[i];
        total_wgt += wgt_partials[i];
    }
    if (total_wgt == 0LL)
        return -EINVAL;

    const double score = total_ssim / (double)total_wgt;

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict, "ssim",
                                                   score, index);
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"ssim", NULL};

/* Real integer_ssim HIP extractor (ADR-0564). Bit-exact with the CPU
 * vmaf_fex_ssim using 9-tap int64 moments and boundary-truncation.
 * Load-bearing: declared via extern in feature_extractor.c. */
// NOLINTNEXTLINE(misc-use-internal-linkage)
VmafFeatureExtractor vmaf_fex_integer_ssim_hip = {
    .name = "integer_ssim_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(IssimStateHip),
    .provided_features = provided_features,
    /* Pictures arrive as CPU VmafPictures; submit() does explicit HtoD.
     * Same posture as all other HIP consumers (no VMAF_FEATURE_EXTRACTOR_HIP
     * flag). */
    .flags = 0,
    .chars =
        {
            .n_dispatches_per_frame = 2,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
