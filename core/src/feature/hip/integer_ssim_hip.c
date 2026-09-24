/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  integer_ssim feature extractor on the HIP backend (ADR-0564).
 *
 *  Provides the `"ssim"` feature of the CPU reference `integer_ssim.c`
 *  (`vmaf_fex_ssim`) and mirrors its CUDA twin `cuda/ssim_cuda.c`
 *  (`vmaf_fex_integer_ssim_cuda`) call for call. The kernels live in
 *  `integer_ssim/integer_ssim_score.hip`:
 *
 *    Pass 1 (integer_ssim_horiz_{8,16}bpc): 9-tap integer Gaussian over each
 *      row, int64 moments into six W x H int64 planes.
 *    Pass 2 (integer_ssim_vert_combine): 9-tap over the columns of those
 *      planes, the per-pixel SSIM term in double, and one (term sum, int64
 *      weight sum) pair per 16x8 block.
 *  collect() adds the block pairs and returns sum(term) / sum(weight), the
 *  `ssim / ssimw` that calc_ssim() returns on the CPU.
 *
 *  HIP adaptations from the CUDA twin:
 *  - Pictures arrive as host VmafPictures; the two luma planes are staged
 *    into packed device buffers with hipMemcpy2DAsync on the private stream
 *    before pass 1 (T7-10b posture, no HIP picture pool yet).
 *  - hipModuleLoadData / hipModuleGetFunction / hipModuleLaunchKernel stand
 *    in for the cuModule* / cuLaunchKernel calls; the kernels are extern "C"
 *    so the lookups by name resolve.
 *  - Device buffers are raw hipMalloc pointers instead of VmafCudaBuffer.
 *
 *  Without enable_hipcc there is no kernel blob to load and init() returns
 *  -ENOSYS, the scaffold contract every HIP extractor shares.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <hip/hip_runtime_api.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "libvmaf/picture.h"
#include "log.h"

#include "../../hip/common.h"
#include "../../hip/hip_handle.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "integer_ssim_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Launch geometry; must match ISSIM_BLOCK_X / ISSIM_BLOCK_Y in
 * integer_ssim_score.hip, whose block reduction is sized for it. */
#define ISSIM_HIP_BLOCK_X 16u
#define ISSIM_HIP_BLOCK_Y 8u

/* Pass-1 output planes, in kernel argument order: mux, muy, x2, xy, y2, w. */
#define ISSIM_HIP_MOMENTS 6u

static int issim_hip_rc(hipError_t rc)
{
    switch (rc) {
    case hipSuccess:
        return 0;
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

typedef struct IssimStateHip {
    VmafHipKernelLifecycle lc;
    /* One double term sum and one int64 weight sum per block. */
    VmafHipKernelReadback rb_ssim;
    VmafHipKernelReadback rb_wgt;
    VmafHipContext *ctx;

    /* Pass-1 planes, width * height int64_t each. */
    void *d_moment[ISSIM_HIP_MOMENTS];

    /* Packed luma staging, width * bytes-per-sample per row. */
    void *ref_in;
    void *cmp_in;

    hipModule_t module;
    hipFunction_t func_horiz_8;
    hipFunction_t func_horiz_16;
    hipFunction_t func_vert;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned grid_x;
    unsigned grid_y;
    unsigned block_count;
    /* (1 << bpc) - 1, the CPU's `samplemax`, as the kernel's double. */
    double samplemax;

    VmafDictionary *feature_name_dict;
} IssimStateHip;

static const VmafOption options[] = {
    {0},
};

static size_t issim_hip_bytes_per_sample(unsigned bpc)
{
    return (bpc <= 8u) ? 1u : 2u;
}

static void issim_hip_init_dims(IssimStateHip *s, unsigned w, unsigned h, unsigned bpc)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->grid_x = (w + ISSIM_HIP_BLOCK_X - 1u) / ISSIM_HIP_BLOCK_X;
    s->grid_y = (h + ISSIM_HIP_BLOCK_Y - 1u) / ISSIM_HIP_BLOCK_Y;
    s->block_count = s->grid_x * s->grid_y;
    s->samplemax = (double)((1u << bpc) - 1u);
}

/* Load the kernel blob and resolve the three kernels by their extern "C"
 * names. */
static int issim_hip_module_load(IssimStateHip *s, const char *fex_name)
{
#ifdef HAVE_HIPCC
    (void)fex_name;
    hipError_t rc = hipModuleLoadData(&s->module, integer_ssim_score_hsaco);
    if (rc != hipSuccess)
        return issim_hip_rc(rc);
    rc = hipModuleGetFunction(&s->func_horiz_8, s->module, "integer_ssim_horiz_8bpc");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->func_horiz_16, s->module, "integer_ssim_horiz_16bpc");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->func_vert, s->module, "integer_ssim_vert_combine");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return issim_hip_rc(rc);
