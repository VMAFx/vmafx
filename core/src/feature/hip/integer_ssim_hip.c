/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  integer_ssim feature extractor on the HIP backend (ADR-0564).
 *
 *  Two-pass 11-tap separable Gaussian SSIM on AMD/HIP, mirroring the CUDA
 *  float_ssim twin (integer_ssim_cuda.c) call-graph-for-call-graph:
 *
 *    Pass 1 (calculate_integer_ssim_hip_horiz_{8,16}bpc): horizontal
 *      11-tap Gaussian over ref / cmp / ref^2 / cmp^2 / ref*cmp into five
 *      intermediate float buffers, each (W-10) x H.
 *    Pass 2 (calculate_integer_ssim_hip_vert_combine): vertical 11-tap +
 *      per-pixel SSIM combine + per-block float partial sum (warp reduce in
 *      shared memory). Output: one float per block in `partials`.
 *  Host accumulates partials in double, divides by (W-10)*(H-10).
 *
 *  HIP adaptations from CUDA:
 *  - hipModuleLoadData / hipModuleGetFunction / hipModuleLaunchKernel
 *    instead of cuModuleLoadData / cuModuleGetFunction / cuLaunchKernel.
 *  - Five intermediate float buffers allocated via hipMalloc (raw device
 *    pointers) instead of VmafCudaBuffer wrappers.
 *  - Pictures arrive as CPU VmafPictures; luma planes are copied HtoD via
 *    hipMemcpy2DAsync on the private readback stream (T7-10b posture).
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
#define ISSIM_HIP_K 11u

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
    /* Per-block float partials: device + pinned host. */
    VmafHipKernelReadback rb;
    VmafHipContext *ctx;

    /* Five intermediate float device buffers for horizontal moment pass.
     * Sized (w_horiz * h_horiz * sizeof(float)) each. */
    void *d_ref_mu;
    void *d_cmp_mu;
    void *d_ref_sq;
    void *d_cmp_sq;
    void *d_refcmp;

    /* Staging buffers: CPU luma planes -> device (HtoD). One per
     * ref/cmp (luma-only). Sized width * height * bpp. */
    void *ref_in;
    void *cmp_in;

    /* HIP module + per-bpc horiz kernel + vert-combine kernel handles. */
    hipModule_t module;
    hipFunction_t func_horiz_8;
    hipFunction_t func_horiz_16;
    hipFunction_t func_vert;

    unsigned partials_capacity;
    unsigned partials_count;

    unsigned width;
    unsigned height;
    unsigned w_horiz; /* width - (ISSIM_HIP_K - 1) — horiz output stride */
    unsigned h_horiz; /* height — horiz output height */
    unsigned w_final; /* width - (ISSIM_HIP_K - 1) — vert output width */
    unsigned h_final; /* height - (ISSIM_HIP_K - 1) — vert output height */
    unsigned bpc;
    float c1;
    float c2;

    unsigned index;
    VmafDictionary *feature_name_dict;
} IssimStateHip;

static const VmafOption options[] = {
    {0},
};

/* ------------------------------------------------------------------ */
/* Dimension initialisation helper                                     */
/* ------------------------------------------------------------------ */

static void issim_hip_init_dims(IssimStateHip *s, unsigned w, unsigned h, unsigned bpc)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->w_horiz = w - (ISSIM_HIP_K - 1u);
    s->h_horiz = h;
    s->w_final = w - (ISSIM_HIP_K - 1u);
    s->h_final = h - (ISSIM_HIP_K - 1u);

    /* SSIM stability constants: L = 255.0, K1 = 0.01, K2 = 0.03.
     * Pinned at 8-bpc scale (L=255) for cross-backend numeric parity. */
    const float L = 255.0f;
    const float K1 = 0.01f;
    const float K2 = 0.03f;
    s->c1 = (K1 * L) * (K1 * L);
    s->c2 = (K2 * L) * (K2 * L);

    const unsigned grid_x = (s->w_final + ISSIM_HIP_BLOCK_X - 1u) / ISSIM_HIP_BLOCK_X;
    const unsigned grid_y = (s->h_final + ISSIM_HIP_BLOCK_Y - 1u) / ISSIM_HIP_BLOCK_Y;
    s->partials_capacity = grid_x * grid_y;
}

