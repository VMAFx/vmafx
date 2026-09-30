/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  psnr_hvs feature extractor on the CUDA backend
 *  (T7-23 / ADR-0188 / ADR-0191 / ADR-1369).
 *
 *  CUDA port of ADR-1369: the kernel reads the raw 8- to 12-bit samples of
 *  the device pictures with their pitch (no host copy, host conversion or
 *  private upload), two threads per 8x8 block exchange their statistics with
 *  a warp shuffle, the integer DCT runs in shared memory, and one launch
 *  covers every active plane. The per-block float expressions are the
 *  previous CUDA kernel's, and reduce_hvs_planes() adds the partials in block
 *  order in float, so the output is bit-identical to the previous twin
 *  (except at 9 and 11 bits, which it scored wrongly). 4:0:0 input is luma
 *  only, as in the CPU extractor.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "cuda/integer_psnr_hvs_cuda.h"
#include "cuda/kernel_template.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct PsnrHvsStateCuda {
    VmafCudaKernelLifecycle lc;
    VmafCudaKernelReadback rb;
    CUfunction func_psnr_hvs;
    CUmodule module;

    unsigned width[PSNR_HVS_NUM_PLANES];
    unsigned height[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_x[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_y[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks[PSNR_HVS_NUM_PLANES];
    unsigned first_block[PSNR_HVS_NUM_PLANES];
    unsigned total_blocks;

    unsigned bpc;
    int32_t samplemax_sq;

    bool enable_chroma;
    unsigned n_planes;

    unsigned index;
    VmafDictionary *feature_name_dict;
} PsnrHvsStateCuda;

static const VmafOption options[] = {
    {
        .name = "enable_chroma",
        .help = "enable calculation for chroma channels",
        .offset = offsetof(PsnrHvsStateCuda, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = true,
    },
    {0},
};

static int validate_hvs_input(unsigned bpc, unsigned w, unsigned h)
{
    if (bpc > 12u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_cuda: invalid bitdepth (%u); bpc must be <= 12\n",
                 bpc);
        return -EINVAL;
    }
    if (w < (unsigned)PSNR_HVS_BLOCK || h < (unsigned)PSNR_HVS_BLOCK) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_cuda: input %ux%u smaller than 8x8 block\n", w, h);
        return -EINVAL;
    }
    return 0;
}

static int configure_hvs_geometry(PsnrHvsStateCuda *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                  unsigned h)
{
    s->width[0] = w;
    s->height[0] = h;
    /* 4:0:0 has no chroma planes: luma only whatever enable_chroma says, as in
     * the CPU extractor (third_party/xiph/psnr_hvs.c::init). */
    s->n_planes =
        (s->enable_chroma && pix_fmt != VMAF_PIX_FMT_YUV400P) ? (unsigned)PSNR_HVS_NUM_PLANES : 1U;
    switch (pix_fmt) {
    case VMAF_PIX_FMT_YUV400P:
        s->width[1] = s->width[2] = 0U;
        s->height[1] = s->height[2] = 0U;
        break;
    case VMAF_PIX_FMT_YUV420P:
        s->width[1] = s->width[2] = (w + 1u) >> 1;
        s->height[1] = s->height[2] = (h + 1u) >> 1;
        break;
    case VMAF_PIX_FMT_YUV422P:
        s->width[1] = s->width[2] = (w + 1u) >> 1;
        s->height[1] = s->height[2] = h;
        break;
    case VMAF_PIX_FMT_YUV444P:
        s->width[1] = s->width[2] = w;
        s->height[1] = s->height[2] = h;
        break;
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_cuda: unsupported pix_fmt\n");
        return -EINVAL;
    }
    return 0;
}

static int configure_hvs_blocks(PsnrHvsStateCuda *s)
{
    s->total_blocks = 0U;
    for (unsigned plane = 0; plane < s->n_planes; plane++) {
        if (s->width[plane] < (unsigned)PSNR_HVS_BLOCK ||
            s->height[plane] < (unsigned)PSNR_HVS_BLOCK) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "psnr_hvs_cuda: plane %u dims %ux%u smaller than 8x8 block\n", plane,
                     s->width[plane], s->height[plane]);
            return -EINVAL;
        }
        s->num_blocks_x[plane] = (s->width[plane] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1;
        s->num_blocks_y[plane] = (s->height[plane] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1;
        s->num_blocks[plane] = s->num_blocks_x[plane] * s->num_blocks_y[plane];
        s->first_block[plane] = s->total_blocks;
        s->total_blocks += s->num_blocks[plane];
    }
    return 0;
}

static int psnr_hvs_init_unwind(VmafFeatureExtractor *fex, PsnrHvsStateCuda *s, int cause)
{
    int rc = cause;
    const int phase_rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (phase_rc && !rc)
        rc = phase_rc;

    const int rb_rc = vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    if (rb_rc && !rc)
        rc = rb_rc;

    const int dict_rc = vmaf_dictionary_free(&s->feature_name_dict);
    if (dict_rc && !rc)
        rc = dict_rc;

    const int mod_rc = vmaf_cuda_module_unload(fex->cu_state, &s->module);
    if (mod_rc && !rc)
        rc = mod_rc;

    return rc;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    PsnrHvsStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    int err = validate_hvs_input(bpc, w, h);
    if (err)
        return err;

    s->bpc = bpc;
    const int32_t samplemax = (1 << bpc) - 1;
    s->samplemax_sq = samplemax * samplemax;

    err = configure_hvs_geometry(s, pix_fmt, w, h);
    if (err)
        return err;
    err = configure_hvs_blocks(s);
    if (err)
        return err;

    err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return psnr_hvs_init_unwind(fex, s, err);

    int ctx_pushed = 0;
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, psnr_hvs_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_psnr_hvs, s->module, "psnr_hvs"), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);

    const size_t partials_bytes = (size_t)s->total_blocks * sizeof(float);
    err = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state, partials_bytes);
    if (err)
        return psnr_hvs_init_unwind(fex, s, err);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return psnr_hvs_init_unwind(fex, s, -ENOMEM);

    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
    return psnr_hvs_init_unwind(fex, s, _cuda_err);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    PsnrHvsStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    s->index = index;

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);
    CUevent dist_ready = vmaf_cuda_picture_get_ready_event(dist_pic);

    PsnrHvsKernelArgs args;
    memset(&args, 0, sizeof(args));
    for (unsigned p = 0; p < s->n_planes; p++) {
        args.plane[p].ref = ref_pic->data[p];
        args.plane[p].dist = dist_pic->data[p];
        args.plane[p].ref_stride = (size_t)ref_pic->stride[p];
        args.plane[p].dist_stride = (size_t)dist_pic->stride[p];
        args.plane[p].width = s->width[p];
        args.plane[p].blocks_x = s->num_blocks_x[p];
        args.plane[p].first_block = s->first_block[p];
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr): Driver API device address the kernel dereferences (ADR-0747)
    args.partials = (float *)s->rb.device->data;
    args.n_planes = s->n_planes;
    args.total_blocks = s->total_blocks;
    args.wide = (s->bpc > 8u) ? 1 : 0;

    int err =
        vmaf_cuda_kernel_submit_pre_launch(&s->lc, fex->cu_state, &s->rb, pic_stream, dist_ready);
    if (err)
        return err;

    const size_t items = 2U * (size_t)s->total_blocks;
    const unsigned grid_x = (unsigned)((items + (size_t)PSNR_HVS_WG - 1U) / (size_t)PSNR_HVS_WG);
    void *kernel_params[] = {&args};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_psnr_hvs, grid_x, 1u, 1u, PSNR_HVS_WG, 1u, 1u, 0,
                                           pic_stream, kernel_params, NULL));

    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    const size_t partials_bytes = (size_t)s->total_blocks * sizeof(float);
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rb.host_pinned, (CUdeviceptr)s->rb.device->data,
                                              partials_bytes, s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static void reduce_hvs_planes(const PsnrHvsStateCuda *s, double scores[PSNR_HVS_NUM_PLANES])
{
    // SAFETY: s->rb.host_pinned has s->total_blocks elements allocated, and
    // s->first_block[p] + s->num_blocks[p] <= s->total_blocks holds by construction.
    const float *partials = (const float *)s->rb.host_pinned;
    for (unsigned p = 0; p < s->n_planes; p++) {
        const float *plane_partials = partials + s->first_block[p];
        float sum = 0.0f;
        for (unsigned i = 0; i < s->num_blocks[p]; i++) {
            sum += plane_partials[i];
        }
        const int pixels = (int)(s->num_blocks[p] * 64u);
        sum /= (float)pixels;
        sum /= (float)s->samplemax_sq;
        scores[p] = (double)sum;
    }
}

