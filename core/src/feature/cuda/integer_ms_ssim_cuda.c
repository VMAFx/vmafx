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
 *    3. ms_ssim_vert_lcs — vertical 11-tap + per-window l/c/s,
 *       each stored at the window's raster position.
 *
 *  picture_copy normalisation runs on the device
 *  (ms_ssim_picture_to_float: uint sample → float, picture_copy()'s
 *  arithmetic) into the pyramid level 0 buffer; no plane goes
 *  through the host (T-CUDA-MS-SSIM-HOST-STAGING-2026-10-06).
 *  CUDA decimate kernels build levels 1-4. Per-scale SSIM
 *  compute reads levels and writes into shared intermediate +
 *  per-scale term planes; the host adds each scale's l, c and s
 *  terms in raster order, as iqa_ssim() does (ms_ssim_scale_sums(),
 *  ADR-1465), and applies the Wang weights for the final product
 *  combine.
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
 *  enable_chroma: float_ms_ssim.c runs the whole pipeline once per plane and
 *  emits float_ms_ssim_cb / float_ms_ssim_cr. So does this twin: every plane
 *  has its own geometry, pyramid and term planes (MsSsimPlaneCuda), the
 *  kernels and the host sums are the luma path's, and each chroma plane must
 *  clear the same 176-pixel minimum (a 4:2:0 input needs 351x351 luma).
 *  YUV400P scores luma only, as the CPU clears the option there.
 *
 *  Engine-scope fence batching (T-GPU-OPT-2 / ADR-0271): all 5
 *  scales' horiz + vert_lcs launches and DtoH term readbacks
 *  of every plane are enqueued in submit() onto the lifecycle's
 *  private stream (s->lc.str). Same-stream ordering serialises
 *  kernels and copies in dependency order without per-scale
 *  syncs, and the term planes are allocated per plane and scale
 *  to avoid the cross-scale aliasing that previously forced a
 *  host-blocking cuStreamSynchronize after each scale. The final
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
#include "feature/metal/float_ms_ssim_option_semantics.h"
#include "feature/nonfinite_score.h"
#include "cuda/drain_batch.h"
#include "cuda/integer_ms_ssim_cuda.h"
#include "cuda/kernel_template.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "cuda_helper.cuh"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define MS_SSIM_SCALES 5
#define MS_SSIM_MAX_PLANES 3
#define MS_SSIM_GAUSSIAN_LEN 11
#define MS_SSIM_K 11
#define MS_SSIM_BLOCK_X 16
#define MS_SSIM_BLOCK_Y 8

static const float g_alphas[MS_SSIM_SCALES] = {0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.1333f};
static const float g_betas[MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};
static const float g_gammas[MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};

/* float_ms_ssim.c's feature name of each plane. */
static const char *const ms_ssim_plane_names[MS_SSIM_MAX_PLANES] = {
    "float_ms_ssim",
    "float_ms_ssim_cb",
    "float_ms_ssim_cr",
};

/* Everything that varies per plane. A chroma plane of a subsampled format is
 * smaller than luma and every kernel takes these dimensions as row pitches,
 * so a chroma pass never reads a luma size. */
typedef struct MsSsimPlaneCuda {
    unsigned width;
    unsigned height;

    unsigned scale_w[MS_SSIM_SCALES];
    unsigned scale_h[MS_SSIM_SCALES];
    unsigned scale_w_horiz[MS_SSIM_SCALES];
    unsigned scale_h_horiz[MS_SSIM_SCALES];
    unsigned scale_w_final[MS_SSIM_SCALES];
    unsigned scale_h_final[MS_SSIM_SCALES];
    unsigned scale_grid_x[MS_SSIM_SCALES];
    unsigned scale_grid_y[MS_SSIM_SCALES];
    /* Windows of each scale, w_final * h_final: one l, c and s term each. */
    size_t scale_window_count[MS_SSIM_SCALES];

    /* Pyramid: 5 levels × ref + cmp, all float. */
    VmafCudaBuffer *pyramid_ref[MS_SSIM_SCALES];
    VmafCudaBuffer *pyramid_cmp[MS_SSIM_SCALES];

    /* Per-scale term planes, one term per window in raster order
     * (ADR-1465): l and c as doubles, s as the float it is. Allocated per
     * scale (T-GPU-OPT-2 / ADR-0271) so that all 5 scales' horiz + vert_lcs
     * launches and DtoH copies queue back-to-back on s->lc.str and the
     * readbacks coalesce with the engine's drain batch. */
    VmafCudaBuffer *l_terms[MS_SSIM_SCALES];
    VmafCudaBuffer *c_terms[MS_SSIM_SCALES];
    VmafCudaBuffer *s_terms[MS_SSIM_SCALES];
    /* Pinned host copies for DtoH (per scale; safe to read after
     * the lifecycle's finished event has been waited on, either
     * via the engine's drain_batch or the legacy per-stream sync). */
    double *h_l_terms[MS_SSIM_SCALES];
    double *h_c_terms[MS_SSIM_SCALES];
    float *h_s_terms[MS_SSIM_SCALES];
} MsSsimPlaneCuda;

