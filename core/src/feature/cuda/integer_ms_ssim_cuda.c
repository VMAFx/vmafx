/* Upstream-mirror filename: defines float_ms_ssim symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  float_ms_ssim feature extractor on the CUDA backend
 *  (T7-23 / ADR-0188 / ADR-0190, GPU long-tail batch 2 part 2b).
 *  CUDA twin of ms_ssim_vulkan (PR #141).
 *
 *  5-level pyramid + 3-output SSIM per scale + host-side Wang
 *  product combine. Three CUDA kernels (see ms_ssim_score.cu):
 *
 *    1. ms_ssim_decimate — 9-tap 9/7 biorthogonal LPF + 2×
 *       downsample (mirrors ms_ssim_decimate.c byte-for-byte).
 *    2. ms_ssim_horiz — horizontal 11-tap separable Gaussian
 *       over 5 SSIM stats (operates on float input — pyramid
 *       levels are already float).
 *    3. ms_ssim_vert_lcs — vertical 11-tap + per-pixel l/c/s +
 *       per-block float partials × 3.
 *
 *  picture_copy normalisation runs on the host (uint sample →
 *  float in [0, 255]), uploaded to the pyramid level 0 buffer.
 *  CUDA decimate kernels build levels 1-4. Per-scale SSIM
 *  compute reads levels and writes into shared intermediate +
 *  per-scale partials buffers; host accumulates partials in
 *  `double` per scale and applies the Wang weights for the final
 *  product combine.
 *
 *  Min-dim guard: 11 << 4 = 176 (matches ADR-0153).
 *
 *  enable_lcs (T7-35 / ADR-0243): when set, emits the 15 extra
 *  per-scale metrics float_ms_ssim_{l,c,s}_scale{0..4}. The
 *  vert_lcs kernel already produces the per-scale L/C/S means
 *  (it's where the "_lcs" in its name comes from); gating the
 *  feature_collector_append calls leaves the default-path
 *  output bit-identical to the pre-T7-35 binary.
 *
 *  Engine-scope fence batching (T-GPU-OPT-2 / ADR-0271): all 5
 *  scales' horiz + vert_lcs launches and DtoH partial readbacks
 *  are enqueued in submit() onto the lifecycle's private stream
 *  (s->lc.str). Same-stream ordering serialises kernels and
 *  copies in dependency order without per-scale syncs, and the
 *  partials buffers are now allocated per-scale to avoid the
 *  cross-scale aliasing that previously forced a host-blocking
 *  cuStreamSynchronize after each scale. The final
 *  cuEventRecord(s->lc.finished, s->lc.str) opts the lifecycle
 *  into the engine's drain batch (drain_batch.h) so the
 *  per-frame readbacks coalesce with the rest of the CUDA
 *  feature stack — collect() then becomes a host-side reduction
 *  only.
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
#include "cuda/drain_batch.h"
#include "cuda/integer_ms_ssim_cuda.h"
#include "cuda/kernel_template.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "picture_copy.h"
#include "cuda_helper.cuh"

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define MS_SSIM_SCALES 5
#define MS_SSIM_GAUSSIAN_LEN 11
#define MS_SSIM_K 11
#define MS_SSIM_BLOCK_X 16
#define MS_SSIM_BLOCK_Y 8

static const float g_alphas[MS_SSIM_SCALES] = {0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.1333f};
static const float g_betas[MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};
static const float g_gammas[MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};

typedef struct MsSsimStateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). Multi-buffer pyramid state stays outside the
     * template's single-pair readback bundle. */
    VmafCudaKernelLifecycle lc;
    CUfunction func_decimate;
    CUfunction func_horiz;
    CUfunction func_vert_lcs;

    unsigned width;
    unsigned height;
    unsigned bpc;

    unsigned scale_w[MS_SSIM_SCALES];
    unsigned scale_h[MS_SSIM_SCALES];
    unsigned scale_w_horiz[MS_SSIM_SCALES];
    unsigned scale_h_horiz[MS_SSIM_SCALES];
    unsigned scale_w_final[MS_SSIM_SCALES];
    unsigned scale_h_final[MS_SSIM_SCALES];
    unsigned scale_grid_x[MS_SSIM_SCALES];
    unsigned scale_grid_y[MS_SSIM_SCALES];
    unsigned scale_block_count[MS_SSIM_SCALES];

    /* ADR-0990: c1/c2/c3 promoted to double so the kernel receives
     * double arguments matching the scalar reference precision. */
    double c1;
    double c2;
    double c3;

    /* Pyramid: 5 levels × ref + cmp, all float. */
    VmafCudaBuffer *pyramid_ref[MS_SSIM_SCALES];
    VmafCudaBuffer *pyramid_cmp[MS_SSIM_SCALES];

    /* Pinned host buffer for picture_copy → upload at scale 0. */
    float *h_ref;
    float *h_cmp;

    /* SSIM intermediates sized for scale 0 (largest). */
    VmafCudaBuffer *h_ref_mu;
    VmafCudaBuffer *h_cmp_mu;
    VmafCudaBuffer *h_ref_sq;
    VmafCudaBuffer *h_cmp_sq;
    VmafCudaBuffer *h_refcmp;

    /* Per-scale partials buffers (T-GPU-OPT-2 / ADR-0271).
     * Previously a single buffer reused across scales; that aliasing
     * forced a per-scale cuStreamSynchronize before the host could
     * walk the partials. Allocating per scale lets all 5 scales'
     * horiz + vert_lcs launches and DtoH copies queue back-to-back
     * on s->lc.str so the readbacks coalesce with the engine's
     * drain batch. */
    VmafCudaBuffer *l_partials[MS_SSIM_SCALES];
    VmafCudaBuffer *c_partials[MS_SSIM_SCALES];
    VmafCudaBuffer *s_partials[MS_SSIM_SCALES];
    /* Pinned host partials for DtoH (per scale; safe to read after
     * the lifecycle's finished event has been waited on, either
     * via the engine's drain_batch or the legacy per-stream sync).
     * ADR-0990: double to match the double partials written by the
     * ms_ssim_vert_lcs kernel. */
    double *h_l_partials[MS_SSIM_SCALES];
    double *h_c_partials[MS_SSIM_SCALES];
    double *h_s_partials[MS_SSIM_SCALES];

    unsigned index;
    VmafDictionary *feature_name_dict;

    bool enable_lcs; /* T7-35 / ADR-0243: emit per-scale L/C/S triples. */
    /* CPU-option parity (wiring-audit-2026-05-16): enable_db / clip_db match
     * float_ms_ssim.c options. At defaults (both false) output is bit-identical. */
    bool enable_db; /* return dB-domain score: -10*log10(1 - ms_ssim) */
    bool clip_db;   /* cap the dB output at the geometry-derived max_db */
    double max_db;  /* ADR-1221: dB ceiling, INFINITY when !clip_db */
    /* PTX module backing the MS-SSIM kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
} MsSsimStateCuda;

static const VmafOption options[] = {
    {
        .name = "enable_lcs",
        .help = "enable luminance, contrast and structure intermediate output",
        .offset = offsetof(MsSsimStateCuda, enable_lcs),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_db",
        .help = "return dB-domain MS-SSIM score: -10*log10(1 - ms_ssim)",
        .offset = offsetof(MsSsimStateCuda, enable_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "clip_db",
        .help = "clip linear ms_ssim to [0, 1] before dB conversion",
        .offset = offsetof(MsSsimStateCuda, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {0},
};

static int close_fex_cuda(VmafFeatureExtractor *fex);

/* Mirrors float_ms_ssim.c::convert_to_db exactly. ADR-1221. */
static double ms_ssim_convert_to_db(double score, double max_db)
{
    /* score >= 1.0 makes log10(1-score) undefined (log10 of zero or negative)
     * yielding -Inf / NaN.  Return max_db directly for perfect similarity.  */
    if (score >= 1.0)
        return max_db;
    const double db = -10. * log10(1.0 - score);
    return db < max_db ? db : max_db;
}