#else
    (void)s;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "feature '%s' requires HIP device kernels compiled with -Denable_hipcc=true\n",
             fex_name);
    return -ENOSYS;
#endif
}

/* Release every device buffer. Safe on a partially allocated state. */
static int issim_hip_bufs_free(IssimStateHip *s)
{
    void **bufs[ISSIM_HIP_MOMENTS + 2u];
    for (unsigned i = 0u; i < ISSIM_HIP_MOMENTS; i++)
        bufs[i] = &s->d_moment[i];
    bufs[ISSIM_HIP_MOMENTS] = &s->ref_in;
    bufs[ISSIM_HIP_MOMENTS + 1u] = &s->cmp_in;

    int err = 0;
    for (unsigned i = 0u; i < ISSIM_HIP_MOMENTS + 2u; i++) {
        if (*bufs[i] == NULL)
            continue;
        const int e = issim_hip_rc(hipFree(*bufs[i]));
        *bufs[i] = NULL;
        if (err == 0)
            err = e;
    }
    return err;
}

static int issim_hip_bufs_alloc(IssimStateHip *s)
{
    const size_t plane_bytes = (size_t)s->width * s->height * sizeof(int64_t);
    const size_t stage_bytes = (size_t)s->width * s->height * issim_hip_bytes_per_sample(s->bpc);

    hipError_t rc = hipSuccess;
    for (unsigned i = 0u; i < ISSIM_HIP_MOMENTS && rc == hipSuccess; i++)
        rc = hipMalloc(&s->d_moment[i], plane_bytes);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->ref_in, stage_bytes);
    if (rc == hipSuccess)
        rc = hipMalloc(&s->cmp_in, stage_bytes);
    if (rc != hipSuccess) {
        (void)issim_hip_bufs_free(s);
        return issim_hip_rc(rc);
    }
    return 0;
}

/* Tear down everything init() may have set up, in reverse order. Every step
 * tolerates a handle that was never created, so this serves both a failed
 * init() and close(). Returns the first error. */
static int issim_hip_release(IssimStateHip *s)
{
    /* Drains the private stream first, so no kernel still uses a buffer. */
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    int e = issim_hip_bufs_free(s);
    if (rc == 0)
        rc = e;
    if (s->module != NULL) {
        e = issim_hip_rc(hipModuleUnload(s->module));
        s->module = NULL;
        if (rc == 0)
            rc = e;
    }
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
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return rc;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    IssimStateHip *s = fex->priv;

    /* Any size works: near the border the window is truncated, as on the
     * CPU. Only an empty frame has no pixel to average over. */
    if (w == 0u || h == 0u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_hip: empty input %ux%u\n", w, h);
        return -EINVAL;
    }
    issim_hip_init_dims(s, w, h, bpc);

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err == 0)
        err = issim_hip_module_load(s, fex->name);
    if (err == 0) {
        err = vmaf_hip_kernel_readback_alloc(&s->rb_ssim, s->ctx,
                                             (size_t)s->block_count * sizeof(double));
    }
    if (err == 0) {
        err = vmaf_hip_kernel_readback_alloc(&s->rb_wgt, s->ctx,
                                             (size_t)s->block_count * sizeof(int64_t));
    }
    if (err == 0)
        err = issim_hip_bufs_alloc(s);
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)issim_hip_release(s);
    return err;
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return issim_hip_release(fex->priv);
}

/* Pass 1. The staged planes are packed, so one row is `width` samples. */
static int issim_hip_launch_horiz(IssimStateHip *s, hipStream_t str)
{
    ptrdiff_t stride = (ptrdiff_t)s->width * (ptrdiff_t)issim_hip_bytes_per_sample(s->bpc);
    void *args[] = {
        (void *)&s->ref_in,      (void *)&stride,         (void *)&s->cmp_in,
        (void *)&stride,         (void *)&s->d_moment[0], (void *)&s->d_moment[1],
        (void *)&s->d_moment[2], (void *)&s->d_moment[3], (void *)&s->d_moment[4],
        (void *)&s->d_moment[5], (void *)&s->width,       (void *)&s->height,
    };
    hipFunction_t fn = (s->bpc <= 8u) ? s->func_horiz_8 : s->func_horiz_16;
    return issim_hip_rc(hipModuleLaunchKernel(fn, s->grid_x, s->grid_y, 1u, ISSIM_HIP_BLOCK_X,
                                              ISSIM_HIP_BLOCK_Y, 1u, 0u, str, args, NULL));
}