/* l, c and s means of every scale of one plane, and the plane's score. */
typedef struct MsSsimPlaneScores {
    double l[MS_SSIM_SCALES];
    double c[MS_SSIM_SCALES];
    double s[MS_SSIM_SCALES];
    double score;
} MsSsimPlaneScores;

typedef struct MsSsimStateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). Multi-buffer pyramid state stays outside the
     * template's single-pair readback bundle. */
    VmafCudaKernelLifecycle lc;
    CUfunction func_to_float;
    CUfunction func_decimate;
    CUfunction func_horiz;
    CUfunction func_vert_lcs;

    /* Luma geometry; planes[p] holds each scored plane's own. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned n_planes;
    MsSsimPlaneCuda planes[MS_SSIM_MAX_PLANES];

    /* The reference's fp32 stabilisation constants (iqa_ssim()), carried as
     * doubles: the kernel's argument list takes doubles (ADR-0990) and
     * narrows them back without loss (ADR-1403). */
    double c1;
    double c2;
    double c3;

    /* SSIM intermediates sized for luma scale 0, the largest window plane of
     * any plane and scale; the in-order stream serialises their reuse. */
    VmafCudaBuffer *h_ref_mu;
    VmafCudaBuffer *h_cmp_mu;
    VmafCudaBuffer *h_ref_sq;
    VmafCudaBuffer *h_cmp_sq;
    VmafCudaBuffer *h_refcmp;

    unsigned index;
    VmafDictionary *feature_name_dict;

    bool enable_lcs; /* T7-35 / ADR-0243: emit per-scale L/C/S triples. */
    /* CPU-option parity (wiring-audit-2026-05-16): enable_db / clip_db match
     * float_ms_ssim.c options. At defaults (both false) output is bit-identical. */
    bool enable_db;     /* return dB-domain score: -10*log10(1 - ms_ssim) */
    bool clip_db;       /* cap the dB output at the geometry-derived max_db */
    bool enable_chroma; /* score Cb and Cr too, as float_ms_ssim.c does */
    double max_db;      /* ADR-1221: dB ceiling, INFINITY when !clip_db */
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
        .help = "cap dB-domain MS-SSIM at the geometry-derived ceiling",
        .offset = offsetof(MsSsimStateCuda, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_chroma",
        .help = "enable calculation for chroma channels (Cb and Cr)",
        .offset = offsetof(MsSsimStateCuda, enable_chroma),
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

/* float_ms_ssim.c's check_chroma_min_dim(): with enable_chroma every scored
 * plane walks the 5-level pyramid, so the subsampled planes must clear the
 * same minimum as luma. */
static int ms_ssim_check_chroma_min_dim(enum VmafPixelFormat pix_fmt, unsigned w, unsigned h,
                                        unsigned min_dim)
{
    unsigned chroma_w = 0u;
    unsigned chroma_h = 0u;
    vmaf_metal_ms_ssim_plane_dimensions(pix_fmt, 1u, w, h, &chroma_w, &chroma_h);
    if (chroma_w >= min_dim && chroma_h >= min_dim)
        return 0;

    unsigned luma_w = 0u;
    unsigned luma_h = 0u;
    vmaf_metal_ms_ssim_min_luma_dimensions(pix_fmt, min_dim, &luma_w, &luma_h);
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "ms_ssim_cuda: enable_chroma needs every plane to clear the pyramid minimum, "
             "but %ux%u luma gives %ux%u chroma and the %d-level %d-tap pyramid "
             "requires at least %ux%u. Use at least %ux%u luma for this pixel "
             "format, or leave enable_chroma off to score luma only.\n",
             w, h, chroma_w, chroma_h, MS_SSIM_SCALES, MS_SSIM_GAUSSIAN_LEN, min_dim, min_dim,
             luma_w, luma_h);
    return -EINVAL;
}