static int configure_ms_ssim_state(MsSsimStateCuda *s, unsigned bpc, unsigned w, unsigned h)
{
    const unsigned min_dim = (unsigned)MS_SSIM_GAUSSIAN_LEN << (MS_SSIM_SCALES - 1);
    if (w < min_dim || h < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ms_ssim_cuda: input %ux%u too small; %d-level %d-tap MS-SSIM pyramid needs"
                 " >= %ux%u (Netflix#1414 / ADR-0153)\n",
                 w, h, MS_SSIM_SCALES, MS_SSIM_GAUSSIAN_LEN, min_dim, min_dim);
        return -EINVAL;
    }
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    const unsigned peak = (1u << bpc) - 1u;
    if (s->clip_db) {
        const double mse = 0.5 / (w * h);
        s->max_db = ceil(10. * log10(peak * peak / mse));
    } else {
        s->max_db = INFINITY;
    }
    s->scale_w[0] = w;
    s->scale_h[0] = h;
    for (int i = 1; i < MS_SSIM_SCALES; i++) {
        s->scale_w[i] = (s->scale_w[i - 1] / 2) + (s->scale_w[i - 1] & 1);
        s->scale_h[i] = (s->scale_h[i - 1] / 2) + (s->scale_h[i - 1] & 1);
    }
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        s->scale_w_horiz[i] = s->scale_w[i] - (MS_SSIM_K - 1);
        s->scale_h_horiz[i] = s->scale_h[i];
        s->scale_w_final[i] = s->scale_w[i] - (MS_SSIM_K - 1);
        s->scale_h_final[i] = s->scale_h[i] - (MS_SSIM_K - 1);
        s->scale_grid_x[i] = (s->scale_w_final[i] + MS_SSIM_BLOCK_X - 1) / MS_SSIM_BLOCK_X;
        s->scale_grid_y[i] = (s->scale_h_final[i] + MS_SSIM_BLOCK_Y - 1) / MS_SSIM_BLOCK_Y;
        s->scale_block_count[i] = s->scale_grid_x[i] * s->scale_grid_y[i];
    }
    const double luma_range = 255.0;
    s->c1 = (0.01 * luma_range) * (0.01 * luma_range);
    s->c2 = (0.03 * luma_range) * (0.03 * luma_range);
    s->c3 = s->c2 * 0.5;
    return 0;
}