/* ------------------------------------------------------------------ */
/* HAVE_HIPCC helpers                                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC

/*
 * Load the HSACO fat binary; resolve the three kernel function handles.
 * Kernel names must match the __global__ symbols in integer_ssim_score.hip
 * (all prefixed with "calculate_integer_ssim_hip_").
 */
static int issim_hip_module_load(IssimStateHip *s)
{
    hipError_t hip_rc = hipModuleLoadData(&s->module, integer_ssim_score_hsaco);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    /* Bug 1 fix: names must match the calculate_ prefix used in the HSACO. */
    hip_rc =
        hipModuleGetFunction(&s->func_horiz_8, s->module, "calculate_integer_ssim_hip_horiz_8bpc");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipModuleGetFunction(&s->func_horiz_16, s->module,
                                  "calculate_integer_ssim_hip_horiz_16bpc");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc =
        hipModuleGetFunction(&s->func_vert, s->module, "calculate_integer_ssim_hip_vert_combine");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return issim_hip_rc(hip_rc);
    }
    return 0;
}

/*
 * Allocate five intermediate float device buffers (horiz pass output) +
 * two luma staging buffers.
 *
 * Bug 2 fix: buffers are float (sizeof(float)), sized over w_horiz*h_horiz,
 * not int64_t over full width*height.
 */
static int issim_hip_bufs_alloc(IssimStateHip *s)
{
    const size_t horiz_bytes = (size_t)s->w_horiz * s->h_horiz * sizeof(float);
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const size_t stage_bytes = (size_t)s->width * s->height * bpp;

    hipError_t hip_rc;
    hip_rc = hipMalloc(&s->d_ref_mu, horiz_bytes);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);
    hip_rc = hipMalloc(&s->d_cmp_mu, horiz_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_ref_mu);
        s->d_ref_mu = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->d_ref_sq, horiz_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_cmp_mu);
        s->d_cmp_mu = NULL;
        (void)hipFree(s->d_ref_mu);
        s->d_ref_mu = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->d_cmp_sq, horiz_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_ref_sq);
        s->d_ref_sq = NULL;
        (void)hipFree(s->d_cmp_mu);
        s->d_cmp_mu = NULL;
        (void)hipFree(s->d_ref_mu);
        s->d_ref_mu = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->d_refcmp, horiz_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_cmp_sq);
        s->d_cmp_sq = NULL;
        (void)hipFree(s->d_ref_sq);
        s->d_ref_sq = NULL;
        (void)hipFree(s->d_cmp_mu);
        s->d_cmp_mu = NULL;
        (void)hipFree(s->d_ref_mu);
        s->d_ref_mu = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->ref_in, stage_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->d_refcmp);
        s->d_refcmp = NULL;
        (void)hipFree(s->d_cmp_sq);
        s->d_cmp_sq = NULL;
        (void)hipFree(s->d_ref_sq);
        s->d_ref_sq = NULL;
        (void)hipFree(s->d_cmp_mu);
        s->d_cmp_mu = NULL;
        (void)hipFree(s->d_ref_mu);
        s->d_ref_mu = NULL;
        return issim_hip_rc(hip_rc);
    }
    hip_rc = hipMalloc(&s->cmp_in, stage_bytes);
    if (hip_rc != hipSuccess) {
        (void)hipFree(s->ref_in);
        s->ref_in = NULL;
        (void)hipFree(s->d_refcmp);
        s->d_refcmp = NULL;
        (void)hipFree(s->d_cmp_sq);
        s->d_cmp_sq = NULL;
        (void)hipFree(s->d_ref_sq);
        s->d_ref_sq = NULL;
        (void)hipFree(s->d_cmp_mu);
        s->d_cmp_mu = NULL;
        (void)hipFree(s->d_ref_mu);
        s->d_ref_mu = NULL;
        return issim_hip_rc(hip_rc);
    }
    return 0;
}

