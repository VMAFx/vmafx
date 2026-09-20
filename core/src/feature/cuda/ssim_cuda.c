/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  Real integer_ssim feature extractor on the CUDA backend (ADR-0564).
 *
 *  This extractor provides the `"ssim"` feature (same name as the CPU
 *  `vmaf_fex_ssim` in `libvmaf/src/feature/integer_ssim.c`) using a
 *  fixed-point integer algorithm that is bit-exact with the CPU.
 *
 *  The CPU algorithm uses:
 *    - A 9-tap Gaussian kernel with INTEGER weights [2,9,28,55,68,55,28,9,2]
 *      (sigma=1.5, KERNEL_WEIGHT=256, sum=256).
 *    - int64_t accumulators for all moments (mux, muy, x2, xy, y2, w).
 *    - Boundary-truncation: pixels near the border use a reduced kernel
 *      (out-of-image taps are skipped, w reflects actual weight sum).
 *    - Final SSIM formula in double from int64 moments.
 *
 *  This file supersedes the misleadingly-named `integer_ssim_cuda.c`,
 *  which implements float_ssim (not integer_ssim) on CUDA. That file
 *  provides `"float_ssim"` and remains compiled as the float_ssim_cuda
 *  extractor. This new file provides `"ssim"` and is registered as
 *  `vmaf_fex_integer_ssim_cuda`.
 *
 *  Two-pass design (mirroring the CPU ring-buffer algorithm):
 *    Pass 1 (horiz): per-pixel 9-tap horizontal int64 moment accumulation.
 *      Writes 6 × (W×H) int64_t intermediate arrays.
 *    Pass 2 (vert+combine): per-pixel 9-tap vertical int64 accumulation
 *      from horizontal arrays, then SSIM formula in double, then per-block
 *      double partial sum + int64 weight sum.
 *  Host: ssim = sum(partials) / sum(partial_weights).
 *
 *  Bit-exactness argument:
 *    - The integer Gaussian kernel is fixed (same 9 integer constants).
 *    - CUDA int64 arithmetic is deterministic on NVIDIA hardware.
 *    - The boundary-truncation logic is identical: same k_min/k_max
 *      formulas as the CPU.
 *    - The final double arithmetic is the same formula.
 *    Target: places=6 vs CPU on the Netflix golden fixture (576×324 8bpc).
 */

#include "vmaf_nullptr.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "common/alignment.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "cuda/ssim_cuda.h"
#include "cuda/kernel_template.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "cuda_helper.cuh"

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define ISSIM_CUDA_BLOCK_X 16
#define ISSIM_CUDA_BLOCK_Y 8
#define ISSIM_CUDA_BLOCK_SZ (ISSIM_CUDA_BLOCK_X * ISSIM_CUDA_BLOCK_Y)