/* Pass 2. Implicitly ordered after pass 1: both run on `str`. */
static int issim_hip_launch_vert(IssimStateHip *s, hipStream_t str)
{
    void *args[] = {
        (void *)&s->d_moment[0],    (void *)&s->d_moment[1],   (void *)&s->d_moment[2],
        (void *)&s->d_moment[3],    (void *)&s->d_moment[4],   (void *)&s->d_moment[5],
        (void *)&s->rb_ssim.device, (void *)&s->rb_wgt.device, (void *)&s->width,
        (void *)&s->height,         (void *)&s->samplemax,
    };
    return issim_hip_rc(hipModuleLaunchKernel(s->func_vert, s->grid_x, s->grid_y, 1u,
                                              ISSIM_HIP_BLOCK_X, ISSIM_HIP_BLOCK_Y, 1u, 0u, str,
                                              args, NULL));
}

/* Copy both per-block partial arrays back and record the `finished` event
 * that collect() waits on. */
static int issim_hip_readback(IssimStateHip *s, hipStream_t str)
{
    hipError_t rc = hipEventRecord(vmaf_hip_event_of(s->lc.submit), str);
    if (rc == hipSuccess) {
        rc = hipMemcpyAsync(s->rb_ssim.host_pinned, s->rb_ssim.device,
                            (size_t)s->block_count * sizeof(double), hipMemcpyDeviceToHost, str);
    }
    if (rc == hipSuccess) {
        rc = hipMemcpyAsync(s->rb_wgt.host_pinned, s->rb_wgt.device,
                            (size_t)s->block_count * sizeof(int64_t), hipMemcpyDeviceToHost, str);
    }
    if (rc != hipSuccess)
        return issim_hip_rc(rc);
    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

/* Stage both host luma planes into the packed device buffers.
 *
 * vmaf_hip_picture_upload() returns only once the copies have read the
 * pictures, and that is load-bearing: the caller may recycle them as soon as
 * submit() returns, and the CLI's picture pool refills a slot with the next
 * frame right away. Without the wait some frames were scored against a mix
 * of their own and the next frame's samples (off by up to 0.2 on the Netflix
 * 576x324 pair, a different set of frames on every run;
 * T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
static int issim_hip_upload(const IssimStateHip *s, const VmafPicture *ref_pic,
                            const VmafPicture *dist_pic)
{
    const size_t row_bytes = (size_t)s->width * issim_hip_bytes_per_sample(s->bpc);
    const VmafHipPlaneUpload planes[] = {
        {.dst = s->ref_in,
         .dst_pitch = row_bytes,
         .pic = ref_pic,
         .plane = 0u,
         .row_bytes = row_bytes,
         .rows = s->height},
        {.dst = s->cmp_in,
         .dst_pitch = row_bytes,
         .pic = dist_pic,
         .plane = 0u,
         .row_bytes = row_bytes,
         .rows = s->height},
    };
    return vmaf_hip_picture_upload(planes, 2u, s->lc.str);
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    IssimStateHip *s = fex->priv;
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);

    int err = issim_hip_upload(s, ref_pic, dist_pic);
    if (err == 0)
        err = issim_hip_launch_horiz(s, str);
    if (err == 0)
        err = issim_hip_launch_vert(s, str);
    if (err == 0)
        err = issim_hip_readback(s, str);
    return err;
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    IssimStateHip *s = fex->priv;

    const int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

    const double *term_partials = s->rb_ssim.host_pinned;
    const int64_t *weight_partials = s->rb_wgt.host_pinned;
    double total_term = 0.0;
    int64_t total_weight = 0;
    for (unsigned i = 0u; i < s->block_count; i++) {
        total_term += term_partials[i];
        total_weight += weight_partials[i];
    }
    return vmaf_ssim_emit_ratio_score_named(feature_collector, s->feature_name_dict,
                                            "integer_ssim_hip", "ssim", total_term,
                                            (double)total_weight, 0, 0.0, index);
}

static const char *provided_features[] = {"ssim", NULL};

/* integer_ssim on HIP (ADR-0564): the 9-tap int64 algorithm of the CPU
 * `ssim` extractor, flagged for model-driven dispatch under --backend hip.
 * Declared via extern in feature_extractor.cpp. */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_ssim_hip = {
    .name = "integer_ssim_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(IssimStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 2,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