/* Free all seven device buffers. Safe to call with NULL pointers. */
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
    if (s->d_refcmp != NULL) {
        (void)hipFree(s->d_refcmp);
        s->d_refcmp = NULL;
    }
    if (s->d_cmp_sq != NULL) {
        (void)hipFree(s->d_cmp_sq);
        s->d_cmp_sq = NULL;
    }
    if (s->d_ref_sq != NULL) {
        (void)hipFree(s->d_ref_sq);
        s->d_ref_sq = NULL;
    }
    if (s->d_cmp_mu != NULL) {
        (void)hipFree(s->d_cmp_mu);
        s->d_cmp_mu = NULL;
    }
    if (s->d_ref_mu != NULL) {
        (void)hipFree(s->d_ref_mu);
        s->d_ref_mu = NULL;
    }
}

/*
 * Pass 1 — horizontal 11-tap Gaussian kernel launch.
 *
 * Bug 3 fix: grid is sized over w_horiz x h_horiz (not full width x height),
 * and the horiz kernel receives the actual input width as the OOB guard. The
 * kernel's x-guard is `x >= w_horiz` which prevents reading past the row end
 * (the kernel accesses ref[y*stride + x + u] for u in [0,11); x is bounded
 * to w_horiz-1 = width-11, so max read is x+10 = width-1 — exactly in bounds).
 */
static int issim_hip_launch_horiz(IssimStateHip *s, hipStream_t str)
{
    const unsigned grid_horiz_x = (s->w_horiz + ISSIM_HIP_BLOCK_X - 1u) / ISSIM_HIP_BLOCK_X;
    const unsigned grid_horiz_y = (s->h_horiz + ISSIM_HIP_BLOCK_Y - 1u) / ISSIM_HIP_BLOCK_Y;

    const ptrdiff_t ref_stride = (ptrdiff_t)(s->width * ((s->bpc <= 8u) ? 1u : 2u));
    hipError_t hip_rc;
    if (s->bpc == 8u) {
        void *args[] = {
            &s->ref_in,   (void *)&ref_stride, &s->cmp_in,   (void *)&ref_stride,
            &s->d_ref_mu, &s->d_cmp_mu,        &s->d_ref_sq, &s->d_cmp_sq,
            &s->d_refcmp, &s->w_horiz,         &s->h_horiz,
        };
        hip_rc =
            hipModuleLaunchKernel(s->func_horiz_8, grid_horiz_x, grid_horiz_y, 1u,
                                  ISSIM_HIP_BLOCK_X, ISSIM_HIP_BLOCK_Y, 1u, 0, str, args, NULL);
    } else {
        void *args[] = {
            &s->ref_in,   (void *)&ref_stride, &s->cmp_in,   (void *)&ref_stride,
            &s->d_ref_mu, &s->d_cmp_mu,        &s->d_ref_sq, &s->d_cmp_sq,
            &s->d_refcmp, &s->w_horiz,         &s->h_horiz,  &s->bpc,
        };
        hip_rc =
            hipModuleLaunchKernel(s->func_horiz_16, grid_horiz_x, grid_horiz_y, 1u,
                                  ISSIM_HIP_BLOCK_X, ISSIM_HIP_BLOCK_Y, 1u, 0, str, args, NULL);
    }
    return issim_hip_rc(hip_rc);
}

/*
 * Pass 2 — vertical 11-tap + SSIM combine + per-block partial sum,
 * followed by DtoH readback and finished-event record.
 * Grid sized over w_final x h_final. Block 16x8.
 *
 * Bug 2 fix: args are the five float intermediate buffers + rb.device
 * (float partials), then dimension/constant params — matching the kernel
 * signature in integer_ssim_score.hip exactly.
 */