static int ms_ssim_configure_geometry(MsSsimStateCuda *s, enum VmafPixelFormat pix_fmt,
                                      unsigned bpc, unsigned w, unsigned h)
{
    const unsigned min_dim = (unsigned)MS_SSIM_GAUSSIAN_LEN << (MS_SSIM_SCALES - 1);
    if (w < min_dim || h < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ms_ssim_cuda: input %ux%u too small; %d-level %d-tap MS-SSIM pyramid needs"
                 " >= %ux%u (Netflix#1414 / ADR-0153)\n",
                 w, h, MS_SSIM_SCALES, MS_SSIM_GAUSSIAN_LEN, min_dim, min_dim);
        return -EINVAL;
    }

    /* YUV400P has no chroma planes: float_ms_ssim.c clears the option. */
    s->n_planes = vmaf_metal_ms_ssim_active_planes(s->enable_chroma, pix_fmt);
    if (s->n_planes > 1u) {
        const int err = ms_ssim_check_chroma_min_dim(pix_fmt, w, h, min_dim);
        if (err)
            return err;
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

static void ms_ssim_configure_plane_scales(MsSsimPlaneCuda *pl)
{
    pl->scale_w[0] = pl->width;
    pl->scale_h[0] = pl->height;
    for (int i = 1; i < MS_SSIM_SCALES; i++) {
        pl->scale_w[i] = (pl->scale_w[i - 1] / 2) + (pl->scale_w[i - 1] & 1);
        pl->scale_h[i] = (pl->scale_h[i - 1] / 2) + (pl->scale_h[i - 1] & 1);
    }
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        pl->scale_w_horiz[i] = pl->scale_w[i] - (MS_SSIM_K - 1);
        pl->scale_h_horiz[i] = pl->scale_h[i];
        pl->scale_w_final[i] = pl->scale_w[i] - (MS_SSIM_K - 1);
        pl->scale_h_final[i] = pl->scale_h[i] - (MS_SSIM_K - 1);
        pl->scale_grid_x[i] =
            (pl->scale_w_final[i] + (unsigned)MS_SSIM_BLOCK_X - 1) / (unsigned)MS_SSIM_BLOCK_X;
        pl->scale_grid_y[i] =
            (pl->scale_h_final[i] + (unsigned)MS_SSIM_BLOCK_Y - 1) / (unsigned)MS_SSIM_BLOCK_Y;
        pl->scale_window_count[i] = (size_t)pl->scale_w_final[i] * pl->scale_h_final[i];
    }
}

static void ms_ssim_configure_scales(MsSsimStateCuda *s, enum VmafPixelFormat pix_fmt)
{
    for (unsigned p = 0u; p < s->n_planes; p++) {
        MsSsimPlaneCuda *pl = &s->planes[p];
        vmaf_metal_ms_ssim_plane_dimensions(pix_fmt, p, s->width, s->height, &pl->width,
                                            &pl->height);
        ms_ssim_configure_plane_scales(pl);
    }

    /* iqa_ssim(): the stabilisation constants are fp32. */
    const int L = 255;
    const float K1 = 0.01f;
    const float K2 = 0.03f;
    const float C1 = (K1 * (float)L) * (K1 * (float)L);
    const float C2 = (K2 * (float)L) * (K2 * (float)L);
    const float C3 = C2 / 2.0f;
    s->c1 = (double)C1;
    s->c2 = (double)C2;
    s->c3 = (double)C3;
}

static int ms_ssim_load_kernels(VmafFeatureExtractor *fex, MsSsimStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, ms_ssim_score_ptx), fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->func_to_float, s->module, "ms_ssim_picture_to_float"), fail);
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

/* One plane's pyramid and term planes on the device. */
static int ms_ssim_alloc_plane_device(VmafCudaState *cu_state, MsSsimPlaneCuda *pl)
{
    int ret = 0;
    for (int i = 0; i < MS_SSIM_SCALES && !ret; i++) {
        const size_t plane_bytes = (size_t)pl->scale_w[i] * pl->scale_h[i] * sizeof(float);
        const size_t windows = pl->scale_window_count[i];
        ret = vmaf_cuda_buffer_alloc(cu_state, &pl->pyramid_ref[i], plane_bytes);
        if (!ret)
            ret = vmaf_cuda_buffer_alloc(cu_state, &pl->pyramid_cmp[i], plane_bytes);
        if (!ret)
            ret = vmaf_cuda_buffer_alloc(cu_state, &pl->l_terms[i], windows * sizeof(double));
        if (!ret)
            ret = vmaf_cuda_buffer_alloc(cu_state, &pl->c_terms[i], windows * sizeof(double));
        if (!ret)
            ret = vmaf_cuda_buffer_alloc(cu_state, &pl->s_terms[i], windows * sizeof(float));
    }
    return ret;
}

