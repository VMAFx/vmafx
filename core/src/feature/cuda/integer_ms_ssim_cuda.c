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
#include "feature/nonfinite_score.h"
#include "cuda/drain_batch.h"
#include "cuda/integer_ms_ssim_cuda.h"
#include "cuda/kernel_template.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "picture_copy.h"
#include "cuda_helper.cuh"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
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

    /* Pinned host buffers for picture staging and upload at scale 0. */
    void *h_input_uint;
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

static int ms_ssim_init_failure(VmafFeatureExtractor *fex, int cause)
{
    const int cleanup_rc = close_fex_cuda(fex);
    return cause ? cause : cleanup_rc;
}

static int ms_ssim_configure_geometry(MsSsimStateCuda *s, unsigned bpc, unsigned w, unsigned h)
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

    if (s->clip_db) {
        const unsigned peak = (1u << bpc) - 1u;
        const double mse = 0.5 / (w * h);
        s->max_db = ceil(10. * log10(peak * peak / mse));
    } else {
        s->max_db = INFINITY;
    }
    return 0;
}

static void ms_ssim_configure_scales(MsSsimStateCuda *s, unsigned w, unsigned h)
{
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
        s->scale_grid_x[i] =
            (s->scale_w_final[i] + (unsigned)MS_SSIM_BLOCK_X - 1) / (unsigned)MS_SSIM_BLOCK_X;
        s->scale_grid_y[i] =
            (s->scale_h_final[i] + (unsigned)MS_SSIM_BLOCK_Y - 1) / (unsigned)MS_SSIM_BLOCK_Y;
        s->scale_block_count[i] = s->scale_grid_x[i] * s->scale_grid_y[i];
    }

    const double L = 255.0;
    const double K1 = 0.01;
    const double K2 = 0.03;
    s->c1 = (K1 * L) * (K1 * L);
    s->c2 = (K2 * L) * (K2 * L);
    s->c3 = s->c2 * 0.5;
}

static int ms_ssim_load_kernels(VmafFeatureExtractor *fex, MsSsimStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, ms_ssim_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_decimate, s->module, "ms_ssim_decimate"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_horiz, s->module, "ms_ssim_horiz"), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_vert_lcs, s->module, "ms_ssim_vert_lcs"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
    return _cuda_err;
}

static int ms_ssim_alloc_device_buffers(VmafFeatureExtractor *fex, MsSsimStateCuda *s)
{
    int ret = 0;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const size_t plane_bytes = (size_t)s->scale_w[i] * s->scale_h[i] * sizeof(float);
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->pyramid_ref[i], plane_bytes);
        if (ret)
            return ret;
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->pyramid_cmp[i], plane_bytes);
        if (ret)
            return ret;
    }
    const size_t horiz_bytes_max =
        (size_t)s->scale_w_horiz[0] * s->scale_h_horiz[0] * sizeof(float);
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_ref_mu, horiz_bytes_max);
    if (ret)
        return ret;
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_cmp_mu, horiz_bytes_max);
    if (ret)
        return ret;
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_ref_sq, horiz_bytes_max);
    if (ret)
        return ret;
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_cmp_sq, horiz_bytes_max);
    if (ret)
        return ret;
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_refcmp, horiz_bytes_max);
    if (ret)
        return ret;

    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const size_t scale_bytes = (size_t)s->scale_block_count[i] * sizeof(double);
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->l_partials[i], scale_bytes);
        if (ret)
            return ret;
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->c_partials[i], scale_bytes);
        if (ret)
            return ret;
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->s_partials[i], scale_bytes);
        if (ret)
            return ret;
    }
    return ret;
}

static int ms_ssim_alloc_host_buffers(VmafFeatureExtractor *fex, MsSsimStateCuda *s)
{
    int ret = 0;
    const size_t raw_input_bytes = (size_t)s->width * s->height * (s->bpc <= 8 ? 1u : 2u);
    const size_t input_bytes = (size_t)s->width * s->height * sizeof(float);
    ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, &s->h_input_uint, raw_input_bytes);
    if (ret)
        return ret;
    ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_ref, input_bytes);
    if (ret)
        return ret;
    ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_cmp, input_bytes);
    if (ret)
        return ret;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const size_t scale_bytes = (size_t)s->scale_block_count[i] * sizeof(double);
        ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_l_partials[i], scale_bytes);
        if (ret)
            return ret;
        ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_c_partials[i], scale_bytes);
        if (ret)
            return ret;
        ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_s_partials[i], scale_bytes);
        if (ret)
            return ret;
    }
    return ret;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    MsSsimStateCuda *s = fex->priv;

    int err = ms_ssim_configure_geometry(s, bpc, w, h);
    if (err)
        return err;
    ms_ssim_configure_scales(s, w, h);

    err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return ms_ssim_init_failure(fex, err);
    err = ms_ssim_load_kernels(fex, s);
    if (err)
        return ms_ssim_init_failure(fex, err);

    int ret = ms_ssim_alloc_device_buffers(fex, s);
    if (ret)
        return ms_ssim_init_failure(fex, ret);
    ret = ms_ssim_alloc_host_buffers(fex, s);
    if (ret)
        return ms_ssim_init_failure(fex, ret);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return ms_ssim_init_failure(fex, -ENOMEM);

    return 0;
}