static int allocate_ms_ssim_device_buffers(VmafFeatureExtractor *fex, MsSsimStateCuda *s)
{
    int ret = 0;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const size_t bytes = (size_t)s->scale_w[i] * s->scale_h[i] * sizeof(float);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->pyramid_ref[i], bytes);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->pyramid_cmp[i], bytes);
    }
    const size_t horiz_bytes = (size_t)s->scale_w_horiz[0] * s->scale_h_horiz[0] * sizeof(float);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_ref_mu, horiz_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_cmp_mu, horiz_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_ref_sq, horiz_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_cmp_sq, horiz_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_refcmp, horiz_bytes);
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const size_t bytes = (size_t)s->scale_block_count[i] * sizeof(double);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->l_partials[i], bytes);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->c_partials[i], bytes);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->s_partials[i], bytes);
    }
    return ret;
}

static int allocate_ms_ssim_host_buffers(VmafFeatureExtractor *fex, MsSsimStateCuda *s)
{
    const size_t input_bytes = (size_t)s->width * s->height * sizeof(float);
    int ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_ref, input_bytes);
    ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_cmp, input_bytes);
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const size_t bytes = (size_t)s->scale_block_count[i] * sizeof(double);
        ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_l_partials[i], bytes);
        ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_c_partials[i], bytes);
        ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_s_partials[i], bytes);
    }
    return ret;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    MsSsimStateCuda *s = fex->priv;

    int err = configure_ms_ssim_state(s, bpc, w, h);
    if (err)
        return err;
    err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    CudaFunctions *cu_f = fex->cu_state->f;
    int _cuda_err;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, ms_ssim_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_decimate, s->module, "ms_ssim_decimate"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_horiz, s->module, "ms_ssim_horiz"), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_vert_lcs, s->module, "ms_ssim_vert_lcs"),
                    fail);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(VMAF_NULLPTR), fail_after_pop);

    int ret = allocate_ms_ssim_device_buffers(fex, s);
    ret |= allocate_ms_ssim_host_buffers(fex, s);
    if (ret) {
        (void)close_fex_cuda(fex);
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_fex_cuda(fex);
        return -ENOMEM;
    }

    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
fail_after_pop:
    (void)close_fex_cuda(fex);
    return _cuda_err;
}

