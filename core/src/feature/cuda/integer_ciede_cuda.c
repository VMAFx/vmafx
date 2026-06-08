/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  ciede2000 feature extractor on the CUDA backend (T7-23 /
 *  ADR-0182, GPU long-tail batch 1c part 2). CUDA twin of
 *  ciede_vulkan (PR #136 / ADR-0187); shares the float-precision
 *  contract — places=4 empirical floor on real hardware.
 *
 *  Single dispatch per frame; warp-shuffle reduces 32 thread
 *  contributions to one float, lane 0 atomicAdd's to a single
 *  device counter. Host divides the counter by w*h and applies
 *  the CPU's logarithmic transform `45 - 20*log10(mean_dE)` for
 *  the final `ciede2000` metric. Mirrors psnr_cuda's submit /
 *  collect scaffolding.
 */

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
#include "cuda/integer_ciede_cuda.h"
#include "cuda/kernel_template.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "cuda_helper.cuh"

typedef struct CiedeStateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). */
    VmafCudaKernelLifecycle lc;
    /* Per-block float partials: device + pinned host. Owned by the
     * template's readback bundle. */
    VmafCudaKernelReadback rb;

    CUfunction funcbpc8;
    CUfunction funcbpc16;
    /* PTX module backing the CIEDE kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
    unsigned partials_capacity;
    unsigned partials_count;
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    unsigned ss_hor;
    unsigned ss_ver;
    VmafDictionary *feature_name_dict;
} CiedeStateCuda;

static const VmafOption options[] = {{0}};

static int ciede_cuda_dispatch(const VmafPicture *ref, const VmafPicture *dis,
                               VmafCudaBuffer *partials, unsigned width, unsigned height,
                               unsigned bpc, unsigned ss_hor, unsigned ss_ver, CUfunction funcbpc8,
                               CUfunction funcbpc16, CudaFunctions *cu_f, CUstream stream)
{
    const int block_dim_x = 16;
    const int block_dim_y = 16;
    const int grid_dim_x = DIV_ROUND_UP(width, block_dim_x);
    const int grid_dim_y = DIV_ROUND_UP(height, block_dim_y);

    void *kernelParams[] = {(void *)ref, (void *)dis, (void *)partials, &width,
                            &height,     &bpc,        &ss_hor,          &ss_ver};
    CUfunction func = (bpc == 8) ? funcbpc8 : funcbpc16;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(func, grid_dim_x, grid_dim_y, 1, block_dim_x,
                                           block_dim_y, 1, 0, stream, kernelParams, NULL));
    return 0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    if (pix_fmt == VMAF_PIX_FMT_YUV400P)
        return -EINVAL;
    CiedeStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    /* Stream + event pair via the template — handles ctx push/pop +
     * rollback on failure. */
    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    /* Module + function lookup is metric-specific; the template
     * doesn't (yet) own that step. */
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, ciede_score_ptx), fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc8, s->module, "calculate_ciede_kernel_8bpc"), fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc16, s->module, "calculate_ciede_kernel_16bpc"), fail);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);

    s->bpc = bpc;
    s->ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1u : 0u;
    s->ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1u : 0u;

    /* Pre-size partials for the announced (w, h). Submit reuses if
     * the picture is the same size; reallocates if a larger picture
     * arrives (rare, but the API doesn't pin geometry). */
    const unsigned grid_x = (w + 15u) / 16u;
    const unsigned grid_y = (h + 15u) / 16u;
    s->partials_capacity = grid_x * grid_y;

    err = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state,
                                          (size_t)s->partials_capacity * sizeof(float));
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
    CiedeStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    const unsigned grid_x = (s->frame_w + 15u) / 16u;
    const unsigned grid_y = (s->frame_h + 15u) / 16u;
    s->partials_count = grid_x * grid_y;

    /* Intentionally inline the pre-launch wait rather than calling
     * vmaf_cuda_kernel_submit_pre_launch — ciede's kernel writes one
     * float per block (no atomic), so the template's memset is
     * unnecessary. The lifecycle / readback / collect helpers still
     * apply. */
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(vmaf_cuda_picture_get_stream(ref_pic),
                                              vmaf_cuda_picture_get_ready_event(dist_pic),
                                              CU_EVENT_WAIT_DEFAULT));

    int err = ciede_cuda_dispatch(ref_pic, dist_pic, s->rb.device, ref_pic->w[0], ref_pic->h[0],
                                  s->bpc, s->ss_hor, s->ss_ver, s->funcbpc8, s->funcbpc16, cu_f,
                                  vmaf_cuda_picture_get_stream(ref_pic));
    if (err)
        return err;

    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, vmaf_cuda_picture_get_stream(ref_pic)));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));

    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->rb.host_pinned, (CUdeviceptr)s->rb.device->data,
                                        (size_t)s->partials_count * sizeof(float), s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    CiedeStateCuda *s = fex->priv;

    int err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (err)
        return err;

    /* Per-block partials → host accumulation in double. Same
     * precision argument as ciede_vulkan (ADR-0187): per-block
     * sums fit in float7 precision (max ~5000 magnitude); the
     * cross-block reduction across thousands of partials needs
     * double to retain places=4. */
    const float *partials_host = s->rb.host_pinned;
    double total = 0.0;
    for (unsigned i = 0; i < s->partials_count; i++)
        total += (double)partials_host[i];
    const double n_pixels = (double)s->frame_w * (double)s->frame_h;
    const double mean_de = total / n_pixels;
    const double score = 45.0 - 20.0 * log10(mean_de);

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "ciede2000", score, index);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    CiedeStateCuda *s = fex->priv;
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

static const char *provided_features[] = {"ciede2000", NULL};

VmafFeatureExtractor vmaf_fex_ciede_cuda = {
    .name = "ciede_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(CiedeStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