static int ms_ssim_copy_plane_to_host(CudaFunctions *cu_f, const VmafPicture *pic,
                                      const MsSsimStateCuda *s, CUstream stream, void *dst)
{
    const unsigned bpc_bytes = (s->bpc <= 8 ? 1u : 2u);
    CUDA_MEMCPY2D copy = {0};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = (CUdeviceptr)pic->data[0];
    copy.srcPitch = (size_t)pic->stride[0];
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = dst;
    copy.dstPitch = (size_t)s->width * bpc_bytes;
    copy.WidthInBytes = (size_t)s->width * bpc_bytes;
    copy.Height = s->height;
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(stream));
    return 0;
}

static void ms_ssim_normalize_plane(float *dst, const MsSsimStateCuda *s,
                                    const VmafPicture *device_pic, void *src)
{
    const unsigned bpc_bytes = (s->bpc <= 8 ? 1u : 2u);
    VmafPicture host_pic = {
        .pix_fmt = device_pic->pix_fmt,
        .bpc = device_pic->bpc,
        .w = {device_pic->w[0], 0, 0},
        .h = {device_pic->h[0], 0, 0},
        .stride = {(ptrdiff_t)((size_t)s->width * bpc_bytes), 0, 0},
        .data = {src, NULL, NULL},
    };
    picture_copy(dst, (ptrdiff_t)((size_t)s->width * sizeof(float)), &host_pic, 0, device_pic->bpc,
                 0);
}

static int ms_ssim_upload_level_zero(CudaFunctions *cu_f, const MsSsimStateCuda *s)
{
    const size_t bytes = (size_t)s->width * s->height * sizeof(float);
    CHECK_CUDA_RETURN(
        cu_f, cuMemcpyHtoDAsync((CUdeviceptr)s->pyramid_ref[0]->data, s->h_ref, bytes, s->lc.str));
    CHECK_CUDA_RETURN(
        cu_f, cuMemcpyHtoDAsync((CUdeviceptr)s->pyramid_cmp[0]->data, s->h_cmp, bytes, s->lc.str));
    return 0;
}

static int ms_ssim_stage_inputs(VmafFeatureExtractor *fex, MsSsimStateCuda *s, VmafPicture *ref_pic,
                                VmafPicture *dist_pic)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    int err = ms_ssim_copy_plane_to_host(cu_f, ref_pic, s, stream, s->h_input_uint);
    if (err == 0) {
        ms_ssim_normalize_plane(s->h_ref, s, ref_pic, s->h_input_uint);
        err = ms_ssim_copy_plane_to_host(cu_f, dist_pic, s, stream, s->h_input_uint);
    }
    if (err == 0) {
        ms_ssim_normalize_plane(s->h_cmp, s, dist_pic, s->h_input_uint);
        err = ms_ssim_upload_level_zero(cu_f, s);
    }
    return err;
}

static int ms_ssim_submit_pyramid(MsSsimStateCuda *s, CudaFunctions *cu_f)
{
    for (int i = 0; i < MS_SSIM_SCALES - 1; i++) {
        const unsigned w_in = s->scale_w[i];
        const unsigned h_in = s->scale_h[i];
        const unsigned w_out = s->scale_w[i + 1];
        const unsigned h_out = s->scale_h[i + 1];
        const unsigned grid_x = (w_out + MS_SSIM_BLOCK_X - 1) / MS_SSIM_BLOCK_X;
        const unsigned grid_y = (h_out + MS_SSIM_BLOCK_Y - 1) / MS_SSIM_BLOCK_Y;
        for (int side = 0; side < 2; side++) {
            VmafCudaBuffer *src = (side == 0) ? s->pyramid_ref[i] : s->pyramid_cmp[i];
            VmafCudaBuffer *dst = (side == 0) ? s->pyramid_ref[i + 1] : s->pyramid_cmp[i + 1];
            void *params[] = {
                (void *)src,   (void *)dst,    (void *)&w_in,
                (void *)&h_in, (void *)&w_out, (void *)&h_out,
            };
            CHECK_CUDA_RETURN(cu_f,
                              cuLaunchKernel(s->func_decimate, grid_x, grid_y, 1, MS_SSIM_BLOCK_X,
                                             MS_SSIM_BLOCK_Y, 1, 0, s->lc.str, params, NULL));
        }
    }
    return 0;
}