/* The five horizontal-pass planes, sized for luma scale 0. */
static int ms_ssim_alloc_intermediates(VmafCudaState *cu_state, MsSsimStateCuda *s)
{
    const MsSsimPlaneCuda *luma = &s->planes[0];
    const size_t horiz_bytes_max =
        (size_t)luma->scale_w_horiz[0] * luma->scale_h_horiz[0] * sizeof(float);
    int ret = vmaf_cuda_buffer_alloc(cu_state, &s->h_ref_mu, horiz_bytes_max);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(cu_state, &s->h_cmp_mu, horiz_bytes_max);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(cu_state, &s->h_ref_sq, horiz_bytes_max);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(cu_state, &s->h_cmp_sq, horiz_bytes_max);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(cu_state, &s->h_refcmp, horiz_bytes_max);
    return ret;
}

/* One plane's pinned term readbacks. */
static int ms_ssim_alloc_plane_host(VmafCudaState *cu_state, MsSsimPlaneCuda *pl)
{
    int ret = 0;
    for (int i = 0; i < MS_SSIM_SCALES && !ret; i++) {
        const size_t windows = pl->scale_window_count[i];
        ret = vmaf_cuda_buffer_host_alloc(cu_state, (void **)&pl->h_l_terms[i],
                                          windows * sizeof(double));
        if (!ret) {
            ret = vmaf_cuda_buffer_host_alloc(cu_state, (void **)&pl->h_c_terms[i],
                                              windows * sizeof(double));
        }
        if (!ret) {
            ret = vmaf_cuda_buffer_host_alloc(cu_state, (void **)&pl->h_s_terms[i],
                                              windows * sizeof(float));
        }
    }
    return ret;
}

/* Every buffer of the extractor. A failure leaves the rest NULL for
 * close_fex_cuda(), which frees whatever was allocated. */
static int ms_ssim_alloc_buffers(VmafFeatureExtractor *fex, MsSsimStateCuda *s)
{
    VmafCudaState *cu_state = fex->cu_state;
    int ret = ms_ssim_alloc_intermediates(cu_state, s);
    for (unsigned p = 0u; p < s->n_planes && !ret; p++) {
        ret = ms_ssim_alloc_plane_device(cu_state, &s->planes[p]);
        if (!ret)
            ret = ms_ssim_alloc_plane_host(cu_state, &s->planes[p]);
    }
    return ret;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    MsSsimStateCuda *s = fex->priv;

    int err = ms_ssim_configure_geometry(s, pix_fmt, bpc, w, h);
    if (err)
        return err;
    ms_ssim_configure_scales(s, pix_fmt);

    err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return ms_ssim_init_failure(fex, err);
    err = ms_ssim_load_kernels(fex, s);
    if (err)
        return ms_ssim_init_failure(fex, err);

    const int ret = ms_ssim_alloc_buffers(fex, s);
    if (ret)
        return ms_ssim_init_failure(fex, ret);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return ms_ssim_init_failure(fex, -ENOMEM);

    return 0;
}

/* picture_copy()'s divisor of a 2-byte sample (float_ms_ssim.c reads the
 * plane through it), or 0 where it reads one byte per sample (8 bits, and
 * the depths it has no case for). */
static float ms_ssim_sample_scaler(unsigned bpc)
{
    return bpc == 10u ? 4.0f : bpc == 12u ? 16.0f : bpc == 16u ? 256.0f : 0.0f;
}

/* Level 0 of `pyramid` from plane `plane` of a device picture, on `stream`:
 * picture_copy() of that plane on the device (ms_ssim_picture_to_float). */