static int issim_hip_launch_vert_readback(IssimStateHip *s, hipStream_t str)
{
    const unsigned grid_x = (s->w_final + ISSIM_HIP_BLOCK_X - 1u) / ISSIM_HIP_BLOCK_X;
    const unsigned grid_y = (s->h_final + ISSIM_HIP_BLOCK_Y - 1u) / ISSIM_HIP_BLOCK_Y;

    void *args2[] = {
        &s->d_ref_mu, &s->d_cmp_mu, &s->d_ref_sq, &s->d_cmp_sq, &s->d_refcmp, &s->rb.device,
        &s->w_horiz,  &s->w_final,  &s->h_final,  &s->c1,       &s->c2,
    };
    hipError_t hip_rc = hipModuleLaunchKernel(s->func_vert, grid_x, grid_y, 1u, ISSIM_HIP_BLOCK_X,
                                              ISSIM_HIP_BLOCK_Y, 1u, 0, str, args2, NULL);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    hip_rc = hipEventRecord((hipEvent_t)s->lc.submit, str);
    if (hip_rc != hipSuccess)
        return issim_hip_rc(hip_rc);

    const size_t copy_bytes = (size_t)s->partials_count * sizeof(float);
    hip_rc =
        hipMemcpyAsync(s->rb.host_pinned, s->rb.device, copy_bytes, hipMemcpyDeviceToHost, str);
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

    if (w < ISSIM_HIP_K || h < ISSIM_HIP_K) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "integer_ssim_hip: input %ux%u smaller than 11x11 Gaussian footprint.\n", w, h);
        return -EINVAL;
    }

    /* Bug 3 + 5 fix: populate w_horiz/h_horiz/w_final/h_final before
     * allocating readback buffer so partials_capacity is computed correctly
     * from w_final*h_final grid, not the full w*h grid. */
    issim_hip_init_dims(s, w, h, bpc);

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err != 0)
        return err;

    err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0)
        goto fail_after_ctx;

    /* Bug 4 + 5 fix: single float-partial readback slot sized for
     * partials_capacity blocks (from w_final/h_final grid). No rb_wgt. */
    err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx,
                                         (size_t)s->partials_capacity * sizeof(float));
    if (err != 0)
        goto fail_after_lc;

#ifdef HAVE_HIPCC
    err = issim_hip_module_load(s);
    if (err != 0)
        goto fail_after_rb;
    err = issim_hip_bufs_alloc(s);
    if (err != 0)
        goto fail_after_module;
#else
    err = -ENOSYS;
    if (err != 0)
        goto fail_after_rb;
#endif

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        err = -ENOMEM;
#ifdef HAVE_HIPCC
        issim_hip_bufs_free(s);
        goto fail_after_module;
#else
        goto fail_after_rb;
#endif
    }
    return 0;

#ifdef HAVE_HIPCC
fail_after_module:
    (void)hipModuleUnload(s->module);
    s->module = NULL;
#endif
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
    IssimStateHip *s = fex->priv;
    s->index = index;

    const unsigned grid_x = (s->w_final + ISSIM_HIP_BLOCK_X - 1u) / ISSIM_HIP_BLOCK_X;
    const unsigned grid_y = (s->h_final + ISSIM_HIP_BLOCK_Y - 1u) / ISSIM_HIP_BLOCK_Y;
    s->partials_count = grid_x * grid_y;

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

    /* Bug 4 fix: accumulate float partials in double, divide by effective
     * pixel count (W-10)*(H-10). No rb_wgt buffer or total_wgt needed —
     * mirrors the CUDA twin's collect_fex_cuda exactly. */
    const float *partials = (const float *)s->rb.host_pinned;
    double total = 0.0;
    for (unsigned i = 0; i < s->partials_count; i++)
        total += (double)partials[i];
    const double n_pixels = (double)s->w_final * (double)s->h_final;
    const double score = total / n_pixels;

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict, "ssim",
                                                   score, index);
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"ssim", NULL};

/* Real integer_ssim HIP extractor (ADR-0564). Load-bearing: declared
 * via extern in feature_extractor.c. */
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