static int ms_ssim_submit_scale(MsSsimStateCuda *s, CudaFunctions *cu_f, int i)
{
    const unsigned width = s->scale_w[i];
    const unsigned w_horiz = s->scale_w_horiz[i];
    const unsigned h_horiz = s->scale_h_horiz[i];
    const unsigned w_final = s->scale_w_final[i];
    const unsigned h_final = s->scale_h_final[i];
    const unsigned grid_x = (w_horiz + MS_SSIM_BLOCK_X - 1) / MS_SSIM_BLOCK_X;
    const unsigned grid_y = (h_horiz + MS_SSIM_BLOCK_Y - 1) / MS_SSIM_BLOCK_Y;

    void *horiz_params[] = {
        (void *)s->pyramid_ref[i], (void *)s->pyramid_cmp[i],
        (void *)s->h_ref_mu,       (void *)s->h_cmp_mu,
        (void *)s->h_ref_sq,       (void *)s->h_cmp_sq,
        (void *)s->h_refcmp,       (void *)&width,
        (void *)&w_horiz,          (void *)&h_horiz,
    };
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_horiz, grid_x, grid_y, 1, MS_SSIM_BLOCK_X,
                                           MS_SSIM_BLOCK_Y, 1, 0, s->lc.str, horiz_params, NULL));

    void *vert_params[] = {
        (void *)s->h_ref_mu,      (void *)s->h_cmp_mu,      (void *)s->h_ref_sq,
        (void *)s->h_cmp_sq,      (void *)s->h_refcmp,      (void *)s->l_partials[i],
        (void *)s->c_partials[i], (void *)s->s_partials[i], (void *)&w_horiz,
        (void *)&w_final,         (void *)&h_final,         (void *)&s->c1,
        (void *)&s->c2,           (void *)&s->c3,
    };
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_vert_lcs, s->scale_grid_x[i], s->scale_grid_y[i],
                                           1, MS_SSIM_BLOCK_X, MS_SSIM_BLOCK_Y, 1, 0, s->lc.str,
                                           vert_params, NULL));

    const size_t bytes = (size_t)s->scale_block_count[i] * sizeof(double);
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_l_partials[i], (CUdeviceptr)s->l_partials[i]->data,
                                        bytes, s->lc.str));
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_c_partials[i], (CUdeviceptr)s->c_partials[i]->data,
                                        bytes, s->lc.str));
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_s_partials[i], (CUdeviceptr)s->s_partials[i]->data,
                                        bytes, s->lc.str));
    return 0;
}

static int ms_ssim_submit_scales(MsSsimStateCuda *s, CudaFunctions *cu_f)
{
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const int err = ms_ssim_submit_scale(s, cu_f, i);
        if (err)
            return err;
    }
    return 0;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    MsSsimStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;
    int err = ms_ssim_stage_inputs(fex, s, ref_pic, dist_pic);
    if (err)
        return err;

    err = ms_ssim_submit_pyramid(s, cu_f);
    if (err)
        return err;

    err = ms_ssim_submit_scales(s, cu_f);
    if (err)
        return err;

    /* Fence the final readback and opt the lifecycle into the
     * engine's drain batch. drain_batch_register is best-effort:
     * when no batch is open or the cap is hit, lc.drained stays
     * false and collect() falls back to the legacy per-stream
     * cuStreamSynchronize via vmaf_cuda_kernel_collect_wait. */
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.finished, s->lc.str));
    (void)vmaf_cuda_drain_batch_register(&s->lc);
    return 0;
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
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const unsigned w_final = s->scale_w_final[i];
        const unsigned h_final = s->scale_h_final[i];
        double total_l = 0.0;
        double total_c = 0.0;
        double total_s = 0.0;
        /* ADR-0990: h_*_partials are now double arrays; no cast needed. */
        for (unsigned j = 0; j < s->scale_block_count[i]; j++) {
            total_l += s->h_l_partials[i][j];
            total_c += s->h_c_partials[i][j];
            total_s += s->h_s_partials[i][j];
        }
        const double n_pixels = (double)w_final * (double)h_final;
        l_means[i] = total_l / n_pixels;
        c_means[i] = total_c / n_pixels;
        s_means[i] = total_s / n_pixels;
    }

    double msssim = 1.0;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        msssim *= pow(l_means[i], (double)g_alphas[i]) * pow(c_means[i], (double)g_betas[i]) *
                  pow(fabs(s_means[i]), (double)g_gammas[i]);
    }
    return vmaf_ms_ssim_emit_scores(feature_collector, s->feature_name_dict, "float_ms_ssim_cuda",
                                    "float_ms_ssim", msssim, s->enable_db, s->max_db, l_means,
                                    c_means, s_means, MS_SSIM_SCALES, s->enable_lcs, index);
}