static int ms_ssim_launch_to_float(const MsSsimStateCuda *s, CudaFunctions *cu_f,
                                   const VmafPicture *pic, unsigned plane,
                                   VmafCudaBuffer *const *pyramid, CUstream stream)
{
    const MsSsimPlaneCuda *pl = &s->planes[plane];
    CUdeviceptr src = (CUdeviceptr)pic->data[plane];
    size_t pitch = (size_t)pic->stride[plane];
    CUdeviceptr dst = pyramid[0]->data; /* the kernel's `float *dst` */
    unsigned w = pl->width;
    unsigned h = pl->height;
    float scaler = ms_ssim_sample_scaler(s->bpc);
    unsigned two_byte = scaler != 0.0f ? 1u : 0u;
    void *params[] = {&src, &pitch, &dst, &w, &h, &two_byte, &scaler};
    const unsigned grid_x = (w + MS_SSIM_BLOCK_X - 1) / MS_SSIM_BLOCK_X;
    const unsigned grid_y = (h + MS_SSIM_BLOCK_Y - 1) / MS_SSIM_BLOCK_Y;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_to_float, grid_x, grid_y, 1, MS_SSIM_BLOCK_X,
                                           MS_SSIM_BLOCK_Y, 1, 0, stream, params, NULL));
    return 0;
}

/* Both pictures' plane into level 0 of the pyramids, on the reference
 * picture's stream: it waits for the distorted picture's upload and for the
 * previous frame's work on the private stream (which read level 0), and the
 * private stream waits for the conversion. Each plane used to go to pinned
 * host memory with a host wait, be converted by picture_copy() on the host
 * and be uploaded again (T-CUDA-MS-SSIM-HOST-STAGING-2026-10-06). */
static int ms_ssim_stage_inputs(VmafFeatureExtractor *fex, MsSsimStateCuda *s, VmafPicture *ref_pic,
                                VmafPicture *dist_pic, unsigned plane)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const MsSsimPlaneCuda *pl = &s->planes[plane];
    CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                              CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(stream, s->lc.finished, CU_EVENT_WAIT_DEFAULT));
    int err = ms_ssim_launch_to_float(s, cu_f, ref_pic, plane, pl->pyramid_ref, stream);
    if (!err)
        err = ms_ssim_launch_to_float(s, cu_f, dist_pic, plane, pl->pyramid_cmp, stream);
    if (err)
        return err;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    return 0;
}

/* One decimation launch: level `i` of `pyramid` into level `i + 1`. The
 * parameter array follows ms_ssim_decimate's signature (ADR-1215);
 * cuLaunchKernel copies the values before it returns. */
static int ms_ssim_launch_decimate(MsSsimStateCuda *s, CudaFunctions *cu_f,
                                   const MsSsimPlaneCuda *pl, VmafCudaBuffer *const *pyramid, int i)
{
    const unsigned w_in = pl->scale_w[i];
    const unsigned h_in = pl->scale_h[i];
    const unsigned w_out = pl->scale_w[i + 1];
    const unsigned h_out = pl->scale_h[i + 1];
    const unsigned grid_x = (w_out + MS_SSIM_BLOCK_X - 1) / MS_SSIM_BLOCK_X;
    const unsigned grid_y = (h_out + MS_SSIM_BLOCK_Y - 1) / MS_SSIM_BLOCK_Y;
    void *params[] = {
        (void *)pyramid[i], (void *)pyramid[i + 1], (void *)&w_in,
        (void *)&h_in,      (void *)&w_out,         (void *)&h_out,
    };
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_decimate, grid_x, grid_y, 1, MS_SSIM_BLOCK_X,
                                           MS_SSIM_BLOCK_Y, 1, 0, s->lc.str, params, NULL));
    return 0;
}

/* Levels 1 to 4 of both pyramids of one plane, reference before distorted
 * per level, as before the split. */
static int ms_ssim_submit_pyramid(MsSsimStateCuda *s, CudaFunctions *cu_f,
                                  const MsSsimPlaneCuda *pl)
{
    for (int i = 0; i < MS_SSIM_SCALES - 1; i++) {
        int err = ms_ssim_launch_decimate(s, cu_f, pl, pl->pyramid_ref, i);
        if (!err)
            err = ms_ssim_launch_decimate(s, cu_f, pl, pl->pyramid_cmp, i);
        if (err)
            return err;
    }
    return 0;
}

