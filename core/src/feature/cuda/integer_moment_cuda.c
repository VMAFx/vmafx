/* Upstream-mirror filename: defines float_moment symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_moment feature extractor on the CUDA backend
 *  (T7-23 / ADR-0182, GPU long-tail batch 1d part 2). CUDA twin
 *  of moment_vulkan (PR #133); shares the float_moment_cuda twin
 *  PR with moment_sycl (part 3).
 *
 *  Single dispatch per frame; emits all four metrics
 *  (float_moment_ref{1st,2nd}, float_moment_dis{1st,2nd}) in
 *  one kernel pass via four uint64 atomic counters. Mirrors the
 *  psnr_cuda host scaffolding shipped in PR #129.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "common/alignment.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "cuda/integer_moment_cuda.h"
#include "cuda/kernel_template.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "cuda_helper.cuh"

typedef struct MomentStateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). */
    VmafCudaKernelLifecycle lc;
    /* Four uint64 atomic counters [ref1, dis1, ref2, dis2]:
     * device + pinned host. Owned by the template's readback bundle. */
    VmafCudaKernelReadback rb;

    CUfunction funcbpc8;
    CUfunction funcbpc16;
    /* PTX module backing the moment kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    VmafDictionary *feature_name_dict;
} MomentStateCuda;

static const VmafOption options[] = {{0}};

static int moment_cuda_dispatch(const VmafPicture *ref, const VmafPicture *dis,
                                VmafCudaBuffer *sums, unsigned width, unsigned height, unsigned bpc,
                                CUfunction funcbpc8, CUfunction funcbpc16, CudaFunctions *cu_f,
                                CUstream stream)
{
    const int block_dim_x = 16;
    const int block_dim_y = 16;
    const int grid_dim_x = DIV_ROUND_UP(width, block_dim_x);
    const int grid_dim_y = DIV_ROUND_UP(height, block_dim_y);

    void *kernelParams[] = {(void *)ref, (void *)dis, (void *)sums, &width, &height};
    CUfunction func = (bpc == 8) ? funcbpc8 : funcbpc16;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(func, grid_dim_x, grid_dim_y, 1, block_dim_x,
                                           block_dim_y, 1, 0, stream, kernelParams, NULL));
    return 0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)w;
    (void)h;
    MomentStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, moment_score_ptx), fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc8, s->module, "calculate_moment_kernel_8bpc"), fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc16, s->module, "calculate_moment_kernel_16bpc"), fail);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);

    s->bpc = bpc;

    err = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state, 4u * sizeof(uint64_t));
    if (err)
        goto free_ref;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        err = -ENOMEM;
        goto free_ref;
    }

    return 0;

free_ref:
    (void)vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    (void)vmaf_dictionary_free(&s->feature_name_dict);
    return err;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return _cuda_err;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    MomentStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];

    /* Pre-launch: zero the four device counters and wait for the
     * dist-side ready event. The kernel uses atomic adds, so the
     * memset is mandatory. */
    int err = vmaf_cuda_kernel_submit_pre_launch(&s->lc, fex->cu_state, &s->rb,
                                                 vmaf_cuda_picture_get_stream(ref_pic),
                                                 vmaf_cuda_picture_get_ready_event(dist_pic));
    if (err)
        return err;

    err = moment_cuda_dispatch(ref_pic, dist_pic, s->rb.device, ref_pic->w[0], ref_pic->h[0],
                               s->bpc, s->funcbpc8, s->funcbpc16, cu_f,
                               vmaf_cuda_picture_get_stream(ref_pic));
    if (err)
        return err;

    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, vmaf_cuda_picture_get_stream(ref_pic)));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));

    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rb.host_pinned, (CUdeviceptr)s->rb.device->data,
                                              s->rb.bytes, s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    MomentStateCuda *s = fex->priv;

    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;

    const uint64_t *sums_host = s->rb.host_pinned;
    const double n_pixels = (double)s->frame_w * (double)s->frame_h;
    const double ref1 = (double)sums_host[0] / n_pixels;
    const double dis1 = (double)sums_host[1] / n_pixels;
    const double ref2 = (double)sums_host[2] / n_pixels;
    const double dis2 = (double)sums_host[3] / n_pixels;

    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_ref1st", ref1, index);
    if (!err)
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_dis1st", dis1, index);
    if (!err)
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_ref2nd", ref2, index);
    if (!err)
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_dis2nd", dis2, index);
    return err;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    MomentStateCuda *s = fex->priv;
    int rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    int rb_rc = vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    if (rc == 0)
        rc = rb_rc;
    int dict_rc = vmaf_dictionary_free(&s->feature_name_dict);
    if (rc == 0)
        rc = dict_rc;
    const CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f && s->module)
        (void)cu_f->cuModuleUnload(s->module);
    return rc;
}

static const char *provided_features[] = {
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
    NULL,
};

VmafFeatureExtractor vmaf_fex_float_moment_cuda = {
    .name = "float_moment_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(MomentStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    /* 1 dispatch/frame, reduction-dominated; AUTO + 1080p area
     * matches motion's profile (ADR-0181 / ADR-0182). */
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