static int download_and_normalize_picture(VmafFeatureExtractor *fex, const MsSsimStateCuda *s,
                                          const VmafPicture *pic, CUstream stream, void *tmp_uint,
                                          float *dst)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const unsigned bpc_bytes = s->bpc <= 8 ? 1u : 2u;
    CUDA_MEMCPY2D copy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = (CUdeviceptr)pic->data[0],
        .srcPitch = (size_t)pic->stride[0],
        .dstMemoryType = CU_MEMORYTYPE_HOST,
        .dstHost = tmp_uint,
        .dstPitch = (size_t)s->width * bpc_bytes,
        .WidthInBytes = (size_t)s->width * bpc_bytes,
        .Height = s->height,
    };
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(stream));
    VmafPicture host_pic = {
        .pix_fmt = pic->pix_fmt,
        .bpc = pic->bpc,
        .w = {pic->w[0], 0, 0},
        .h = {pic->h[0], 0, 0},
        .stride = {(ptrdiff_t)((size_t)s->width * bpc_bytes), 0, 0},
        .data = {tmp_uint, VMAF_NULLPTR, VMAF_NULLPTR},
    };
    picture_copy(dst, (ptrdiff_t)((size_t)s->width * sizeof(float)), &host_pic, 0, pic->bpc, 0);
    return 0;
}

static int upload_ms_ssim_level_zero(VmafFeatureExtractor *fex, const MsSsimStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const size_t bytes = (size_t)s->width * s->height * sizeof(float);
    CHECK_CUDA_RETURN(
        cu_f, cuMemcpyHtoDAsync((CUdeviceptr)s->pyramid_ref[0]->data, s->h_ref, bytes, s->lc.str));
    CHECK_CUDA_RETURN(
        cu_f, cuMemcpyHtoDAsync((CUdeviceptr)s->pyramid_cmp[0]->data, s->h_cmp, bytes, s->lc.str));
    return 0;
}

static int prepare_ms_ssim_level_zero(VmafFeatureExtractor *fex, MsSsimStateCuda *s,
                                      const VmafPicture *ref_pic, const VmafPicture *dist_pic)
{
    const unsigned bpc_bytes = s->bpc <= 8 ? 1u : 2u;
    const size_t bytes = (size_t)s->width * s->height * bpc_bytes;
    void *tmp_uint = VMAF_NULLPTR;
    int ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, &tmp_uint, bytes);
    if (ret)
        return -ENOMEM;
    CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    ret = download_and_normalize_picture(fex, s, ref_pic, stream, tmp_uint, s->h_ref);
    if (!ret)
        ret = download_and_normalize_picture(fex, s, dist_pic, stream, tmp_uint, s->h_cmp);
    if (!ret)
        ret = upload_ms_ssim_level_zero(fex, s);
    (void)vmaf_cuda_buffer_host_free(fex->cu_state, tmp_uint);
    return ret;
}

static int launch_ms_ssim_pyramid(VmafFeatureExtractor *fex, MsSsimStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    for (int i = 0; i < MS_SSIM_SCALES - 1; i++) {
        const unsigned w_in = s->scale_w[i];
        const unsigned h_in = s->scale_h[i];
        const unsigned w_out = s->scale_w[i + 1];
        const unsigned h_out = s->scale_h[i + 1];
        const unsigned grid_x = (w_out + MS_SSIM_BLOCK_X - 1) / MS_SSIM_BLOCK_X;
        const unsigned grid_y = (h_out + MS_SSIM_BLOCK_Y - 1) / MS_SSIM_BLOCK_Y;
        for (int side = 0; side < 2; side++) {
            VmafCudaBuffer *src = side == 0 ? s->pyramid_ref[i] : s->pyramid_cmp[i];
            VmafCudaBuffer *dst = side == 0 ? s->pyramid_ref[i + 1] : s->pyramid_cmp[i + 1];
            void *params[] = {
                (void *)src,   (void *)dst,    (void *)&w_in,
                (void *)&h_in, (void *)&w_out, (void *)&h_out,
            };
            CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_decimate, grid_x, grid_y, 1,
                                                   MS_SSIM_BLOCK_X, MS_SSIM_BLOCK_Y, 1, 0,
                                                   s->lc.str, params, VMAF_NULLPTR));
        }
    }
    return 0;
}