static int ms_ssim_submit_scale(MsSsimStateCuda *s, CudaFunctions *cu_f, const MsSsimPlaneCuda *pl,
                                int i)
{
    const unsigned width = pl->scale_w[i];
    const unsigned w_horiz = pl->scale_w_horiz[i];
    const unsigned h_horiz = pl->scale_h_horiz[i];
    const unsigned w_final = pl->scale_w_final[i];
    const unsigned h_final = pl->scale_h_final[i];
    const unsigned grid_x = (w_horiz + MS_SSIM_BLOCK_X - 1) / MS_SSIM_BLOCK_X;
    const unsigned grid_y = (h_horiz + MS_SSIM_BLOCK_Y - 1) / MS_SSIM_BLOCK_Y;

    void *horiz_params[] = {
        (void *)pl->pyramid_ref[i], (void *)pl->pyramid_cmp[i],
        (void *)s->h_ref_mu,        (void *)s->h_cmp_mu,
        (void *)s->h_ref_sq,        (void *)s->h_cmp_sq,
        (void *)s->h_refcmp,        (void *)&width,
        (void *)&w_horiz,           (void *)&h_horiz,
    };
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_horiz, grid_x, grid_y, 1, MS_SSIM_BLOCK_X,
                                           MS_SSIM_BLOCK_Y, 1, 0, s->lc.str, horiz_params, NULL));

    void *vert_params[] = {
        (void *)s->h_ref_mu, (void *)s->h_cmp_mu,    (void *)s->h_ref_sq,    (void *)s->h_cmp_sq,
        (void *)s->h_refcmp, (void *)pl->l_terms[i], (void *)pl->c_terms[i], (void *)pl->s_terms[i],
        (void *)&w_horiz,    (void *)&w_final,       (void *)&h_final,       (void *)&s->c1,
        (void *)&s->c2,      (void *)&s->c3,
    };
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_vert_lcs, pl->scale_grid_x[i],
                                           pl->scale_grid_y[i], 1, MS_SSIM_BLOCK_X, MS_SSIM_BLOCK_Y,
                                           1, 0, s->lc.str, vert_params, NULL));

    const size_t windows = pl->scale_window_count[i];
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(pl->h_l_terms[i], (CUdeviceptr)pl->l_terms[i]->data,
                                              windows * sizeof(double), s->lc.str));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(pl->h_c_terms[i], (CUdeviceptr)pl->c_terms[i]->data,
                                              windows * sizeof(double), s->lc.str));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(pl->h_s_terms[i], (CUdeviceptr)pl->s_terms[i]->data,
                                              windows * sizeof(float), s->lc.str));
    return 0;
}

/* One plane's whole frame: staging, pyramid and every scale's terms. */
static int ms_ssim_submit_plane(VmafFeatureExtractor *fex, MsSsimStateCuda *s, VmafPicture *ref_pic,
                                VmafPicture *dist_pic, unsigned plane)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const MsSsimPlaneCuda *pl = &s->planes[plane];
    int err = ms_ssim_stage_inputs(fex, s, ref_pic, dist_pic, plane);
    if (!err)
        err = ms_ssim_submit_pyramid(s, cu_f, pl);
    for (int i = 0; i < MS_SSIM_SCALES && !err; i++)
        err = ms_ssim_submit_scale(s, cu_f, pl, i);
    return err;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    MsSsimStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;
    for (unsigned p = 0u; p < s->n_planes; p++) {
        const int err = ms_ssim_submit_plane(fex, s, ref_pic, dist_pic, p);
        if (err)
            return err;
    }

    /* Fence the final readback and opt the lifecycle into the
     * engine's drain batch. drain_batch_register is best-effort:
     * when no batch is open or the cap is hit, lc.drained stays
     * false and collect() falls back to the legacy per-stream
     * cuStreamSynchronize via vmaf_cuda_kernel_collect_wait. */
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.finished, s->lc.str));
    (void)vmaf_cuda_drain_batch_register(&s->lc);
    return 0;
}

/* ms_ssim_scale_sums - iqa_ssim()'s `l_sum`, `c_sum` and `s_sum` of one scale.
 *
 * The CPU adds every window's l, c and s into one double each, top to bottom
 * and left to right (ssim_accumulate_default_scalar() and its SIMD forms,
 * which keep the same single running sums). A double sum is its order: the
 * three planes are in raster order and this loop is the only place the terms
 * are added. The sums are independent of one another, so one pass adds each
 * of them in index order; s widens to double as the CPU's `sv = sv_f` does.
 */
static void ms_ssim_scale_sums(const double *l_terms, const double *c_terms, const float *s_terms,
                               size_t n_windows, double sums[3])
{
    double l = 0.0;
    double c = 0.0;
    double st = 0.0;
    for (size_t j = 0u; j < n_windows; j++) {
        l += l_terms[j];
        c += c_terms[j];
        st += (double)s_terms[j];
    }
    sums[0] = l;
    sums[1] = c;
    sums[2] = st;
}