static int ms_ssim_free_device_buffer(VmafCudaState *cu_state, VmafCudaBuffer **buffer)
{
    return vmaf_cuda_buffer_free_owned(cu_state, buffer);
}

static int ms_ssim_free_host_buffer(VmafCudaState *cu_state, void **buffer)
{
    return vmaf_cuda_buffer_host_free_owned(cu_state, buffer);
}

static int ms_ssim_free_pyramid(VmafCudaState *cu_state, MsSsimStateCuda *s)
{
    int rc = 0;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        int e = ms_ssim_free_device_buffer(cu_state, &s->pyramid_ref[i]);
        if (e && !rc)
            rc = e;
        e = ms_ssim_free_device_buffer(cu_state, &s->pyramid_cmp[i]);
        if (e && !rc)
            rc = e;
    }
    return rc;
}

static int ms_ssim_free_intermediates(VmafCudaState *cu_state, MsSsimStateCuda *s)
{
    int rc = ms_ssim_free_device_buffer(cu_state, &s->h_ref_mu);
    int e = ms_ssim_free_device_buffer(cu_state, &s->h_cmp_mu);
    if (e && !rc)
        rc = e;
    e = ms_ssim_free_device_buffer(cu_state, &s->h_ref_sq);
    if (e && !rc)
        rc = e;
    e = ms_ssim_free_device_buffer(cu_state, &s->h_cmp_sq);
    if (e && !rc)
        rc = e;
    e = ms_ssim_free_device_buffer(cu_state, &s->h_refcmp);
    if (e && !rc)
        rc = e;
    return rc;
}

static int ms_ssim_free_partials(VmafCudaState *cu_state, MsSsimStateCuda *s)
{
    int rc = 0;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        int e = ms_ssim_free_device_buffer(cu_state, &s->l_partials[i]);
        if (e && !rc)
            rc = e;
        e = ms_ssim_free_device_buffer(cu_state, &s->c_partials[i]);
        if (e && !rc)
            rc = e;
        e = ms_ssim_free_device_buffer(cu_state, &s->s_partials[i]);
        if (e && !rc)
            rc = e;
        e = ms_ssim_free_host_buffer(cu_state, (void **)&s->h_l_partials[i]);
        if (e && !rc)
            rc = e;
        e = ms_ssim_free_host_buffer(cu_state, (void **)&s->h_c_partials[i]);
        if (e && !rc)
            rc = e;
        e = ms_ssim_free_host_buffer(cu_state, (void **)&s->h_s_partials[i]);
        if (e && !rc)
            rc = e;
    }
    return rc;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    MsSsimStateCuda *s = fex->priv;
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (ret)
        return ret;

    int e = ms_ssim_free_pyramid(fex->cu_state, s);
    if (e && !ret)
        ret = e;
    e = ms_ssim_free_intermediates(fex->cu_state, s);
    if (e && !ret)
        ret = e;
    e = ms_ssim_free_host_buffer(fex->cu_state, &s->h_input_uint);
    if (e && !ret)
        ret = e;
    e = ms_ssim_free_host_buffer(fex->cu_state, (void **)&s->h_ref);
    if (e && !ret)
        ret = e;
    e = ms_ssim_free_host_buffer(fex->cu_state, (void **)&s->h_cmp);
    if (e && !ret)
        ret = e;
    e = ms_ssim_free_partials(fex->cu_state, s);
    if (e && !ret)
        ret = e;
    e = vmaf_dictionary_free(&s->feature_name_dict);
    if (e && !ret)
        ret = e;
    e = vmaf_cuda_module_unload(fex->cu_state, &s->module);
    if (e && !ret)
        ret = e;
    return ret;
}

static const char *provided_features[] = {"float_ms_ssim", NULL};

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

/* NOLINTEND(modernize-use-nullptr) */