static int append_hvs_scores(VmafFeatureCollector *collector, const PsnrHvsStateCuda *s,
                             const double scores[PSNR_HVS_NUM_PLANES], unsigned index)
{
    static const char *const plane_features[PSNR_HVS_NUM_PLANES] = {"psnr_hvs_y", "psnr_hvs_cb",
                                                                    "psnr_hvs_cr"};
    int err = 0;
    for (unsigned p = 0; p < s->n_planes; p++) {
        const double db = 10.0 * (-1.0 * log10(scores[p]));
        err |= vmaf_feature_collector_append(collector, plane_features[p], db, index);
    }
    const double combined =
        (s->n_planes == 1U) ? scores[0] : 0.8 * scores[0] + 0.1 * (scores[1] + scores[2]);
    const double db_combined = 10.0 * (-1.0 * log10(combined));
    err |= vmaf_feature_collector_append(collector, "psnr_hvs", db_combined, index);
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    PsnrHvsStateCuda *s = fex->priv;

    int err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (err)
        return err;

    double plane_scores[PSNR_HVS_NUM_PLANES] = {0.0, 0.0, 0.0};
    reduce_hvs_planes(s, plane_scores);
    return append_hvs_scores(feature_collector, s, plane_scores, index);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    PsnrHvsStateCuda *s = fex->priv;
    return psnr_hvs_init_unwind(fex, s, 0);
}

static const char *provided_features[] = {"psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr", "psnr_hvs",
                                          NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_psnr_hvs_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_psnr_hvs_cuda = {
    .name = "psnr_hvs_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(PsnrHvsStateCuda),
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

/* NOLINTEND(modernize-use-nullptr) */