/* One plane's per-scale means, as iqa_ssim() returns them, and its score, as
 * ms_ssim.c combines them. */
static void ms_ssim_plane_scores(const MsSsimPlaneCuda *pl, MsSsimPlaneScores *out)
{
    double *l_means = out->l;
    double *c_means = out->c;
    double *s_means = out->s;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const unsigned w_final = pl->scale_w_final[i];
        const unsigned h_final = pl->scale_h_final[i];
        double sums[3] = {0.0, 0.0, 0.0};
        ms_ssim_scale_sums(pl->h_l_terms[i], pl->h_c_terms[i], pl->h_s_terms[i],
                           pl->scale_window_count[i], sums);
        const double total_l = sums[0];
        const double total_c = sums[1];
        const double total_s = sums[2];
        const double n_pixels = (double)w_final * (double)h_final;
        /* iqa_ssim() returns each mean as a float, and ms_ssim.c combines the
         * floats. */
        l_means[i] = (double)(float)(total_l / n_pixels);
        c_means[i] = (double)(float)(total_c / n_pixels);
        s_means[i] = (double)(float)(total_s / n_pixels);
    }

    double msssim = 1.0;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        msssim *= pow(fabs(l_means[i]), (double)g_alphas[i]) *
                  pow(fabs(c_means[i]), (double)g_betas[i]) *
                  pow(fabs(s_means[i]), (double)g_gammas[i]);
    }
    out->score = msssim;
}

/* float_ms_ssim.c's validate_plane_scores(): a non-finite score or mean of
 * any plane fails the frame before anything is emitted. */
static int ms_ssim_validate_plane(const MsSsimPlaneScores *sc, unsigned plane, unsigned index)
{
    if (!isfinite(sc->score)) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "float_ms_ssim_cuda: non-finite score at frame %u (feature=%s value=%g)\n", index,
                 ms_ssim_plane_names[plane], sc->score);
        return -EINVAL;
    }
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        if (isfinite(sc->l[i]) && isfinite(sc->c[i]) && isfinite(sc->s[i]))
            continue;
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "float_ms_ssim_cuda: non-finite atom at frame %u "
                 "(feature=%s scale=%d l=%g c=%g s=%g)\n",
                 index, ms_ssim_plane_names[plane], i, sc->l[i], sc->c[i], sc->s[i]);
        return -EINVAL;
    }
    return 0;
}

/* Luma with its optional per-scale means, then each chroma score, after
 * every plane's score has been prepared as float_ms_ssim.c prepares it. */
static int ms_ssim_emit_planes(const MsSsimStateCuda *s, VmafFeatureCollector *feature_collector,
                               const MsSsimPlaneScores *sc, unsigned index)
{
    for (unsigned p = 0u; p < s->n_planes; p++) {
        double prepared_score = 0.0;
        const int err = vmaf_ssim_prepare_score_named(
            ms_ssim_plane_names[p], sc[p].score, s->enable_db, s->max_db, index, &prepared_score);
        if (err)
            return err;
    }
    int err = vmaf_ms_ssim_emit_scores(
        feature_collector, s->feature_name_dict, "float_ms_ssim_cuda", "float_ms_ssim", sc[0].score,
        s->enable_db, s->max_db, sc[0].l, sc[0].c, sc[0].s, MS_SSIM_SCALES, s->enable_lcs, index);
    for (unsigned p = 1u; p < s->n_planes && !err; p++) {
        err = vmaf_ssim_emit_score_named(feature_collector, s->feature_name_dict,
                                         "float_ms_ssim_cuda", ms_ssim_plane_names[p], sc[p].score,
                                         s->enable_db, s->max_db, index);
    }
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    MsSsimStateCuda *s = fex->priv;

    /* Wait for every plane's DtoH copies to land. Fast path
     * (T-GPU-OPT-2 / ADR-0271): when the engine has already drained
     * lc.finished as part of its batched flush, this is a no-op
     * (lc.drained is true → reset and return). Otherwise falls back
     * to cuStreamSynchronize(lc.str). */
    const int wait_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (wait_err)
        return wait_err;

    MsSsimPlaneScores scores[MS_SSIM_MAX_PLANES];
    memset(scores, 0, sizeof(scores));
    for (unsigned p = 0u; p < s->n_planes; p++) {
        ms_ssim_plane_scores(&s->planes[p], &scores[p]);
        const int err = ms_ssim_validate_plane(&scores[p], p, index);
        if (err)
            return err;
    }
    return ms_ssim_emit_planes(s, feature_collector, scores, index);
}