static int launch_ms_ssim_scale(VmafFeatureExtractor *fex, MsSsimStateCuda *s, int scale)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const unsigned width = s->scale_w[scale];
    const unsigned w_horiz = s->scale_w_horiz[scale];
    const unsigned h_horiz = s->scale_h_horiz[scale];
    const unsigned w_final = s->scale_w_final[scale];
    const unsigned h_final = s->scale_h_final[scale];
    const unsigned grid_x = (w_horiz + MS_SSIM_BLOCK_X - 1) / MS_SSIM_BLOCK_X;
    const unsigned grid_y = (h_horiz + MS_SSIM_BLOCK_Y - 1) / MS_SSIM_BLOCK_Y;
    void *horiz_params[] = {
        (void *)s->pyramid_ref[scale],
        (void *)s->pyramid_cmp[scale],
        (void *)s->h_ref_mu,
        (void *)s->h_cmp_mu,
        (void *)s->h_ref_sq,
        (void *)s->h_cmp_sq,
        (void *)s->h_refcmp,
        (void *)&width,
        (void *)&w_horiz,
        (void *)&h_horiz,
    };
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_horiz, grid_x, grid_y, 1, MS_SSIM_BLOCK_X,
                                     MS_SSIM_BLOCK_Y, 1, 0, s->lc.str, horiz_params, VMAF_NULLPTR));
    void *vert_params[] = {
        (void *)s->h_ref_mu,
        (void *)s->h_cmp_mu,
        (void *)s->h_ref_sq,
        (void *)s->h_cmp_sq,
        (void *)s->h_refcmp,
        (void *)s->l_partials[scale],
        (void *)s->c_partials[scale],
        (void *)s->s_partials[scale],
        (void *)&w_horiz,
        (void *)&w_final,
        (void *)&h_final,
        (void *)&s->c1,
        (void *)&s->c2,
        (void *)&s->c3,
    };
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_vert_lcs, s->scale_grid_x[scale],
                                     s->scale_grid_y[scale], 1, MS_SSIM_BLOCK_X, MS_SSIM_BLOCK_Y, 1,
                                     0, s->lc.str, vert_params, VMAF_NULLPTR));
    const size_t bytes = (size_t)s->scale_block_count[scale] * sizeof(double);
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_l_partials[scale],
                                        (CUdeviceptr)s->l_partials[scale]->data, bytes, s->lc.str));
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_c_partials[scale],
                                        (CUdeviceptr)s->c_partials[scale]->data, bytes, s->lc.str));
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_s_partials[scale],
                                        (CUdeviceptr)s->s_partials[scale]->data, bytes, s->lc.str));
    return 0;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                           const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                           const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    MsSsimStateCuda *s = fex->priv;
    s->index = index;
    int ret = prepare_ms_ssim_level_zero(fex, s, ref_pic, dist_pic);
    if (ret)
        return ret;
    ret = launch_ms_ssim_pyramid(fex, s);
    if (ret)
        return ret;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        ret = launch_ms_ssim_scale(fex, s, i);
        if (ret)
            return ret;
    }
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.finished, s->lc.str));
    (void)vmaf_cuda_drain_batch_register(&s->lc);
    return 0;
}

static void compute_ms_ssim_means(const MsSsimStateCuda *s, double *l_means, double *c_means,
                                  double *s_means)
{
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        double total_l = 0.0;
        double total_c = 0.0;
        double total_s = 0.0;
        for (unsigned j = 0; j < s->scale_block_count[i]; j++) {
            total_l += s->h_l_partials[i][j];
            total_c += s->h_c_partials[i][j];
            total_s += s->h_s_partials[i][j];
        }
        const double n_pixels = (double)s->scale_w_final[i] * (double)s->scale_h_final[i];
        l_means[i] = total_l / n_pixels;
        c_means[i] = total_c / n_pixels;
        s_means[i] = total_s / n_pixels;
    }
}