typedef struct IssimStateCuda {
    VmafCudaKernelLifecycle lc;
    /* Two readback slots: double partials + int64 partial weights. */
    VmafCudaKernelReadback rb_ssim;
    VmafCudaKernelReadback rb_wgt;

    CUfunction func_horiz_8;
    CUfunction func_horiz_16;
    CUfunction func_vert;

    /* 6 × (width×height) int64_t intermediate buffers for the horizontal
     * moment arrays (mux, muy, x2, xy, y2, w). */
    VmafCudaBuffer *d_mux;
    VmafCudaBuffer *d_muy;
    VmafCudaBuffer *d_x2;
    VmafCudaBuffer *d_xy;
    VmafCudaBuffer *d_y2;
    VmafCudaBuffer *d_w;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned grid_x;
    unsigned grid_y;
    unsigned block_count;

    unsigned index;
    /* PTX module backing the SSIM kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
    VmafDictionary *feature_name_dict;
} IssimStateCuda;

static const VmafOption options[] = {
    {0},
};

static void free_issim_buffer(VmafFeatureExtractor *fex, VmafCudaBuffer **buffer, int *status)
{
    if (!*buffer)
        return;
    const int err = vmaf_cuda_buffer_free(fex->cu_state, *buffer);
    free(*buffer);
    *buffer = VMAF_NULLPTR;
    if (*status == 0)
        *status = err;
}

static void cleanup_issim_init(VmafFeatureExtractor *fex)
{
    IssimStateCuda *s = fex->priv;
    VmafCudaBuffer **buffers[] = {&s->d_mux, &s->d_muy, &s->d_x2, &s->d_xy, &s->d_y2, &s->d_w};
    int ignored = 0;
    for (unsigned i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
        free_issim_buffer(fex, buffers[i], &ignored);
    (void)vmaf_cuda_kernel_readback_free(&s->rb_wgt, fex->cu_state);
    (void)vmaf_cuda_kernel_readback_free(&s->rb_ssim, fex->cu_state);
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
}

static int init_issim_buffers(VmafFeatureExtractor *fex, size_t plane_bytes,
                              size_t ssim_partials_bytes, size_t weight_partials_bytes)
{
    IssimStateCuda *s = fex->priv;
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_mux, plane_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_muy, plane_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_x2, plane_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_xy, plane_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_y2, plane_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_w, plane_bytes);
    if (!ret)
        ret = vmaf_cuda_kernel_readback_alloc(&s->rb_ssim, fex->cu_state, ssim_partials_bytes);
    if (!ret)
        ret = vmaf_cuda_kernel_readback_alloc(&s->rb_wgt, fex->cu_state, weight_partials_bytes);
    if (!ret) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            ret = -ENOMEM;
    }
    if (ret)
        cleanup_issim_init(fex);
    return ret;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    IssimStateCuda *s = fex->priv;

    if (w < 1u || h < 1u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_cuda: zero-dimension input %ux%u\n", w, h);
        return -EINVAL;
    }

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    CudaFunctions *cu_f = fex->cu_state->f;
    int _cuda_err;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail_lc);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, integer_ssim_score_ptx), fail_ctx);
    CHECK_CUDA_GOTO(cu_f,
                    cuModuleGetFunction(&s->func_horiz_8, s->module, "integer_ssim_horiz_8bpc"),
                    fail_ctx);
    CHECK_CUDA_GOTO(cu_f,
                    cuModuleGetFunction(&s->func_horiz_16, s->module, "integer_ssim_horiz_16bpc"),
                    fail_ctx);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->func_vert, s->module, "integer_ssim_vert_combine"), fail_ctx);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(VMAF_NULLPTR), fail_ctx);

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->grid_x = (w + ISSIM_CUDA_BLOCK_X - 1u) / ISSIM_CUDA_BLOCK_X;
    s->grid_y = (h + ISSIM_CUDA_BLOCK_Y - 1u) / ISSIM_CUDA_BLOCK_Y;
    s->block_count = s->grid_x * s->grid_y;

    const size_t int64_plane_bytes = (size_t)w * h * sizeof(int64_t);
    const size_t double_partials_bytes = (size_t)s->block_count * sizeof(double);
    const size_t int64_partials_bytes = (size_t)s->block_count * sizeof(int64_t);

    return init_issim_buffers(fex, int64_plane_bytes, double_partials_bytes, int64_partials_bytes);

fail_ctx:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
fail_lc:
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return _cuda_err;
}

static int launch_issim_horizontal(IssimStateCuda *s, const VmafPicture *ref_pic,
                                   const VmafPicture *dist_pic, CUstream stream,
                                   CudaFunctions *cu_f)
{
    ptrdiff_t ref_stride = ref_pic->stride[0];
    ptrdiff_t cmp_stride = dist_pic->stride[0];
    uint8_t *ref_data = ref_pic->data[0];
    uint8_t *dist_data = dist_pic->data[0];
    void *params[] = {
        (void *)&ref_data, &ref_stride,     (void *)&dist_data, &cmp_stride,
        &s->d_mux->data,   &s->d_muy->data, &s->d_x2->data,     &s->d_xy->data,
        &s->d_y2->data,    &s->d_w->data,   &s->width,          &s->height,
    };
    CUfunction function = s->bpc == 8u ? s->func_horiz_8 : s->func_horiz_16;
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(function, s->grid_x, s->grid_y, 1u, ISSIM_CUDA_BLOCK_X,
                                     ISSIM_CUDA_BLOCK_Y, 1u, 0, stream, params, VMAF_NULLPTR));
    return 0;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                           const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                           const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    IssimStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;

    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(vmaf_cuda_picture_get_stream(ref_pic),
                                              vmaf_cuda_picture_get_ready_event(dist_pic),
                                              CU_EVENT_WAIT_DEFAULT));

    CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    int err = launch_issim_horizontal(s, ref_pic, dist_pic, stream, cu_f);
    if (err)
        return err;

    /* Pass 2 — vertical accumulation + SSIM formula + block reduction. */
    int64_t samplemax = (int64_t)((1u << s->bpc) - 1u);
    void *params2[] = {
        &s->d_mux->data,
        &s->d_muy->data,
        &s->d_x2->data,
        &s->d_xy->data,
        &s->d_y2->data,
        &s->d_w->data,
        &s->rb_ssim.device->data,
        &s->rb_wgt.device->data,
        &s->width,
        &s->height,
        &samplemax,
    };
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_vert, s->grid_x, s->grid_y, 1u, ISSIM_CUDA_BLOCK_X,
                                     ISSIM_CUDA_BLOCK_Y, 1u, 0, stream, params2, VMAF_NULLPTR));

    /* Async DtoH for both readback buffers. */
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rb_ssim.host_pinned,
                                              (CUdeviceptr)s->rb_ssim.device->data,
                                              (size_t)s->block_count * sizeof(double), s->lc.str));
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->rb_wgt.host_pinned, (CUdeviceptr)s->rb_wgt.device->data,
                                        (size_t)s->block_count * sizeof(int64_t), s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    IssimStateCuda *s = fex->priv;

    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;

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
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    IssimStateCuda *s = fex->priv;

    int rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);

    VmafCudaBuffer **buffers[] = {&s->d_mux, &s->d_muy, &s->d_x2, &s->d_xy, &s->d_y2, &s->d_w};
    for (unsigned i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
        free_issim_buffer(fex, buffers[i], &rc);

    int e2 = vmaf_cuda_kernel_readback_free(&s->rb_ssim, fex->cu_state);
    if (rc == 0)
        rc = e2;
    e2 = vmaf_cuda_kernel_readback_free(&s->rb_wgt, fex->cu_state);
    if (rc == 0)
        rc = e2;
    if (s->feature_name_dict) {
        e2 = vmaf_dictionary_free(&s->feature_name_dict);
        if (rc == 0)
            rc = e2;
    }
    const CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f && s->module)
        (void)cu_f->cuModuleUnload(s->module);
    return rc;
}

static const char *provided_features[] = {"ssim", VMAF_NULLPTR};

/* Real integer_ssim GPU extractor (ADR-0564). Bit-exact with the CPU
 * `vmaf_fex_ssim` using fixed-point int64 accumulation and boundary-
 * truncation matching the CPU's ring-buffer algorithm.
 * Named `integer_ssim_cuda` to distinguish it from the CPU `ssim`
 * and from `float_ssim_cuda` which uses floating-point Gaussian weights. */
VmafFeatureExtractor vmaf_fex_integer_ssim_cuda = {
    .name = "integer_ssim_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(IssimStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    .chars =
        {
            .n_dispatches_per_frame = 2,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