static int ms_ssim_free_device_buffer(VmafCudaState *cu_state, VmafCudaBuffer **buffer)
{
    return vmaf_cuda_buffer_free_owned(cu_state, buffer);
}

static int ms_ssim_free_host_buffer(VmafCudaState *cu_state, void **buffer)
{
    return vmaf_cuda_buffer_host_free_owned(cu_state, buffer);
}

/* `*rc` keeps the first error of a release sequence. */
static void ms_ssim_keep_first_error(int *rc, int e)
{
    if (e && !*rc)
        *rc = e;
}

static int ms_ssim_free_plane_scale(VmafCudaState *cu_state, MsSsimPlaneCuda *pl, int i)
{
    int rc = ms_ssim_free_device_buffer(cu_state, &pl->pyramid_ref[i]);
    ms_ssim_keep_first_error(&rc, ms_ssim_free_device_buffer(cu_state, &pl->pyramid_cmp[i]));
    ms_ssim_keep_first_error(&rc, ms_ssim_free_device_buffer(cu_state, &pl->l_terms[i]));
    ms_ssim_keep_first_error(&rc, ms_ssim_free_device_buffer(cu_state, &pl->c_terms[i]));
    ms_ssim_keep_first_error(&rc, ms_ssim_free_device_buffer(cu_state, &pl->s_terms[i]));
    ms_ssim_keep_first_error(&rc, ms_ssim_free_host_buffer(cu_state, (void **)&pl->h_l_terms[i]));
    ms_ssim_keep_first_error(&rc, ms_ssim_free_host_buffer(cu_state, (void **)&pl->h_c_terms[i]));
    ms_ssim_keep_first_error(&rc, ms_ssim_free_host_buffer(cu_state, (void **)&pl->h_s_terms[i]));
    return rc;
}

/* Every buffer of every plane, MS_SSIM_MAX_PLANES and not n_planes: an init
 * that failed part-way leaves earlier planes allocated, and the frees skip
 * NULL. */
static int ms_ssim_free_planes(VmafCudaState *cu_state, MsSsimStateCuda *s)
{
    int rc = 0;
    for (unsigned p = 0u; p < MS_SSIM_MAX_PLANES; p++) {
        MsSsimPlaneCuda *pl = &s->planes[p];
        for (int i = 0; i < MS_SSIM_SCALES; i++)
            ms_ssim_keep_first_error(&rc, ms_ssim_free_plane_scale(cu_state, pl, i));
    }
    return rc;
}

static int ms_ssim_free_intermediates(VmafCudaState *cu_state, MsSsimStateCuda *s)
{
    int rc = ms_ssim_free_device_buffer(cu_state, &s->h_ref_mu);
    ms_ssim_keep_first_error(&rc, ms_ssim_free_device_buffer(cu_state, &s->h_cmp_mu));
    ms_ssim_keep_first_error(&rc, ms_ssim_free_device_buffer(cu_state, &s->h_ref_sq));
    ms_ssim_keep_first_error(&rc, ms_ssim_free_device_buffer(cu_state, &s->h_cmp_sq));
    ms_ssim_keep_first_error(&rc, ms_ssim_free_device_buffer(cu_state, &s->h_refcmp));
    return rc;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    MsSsimStateCuda *s = fex->priv;
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (ret)
        return ret;

    ms_ssim_keep_first_error(&ret, ms_ssim_free_planes(fex->cu_state, s));
    ms_ssim_keep_first_error(&ret, ms_ssim_free_intermediates(fex->cu_state, s));
    ms_ssim_keep_first_error(&ret, vmaf_dictionary_free(&s->feature_name_dict));
    ms_ssim_keep_first_error(&ret, vmaf_cuda_module_unload(fex->cu_state, &s->module));
    return ret;
}

/* All three plane features, as float_ms_ssim.c provides them: without _cb /
 * _cr here the ADR-0530 name fallback would route them to the CPU extractor. */
static const char *provided_features[] = {"float_ms_ssim", "float_ms_ssim_cb", "float_ms_ssim_cr",
                                          NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_float_ms_ssim_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
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