static int append_ms_ssim_lcs(VmafFeatureCollector *feature_collector, const double *l_means,
                              const double *c_means, const double *s_means, unsigned index)
{
    static const char *const l_names[MS_SSIM_SCALES] = {
        "float_ms_ssim_l_scale0", "float_ms_ssim_l_scale1", "float_ms_ssim_l_scale2",
        "float_ms_ssim_l_scale3", "float_ms_ssim_l_scale4",
    };
    static const char *const c_names[MS_SSIM_SCALES] = {
        "float_ms_ssim_c_scale0", "float_ms_ssim_c_scale1", "float_ms_ssim_c_scale2",
        "float_ms_ssim_c_scale3", "float_ms_ssim_c_scale4",
    };
    static const char *const s_names[MS_SSIM_SCALES] = {
        "float_ms_ssim_s_scale0", "float_ms_ssim_s_scale1", "float_ms_ssim_s_scale2",
        "float_ms_ssim_s_scale3", "float_ms_ssim_s_scale4",
    };
    int err = 0;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        err |= vmaf_feature_collector_append(feature_collector, l_names[i], l_means[i], index);
        err |= vmaf_feature_collector_append(feature_collector, c_names[i], c_means[i], index);
        err |= vmaf_feature_collector_append(feature_collector, s_names[i], s_means[i], index);
    }
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    MsSsimStateCuda *s = fex->priv;

    /* Wait for all 5 scales' DtoH copies to land. Fast path
     * (T-GPU-OPT-2 / ADR-0271): when the engine has already drained
     * lc.finished as part of its batched flush, this is a no-op
     * (lc.drained is true → reset and return). Otherwise falls back
     * to cuStreamSynchronize(lc.str). */
    int wait_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (wait_err)
        return wait_err;

    double l_means[MS_SSIM_SCALES] = {0};
    double c_means[MS_SSIM_SCALES] = {0};
    double s_means[MS_SSIM_SCALES] = {0};
    compute_ms_ssim_means(s, l_means, c_means, s_means);

    double msssim = 1.0;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        msssim *= pow(l_means[i], (double)g_alphas[i]) * pow(c_means[i], (double)g_betas[i]) *
                  pow(fabs(s_means[i]), (double)g_gammas[i]);
    }

    /* dB conversion — mirrors CPU float_ms_ssim.c behaviour exactly. */
    double score = msssim;
    if (s->enable_db)
        score = ms_ssim_convert_to_db(score, s->max_db);

    /* Append the (possibly dB-converted) score, not the raw msssim.
     * Mirrors CPU float_ms_ssim.c:extract() which converts before appending. */
    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_ms_ssim", score, index);
    if (s->enable_lcs)
        err |= append_ms_ssim_lcs(feature_collector, l_means, c_means, s_means, index);
    return err;
}

static int free_ms_ssim_device_buffer(VmafFeatureExtractor *fex, VmafCudaBuffer **buffer)
{
    if (!*buffer)
        return 0;
    const int ret = vmaf_cuda_buffer_free(fex->cu_state, *buffer);
    free(*buffer);
    *buffer = VMAF_NULLPTR;
    return ret;
}

static int free_ms_ssim_host_buffer(VmafFeatureExtractor *fex, void **buffer)
{
    if (!*buffer)
        return 0;
    const int ret = vmaf_cuda_buffer_host_free(fex->cu_state, *buffer);
    *buffer = VMAF_NULLPTR;
    return ret;
}

static int free_ms_ssim_scale_buffers(VmafFeatureExtractor *fex, MsSsimStateCuda *s, int scale)
{
    int ret = free_ms_ssim_device_buffer(fex, &s->pyramid_ref[scale]);
    ret |= free_ms_ssim_device_buffer(fex, &s->pyramid_cmp[scale]);
    ret |= free_ms_ssim_device_buffer(fex, &s->l_partials[scale]);
    ret |= free_ms_ssim_device_buffer(fex, &s->c_partials[scale]);
    ret |= free_ms_ssim_device_buffer(fex, &s->s_partials[scale]);
    ret |= free_ms_ssim_host_buffer(fex, (void **)&s->h_l_partials[scale]);
    ret |= free_ms_ssim_host_buffer(fex, (void **)&s->h_c_partials[scale]);
    ret |= free_ms_ssim_host_buffer(fex, (void **)&s->h_s_partials[scale]);
    return ret;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    MsSsimStateCuda *s = fex->priv;
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    for (int i = 0; i < MS_SSIM_SCALES; i++)
        ret |= free_ms_ssim_scale_buffers(fex, s, i);
    ret |= free_ms_ssim_device_buffer(fex, &s->h_ref_mu);
    ret |= free_ms_ssim_device_buffer(fex, &s->h_cmp_mu);
    ret |= free_ms_ssim_device_buffer(fex, &s->h_ref_sq);
    ret |= free_ms_ssim_device_buffer(fex, &s->h_cmp_sq);
    ret |= free_ms_ssim_device_buffer(fex, &s->h_refcmp);
    ret |= free_ms_ssim_host_buffer(fex, (void **)&s->h_ref);
    ret |= free_ms_ssim_host_buffer(fex, (void **)&s->h_cmp);
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    const CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f && s->module) {
        (void)cu_f->cuModuleUnload(s->module);
        s->module = VMAF_NULLPTR;
    }
    return ret;
}

static const char *provided_features[] = {"float_ms_ssim", VMAF_NULLPTR};

VmafFeatureExtractor vmaf_fex_float_ms_ssim_cuda = {
    .name = "float_ms_ssim_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(MsSsimStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    .chars =
        {
            .n_dispatches_per_frame = 18,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
