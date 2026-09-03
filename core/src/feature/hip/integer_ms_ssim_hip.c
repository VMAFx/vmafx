/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  integer_ms_ssim feature extractor on the HIP backend — ninth
 *  kernel-template consumer (ADR-0285).
 *
 *  Mirrors `libvmaf/src/feature/cuda/integer_ms_ssim_cuda.c` call-
 *  graph-for-call-graph. Three-kernel, 5-level pyramid design:
 *
 *    1. ms_ssim_decimate — 9-tap 9/7 biorthogonal LPF + 2× downsample,
 *       period-2n mirror boundary. Builds pyramid levels 1..4 from
 *       the float-normalised level 0 (picture_copy output).
 *    2. ms_ssim_horiz — horizontal 11-tap separable Gaussian over
 *       ref / cmp / ref² / cmp² / ref·cmp.
 *    3. ms_ssim_vert_lcs — vertical 11-tap + per-pixel l/c/s +
 *       per-block float partial triples (l, c, s).
 *
 *  Host side normalises uint → float [0,255] via picture_copy, uploads
 *  to level 0, builds the pyramid, runs horiz + vert_lcs for all 5
 *  scales on a single stream, reads back per-scale partials, then
 *  accumulates in double and applies Wang weights in collect().
 *
 *  HIP adaptation from the CUDA twin:
 *  - Raw float* device pointers (hipMalloc) instead of VmafCudaBuffer.
 *  - `hipModuleLoadData` / `hipModuleGetFunction` /
 *    `hipModuleLaunchKernel` replace the CUDA equivalents.
 *  - Pictures arrive as CPU VmafPictures (VMAF_FEATURE_EXTRACTOR_HIP
 *    flag cleared, T7-10b posture). Luma planes are copied HtoD via
 *    `hipMemcpy2DAsync` on the private submit stream.
 *  - Per-scale pinned-host partials are allocated via
 *    `hipHostMalloc` (write-combined flag) for async DtoH.
 *
 *  Min-dim guard: 11 << 4 = 176 (matches ADR-0153).
 *  enable_lcs (ADR-0243 pattern): when set, emits the 15 extra per-scale
 *  metrics float_ms_ssim_{l,c,s}_scale{0..4}.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "picture_copy.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "integer_ms_ssim_hip.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MS_SSIM_SCALES 5
#define MS_SSIM_K 11
#define MS_SSIM_BLOCK_X 16u
#define MS_SSIM_BLOCK_Y 8u

static const float g_alphas[MS_SSIM_SCALES] = {0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.1333f};
static const float g_betas[MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};
static const float g_gammas[MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};

/* ------------------------------------------------------------------ */
/* HIP-to-errno translation                                            */
/* ------------------------------------------------------------------ */

static int ms_ssim_hip_rc(hipError_t rc)
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

typedef struct MsSsimStateHip {
    VmafHipKernelLifecycle lc;
    VmafHipContext *ctx;

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

    double c1;
    double c2;
    double c3;

    /* Pyramid: 5 levels × ref + cmp, all float (hipMalloc). */
    void *pyramid_ref[MS_SSIM_SCALES];
    void *pyramid_cmp[MS_SSIM_SCALES];

    /* Staging: normalised float planes for level 0 (hipMalloc). */
    void *d_ref0;
    void *d_cmp0;

    /* Pinned host float buffers for picture_copy → H2D upload (hipHostMalloc). */
    float *h_ref;
    float *h_cmp;

    /* SSIM intermediates sized for scale 0 (hipMalloc). */
    void *d_ref_mu;
    void *d_cmp_mu;
    void *d_ref_sq;
    void *d_cmp_sq;
    void *d_refcmp;

    /* Per-scale device partials (hipMalloc, sizeof(double) per ADR-0990). */
    void *l_partials[MS_SSIM_SCALES];
    void *c_partials[MS_SSIM_SCALES];
    void *s_partials[MS_SSIM_SCALES];

    /* Per-scale pinned host partials for async DtoH (hipHostMalloc, sizeof(double)). */
    double *h_l_partials[MS_SSIM_SCALES];
    double *h_c_partials[MS_SSIM_SCALES];
    double *h_s_partials[MS_SSIM_SCALES];

    /* HIP module + three kernel handles. */
    hipModule_t module;
    hipFunction_t func_decimate;
    hipFunction_t func_horiz;
    hipFunction_t func_vert_lcs;

    unsigned index;
    VmafDictionary *feature_name_dict;

    bool enable_lcs;
    bool enable_db;     /* return dB-domain score: -10*log10(1 - ms_ssim) */
    bool clip_db;       /* clip linear ms_ssim to [0, 1] before dB conversion */
    bool enable_chroma; /* accepted but clamps n_planes to 1; luma-only. */
    unsigned n_planes;
} MsSsimStateHip;

static const VmafOption options[] = {
    {
        .name = "enable_lcs",
        .help = "enable luminance, contrast and structure intermediate output",
        .offset = offsetof(MsSsimStateHip, enable_lcs),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_db",
        .help = "return dB-domain MS-SSIM score: -10*log10(1 - ms_ssim)",
        .offset = offsetof(MsSsimStateHip, enable_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "clip_db",
        .help = "clip linear ms_ssim to [0, 1] before dB conversion",
        .offset = offsetof(MsSsimStateHip, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_chroma",
        .help = "enable MS-SSIM calculation for chroma channels (Cb and Cr); "
                "currently MS-SSIM is luma-only, so this flag is accepted but "
                "always clamps n_planes to 1 until a chroma extension lands",
        .offset = offsetof(MsSsimStateHip, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Dimension helpers (extracted to keep init under 60 lines)          */
/* ------------------------------------------------------------------ */

static int ms_ssim_hip_validate(unsigned w, unsigned h)
{
    const unsigned min_dim = (unsigned)MS_SSIM_K << (MS_SSIM_SCALES - 1);
    if (w < min_dim || h < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ms_ssim_hip: input %ux%u too small; %d-level %d-tap MS-SSIM"
                 " pyramid needs >= %ux%u (ADR-0153)\n",
                 w, h, MS_SSIM_SCALES, MS_SSIM_K, min_dim, min_dim);
        return -EINVAL;
    }
    return 0;
}

static void ms_ssim_hip_init_dims(MsSsimStateHip *s, unsigned w, unsigned h, unsigned bpc)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;

    s->scale_w[0] = w;
    s->scale_h[0] = h;
    for (int i = 1; i < MS_SSIM_SCALES; i++) {
        s->scale_w[i] = (s->scale_w[i - 1] / 2) + (s->scale_w[i - 1] & 1u);
        s->scale_h[i] = (s->scale_h[i - 1] / 2) + (s->scale_h[i - 1] & 1u);
    }
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        s->scale_w_horiz[i] = s->scale_w[i] - (MS_SSIM_K - 1u);
        s->scale_h_horiz[i] = s->scale_h[i];
        s->scale_w_final[i] = s->scale_w[i] - (MS_SSIM_K - 1u);
        s->scale_h_final[i] = s->scale_h[i] - (MS_SSIM_K - 1u);
        s->scale_grid_x[i] = (s->scale_w_final[i] + MS_SSIM_BLOCK_X - 1u) / MS_SSIM_BLOCK_X;
        s->scale_grid_y[i] = (s->scale_h_final[i] + MS_SSIM_BLOCK_Y - 1u) / MS_SSIM_BLOCK_Y;
        s->scale_block_count[i] = s->scale_grid_x[i] * s->scale_grid_y[i];
    }

    const double L = 255.0;
    const double K1 = 0.01;
    const double K2 = 0.03;
    s->c1 = (K1 * L) * (K1 * L);
    s->c2 = (K2 * L) * (K2 * L);
    s->c3 = s->c2 * 0.5;
}

/* ------------------------------------------------------------------ */
/* HAVE_HIPCC helpers                                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC

/* Load the HSACO and resolve the three kernel function handles. */
static int ms_ssim_hip_module_load(MsSsimStateHip *s)
{
    hipError_t hip_rc = hipModuleLoadData(&s->module, ms_ssim_score_hsaco);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);

    hip_rc = hipModuleGetFunction(&s->func_decimate, s->module, "ms_ssim_decimate");
    if (hip_rc != hipSuccess)
        goto fail;
    hip_rc = hipModuleGetFunction(&s->func_horiz, s->module, "ms_ssim_horiz");
    if (hip_rc != hipSuccess)
        goto fail;
    hip_rc = hipModuleGetFunction(&s->func_vert_lcs, s->module, "ms_ssim_vert_lcs");
    if (hip_rc != hipSuccess)
        goto fail;
    return 0;

fail:
    (void)hipModuleUnload(s->module);
    s->module = NULL;
    return ms_ssim_hip_rc(hip_rc);
}

/* Allocate all device buffers. Returns 0 or negative errno.
 * On failure, already-allocated buffers are freed and NULL-ed. */
static int ms_ssim_hip_bufs_alloc(MsSsimStateHip *s)
{
    const size_t level0_bytes = (size_t)s->scale_w[0] * s->scale_h[0] * sizeof(float);
    const size_t horiz_max = (size_t)s->scale_w_horiz[0] * s->scale_h_horiz[0] * sizeof(float);

    hipError_t hip_rc;
    /* Level 0 float staging buffers (device). */
    hip_rc = hipMalloc(&s->d_ref0, level0_bytes);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);
    hip_rc = hipMalloc(&s->d_cmp0, level0_bytes);
    if (hip_rc != hipSuccess)
        goto fail_cmp0;

    /* Pinned host float buffers for picture_copy → H2D upload. */
    hip_rc = hipHostMalloc((void **)&s->h_ref, level0_bytes, hipHostMallocDefault);
    if (hip_rc != hipSuccess)
        goto fail_h_ref;
    hip_rc = hipHostMalloc((void **)&s->h_cmp, level0_bytes, hipHostMallocDefault);
    if (hip_rc != hipSuccess)
        goto fail_h_cmp;

    /* Pyramid levels 0..4 for ref and cmp. */
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const size_t lvl = (size_t)s->scale_w[i] * s->scale_h[i] * sizeof(float);
        hip_rc = hipMalloc(&s->pyramid_ref[i], lvl);
        if (hip_rc != hipSuccess)
            goto fail_pyramid;
        hip_rc = hipMalloc(&s->pyramid_cmp[i], lvl);
        if (hip_rc != hipSuccess) {
            (void)hipFree(s->pyramid_ref[i]);
            s->pyramid_ref[i] = NULL;
            goto fail_pyramid;
        }
    }

    /* SSIM intermediate float buffers (sized for scale 0, reused per scale). */
    hip_rc = hipMalloc(&s->d_ref_mu, horiz_max);
    if (hip_rc != hipSuccess)
        goto fail_intermed;
    hip_rc = hipMalloc(&s->d_cmp_mu, horiz_max);
    if (hip_rc != hipSuccess)
        goto fail_cmp_mu;
    hip_rc = hipMalloc(&s->d_ref_sq, horiz_max);
    if (hip_rc != hipSuccess)
        goto fail_ref_sq;
    hip_rc = hipMalloc(&s->d_cmp_sq, horiz_max);
    if (hip_rc != hipSuccess)
        goto fail_cmp_sq;
    hip_rc = hipMalloc(&s->d_refcmp, horiz_max);
    if (hip_rc != hipSuccess)
        goto fail_refcmp;

    /* Per-scale device partials (sizeof(double) per ADR-0990). */
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const size_t pb = (size_t)s->scale_block_count[i] * sizeof(double);
        hip_rc = hipMalloc(&s->l_partials[i], pb);
        if (hip_rc != hipSuccess)
            goto fail_partials;
        hip_rc = hipMalloc(&s->c_partials[i], pb);
        if (hip_rc != hipSuccess) {
            (void)hipFree(s->l_partials[i]);
            s->l_partials[i] = NULL;
            goto fail_partials;
        }
        hip_rc = hipMalloc(&s->s_partials[i], pb);
        if (hip_rc != hipSuccess) {
            (void)hipFree(s->c_partials[i]);
            s->c_partials[i] = NULL;
            (void)hipFree(s->l_partials[i]);
            s->l_partials[i] = NULL;
            goto fail_partials;
        }
    }

    /* Pinned host partials for async DtoH (write-combined, sizeof(double)). */
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        const size_t pb = (size_t)s->scale_block_count[i] * sizeof(double);
        hip_rc = hipHostMalloc((void **)&s->h_l_partials[i], pb, hipHostMallocWriteCombined);
        if (hip_rc != hipSuccess)
            goto fail_pinned;
        hip_rc = hipHostMalloc((void **)&s->h_c_partials[i], pb, hipHostMallocWriteCombined);
        if (hip_rc != hipSuccess) {
            (void)hipHostFree(s->h_l_partials[i]);
            s->h_l_partials[i] = NULL;
            goto fail_pinned;
        }
        hip_rc = hipHostMalloc((void **)&s->h_s_partials[i], pb, hipHostMallocWriteCombined);
        if (hip_rc != hipSuccess) {
            (void)hipHostFree(s->h_c_partials[i]);
            s->h_c_partials[i] = NULL;
            (void)hipHostFree(s->h_l_partials[i]);
            s->h_l_partials[i] = NULL;
            goto fail_pinned;
        }
    }
    return 0;

    /* Unwind in reverse. Labels mark the first that failed. */
fail_pinned:
    for (int i = MS_SSIM_SCALES - 1; i >= 0; i--) {
        if (s->h_s_partials[i]) {
            (void)hipHostFree(s->h_s_partials[i]);
            s->h_s_partials[i] = NULL;
        }
        if (s->h_c_partials[i]) {
            (void)hipHostFree(s->h_c_partials[i]);
            s->h_c_partials[i] = NULL;
        }
        if (s->h_l_partials[i]) {
            (void)hipHostFree(s->h_l_partials[i]);
            s->h_l_partials[i] = NULL;
        }
    }
fail_partials:
    for (int i = MS_SSIM_SCALES - 1; i >= 0; i--) {
        if (s->s_partials[i]) {
            (void)hipFree(s->s_partials[i]);
            s->s_partials[i] = NULL;
        }
        if (s->c_partials[i]) {
            (void)hipFree(s->c_partials[i]);
            s->c_partials[i] = NULL;
        }
        if (s->l_partials[i]) {
            (void)hipFree(s->l_partials[i]);
            s->l_partials[i] = NULL;
        }
    }
    (void)hipFree(s->d_refcmp);
    s->d_refcmp = NULL;
fail_refcmp:
    (void)hipFree(s->d_cmp_sq);
    s->d_cmp_sq = NULL;
fail_cmp_sq:
    (void)hipFree(s->d_ref_sq);
    s->d_ref_sq = NULL;
fail_ref_sq:
    (void)hipFree(s->d_cmp_mu);
    s->d_cmp_mu = NULL;
fail_cmp_mu:
    (void)hipFree(s->d_ref_mu);
    s->d_ref_mu = NULL;
fail_intermed:
fail_pyramid:
    for (int i = MS_SSIM_SCALES - 1; i >= 0; i--) {
        if (s->pyramid_cmp[i]) {
            (void)hipFree(s->pyramid_cmp[i]);
            s->pyramid_cmp[i] = NULL;
        }
        if (s->pyramid_ref[i]) {
            (void)hipFree(s->pyramid_ref[i]);
            s->pyramid_ref[i] = NULL;
        }
    }
    /* Falls through to free h_cmp, h_ref, d_cmp0, d_ref0. */
    if (s->h_cmp) {
        (void)hipHostFree(s->h_cmp);
        s->h_cmp = NULL;
    }
fail_h_cmp:
    if (s->h_ref) {
        (void)hipHostFree(s->h_ref);
        s->h_ref = NULL;
    }
fail_h_ref:
    (void)hipFree(s->d_cmp0);
    s->d_cmp0 = NULL;
fail_cmp0:
    (void)hipFree(s->d_ref0);
    s->d_ref0 = NULL;
    return ms_ssim_hip_rc(hip_rc);
}

/* Free all device and pinned-host buffers. Safe with NULL pointers. */
static void ms_ssim_hip_bufs_free(MsSsimStateHip *s)
{
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        if (s->h_s_partials[i]) {
            (void)hipHostFree(s->h_s_partials[i]);
            s->h_s_partials[i] = NULL;
        }
        if (s->h_c_partials[i]) {
            (void)hipHostFree(s->h_c_partials[i]);
            s->h_c_partials[i] = NULL;
        }
        if (s->h_l_partials[i]) {
            (void)hipHostFree(s->h_l_partials[i]);
            s->h_l_partials[i] = NULL;
        }
        if (s->s_partials[i]) {
            (void)hipFree(s->s_partials[i]);
            s->s_partials[i] = NULL;
        }
        if (s->c_partials[i]) {
            (void)hipFree(s->c_partials[i]);
            s->c_partials[i] = NULL;
        }
        if (s->l_partials[i]) {
            (void)hipFree(s->l_partials[i]);
            s->l_partials[i] = NULL;
        }
        if (s->pyramid_cmp[i]) {
            (void)hipFree(s->pyramid_cmp[i]);
            s->pyramid_cmp[i] = NULL;
        }
        if (s->pyramid_ref[i]) {
            (void)hipFree(s->pyramid_ref[i]);
            s->pyramid_ref[i] = NULL;
        }
    }
    if (s->d_refcmp) {
        (void)hipFree(s->d_refcmp);
        s->d_refcmp = NULL;
    }
    if (s->d_cmp_sq) {
        (void)hipFree(s->d_cmp_sq);
        s->d_cmp_sq = NULL;
    }
    if (s->d_ref_sq) {
        (void)hipFree(s->d_ref_sq);
        s->d_ref_sq = NULL;
    }
    if (s->d_cmp_mu) {
        (void)hipFree(s->d_cmp_mu);
        s->d_cmp_mu = NULL;
    }
    if (s->d_ref_mu) {
        (void)hipFree(s->d_ref_mu);
        s->d_ref_mu = NULL;
    }
    if (s->h_cmp) {
        (void)hipHostFree(s->h_cmp);
        s->h_cmp = NULL;
    }
    if (s->h_ref) {
        (void)hipHostFree(s->h_ref);
        s->h_ref = NULL;
    }
    if (s->d_cmp0) {
        (void)hipFree(s->d_cmp0);
        s->d_cmp0 = NULL;
    }
    if (s->d_ref0) {
        (void)hipFree(s->d_ref0);
        s->d_ref0 = NULL;
    }
}

/* Normalise a uint VmafPicture luma plane into a pinned float host buffer,
 * then upload asynchronously to the device.
 *
 * Mirrors the CUDA path in integer_ms_ssim_cuda.c: picture_copy() converts
 * uint samples → float [0, 255], then hipMemcpyAsync uploads the contiguous
 * float buffer.  Commit 681ab99451 originally called hipMemcpy2DAsync with
 * dpitch = width * bpc_bytes directly into the float device buffer — that
 * wrote only width*height raw uint bytes into a width*height*sizeof(float)
 * allocation and left the remaining 3/4 uninitialized, producing garbage in
 * the decimate + horiz kernels. */
static int ms_ssim_hip_upload_plane(MsSsimStateHip *s, hipStream_t str, const VmafPicture *pic,
                                    float *h_staging, void *d_dst)
{
    /* picture_copy: uint → float [0, 255] into the pinned host staging buffer.
     * Stride is width * sizeof(float) (contiguous, no padding). */
    picture_copy(h_staging, (ptrdiff_t)((size_t)s->width * sizeof(float)), (VmafPicture *)pic, 0,
                 s->bpc, 0);

    /* Async H2D upload of the normalised float plane. */
    const size_t float_bytes = (size_t)s->width * s->height * sizeof(float);
    hipError_t hip_rc = hipMemcpyAsync(d_dst, h_staging, float_bytes, hipMemcpyHostToDevice, str);
    return ms_ssim_hip_rc(hip_rc);
}

/* Launch the decimate kernel for one level transition on one side. */
static int ms_ssim_hip_launch_decimate(MsSsimStateHip *s, hipStream_t str, void *src, void *dst,
                                       unsigned w_in, unsigned h_in, unsigned w_out, unsigned h_out)
{
    const unsigned gx = (w_out + MS_SSIM_BLOCK_X - 1u) / MS_SSIM_BLOCK_X;
    const unsigned gy = (h_out + MS_SSIM_BLOCK_Y - 1u) / MS_SSIM_BLOCK_Y;
    void *args[] = {&src, &dst, &w_in, &h_in, &w_out, &h_out};
    return ms_ssim_hip_rc(hipModuleLaunchKernel(s->func_decimate, gx, gy, 1u, MS_SSIM_BLOCK_X,
                                                MS_SSIM_BLOCK_Y, 1u, 0, str, args, NULL));
}

/* Launch horiz + vert_lcs + DtoH for one scale on str. */
static int ms_ssim_hip_launch_scale(MsSsimStateHip *s, hipStream_t str, int i)
{
    unsigned width = s->scale_w[i];
    unsigned w_horiz = s->scale_w_horiz[i];
    unsigned h_horiz = s->scale_h_horiz[i];
    unsigned w_final = s->scale_w_final[i];
    unsigned h_final = s->scale_h_final[i];
    const unsigned hgx = (w_horiz + MS_SSIM_BLOCK_X - 1u) / MS_SSIM_BLOCK_X;
    const unsigned hgy = (h_horiz + MS_SSIM_BLOCK_Y - 1u) / MS_SSIM_BLOCK_Y;

    void *horiz_args[] = {
        &s->pyramid_ref[i], &s->pyramid_cmp[i], &s->d_ref_mu, &s->d_cmp_mu, &s->d_ref_sq,
        &s->d_cmp_sq,       &s->d_refcmp,       &width,       &w_horiz,     &h_horiz,
    };
    hipError_t hip_rc = hipModuleLaunchKernel(s->func_horiz, hgx, hgy, 1u, MS_SSIM_BLOCK_X,
                                              MS_SSIM_BLOCK_Y, 1u, 0, str, horiz_args, NULL);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);

    void *vert_args[] = {
        &s->d_ref_mu,
        &s->d_cmp_mu,
        &s->d_ref_sq,
        &s->d_cmp_sq,
        &s->d_refcmp,
        &s->l_partials[i],
        &s->c_partials[i],
        &s->s_partials[i],
        &w_horiz,
        &w_final,
        &h_final,
        &s->c1,
        &s->c2,
        &s->c3,
    };
    hip_rc = hipModuleLaunchKernel(s->func_vert_lcs, s->scale_grid_x[i], s->scale_grid_y[i], 1u,
                                   MS_SSIM_BLOCK_X, MS_SSIM_BLOCK_Y, 1u, 0, str, vert_args, NULL);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);

    const size_t pb = (size_t)s->scale_block_count[i] * sizeof(double);
    hip_rc = hipMemcpyAsync(s->h_l_partials[i], s->l_partials[i], pb, hipMemcpyDeviceToHost, str);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);
    hip_rc = hipMemcpyAsync(s->h_c_partials[i], s->c_partials[i], pb, hipMemcpyDeviceToHost, str);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);
    hip_rc = hipMemcpyAsync(s->h_s_partials[i], s->s_partials[i], pb, hipMemcpyDeviceToHost, str);
    return ms_ssim_hip_rc(hip_rc);
}

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* init / close                                                        */
/* ------------------------------------------------------------------ */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    MsSsimStateHip *s = fex->priv;

    if (pix_fmt == VMAF_PIX_FMT_YUV400P || !s->enable_chroma)
        s->n_planes = 1u;
    else
        s->n_planes = 1u; /* reserved: MS-SSIM chroma extension not yet impl. */

    int err = ms_ssim_hip_validate(w, h);
    if (err != 0)
        return err;

    ms_ssim_hip_init_dims(s, w, h, bpc);

    err = vmaf_hip_context_new(&s->ctx, 0);
    if (err != 0)
        return err;

    err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0)
        goto fail_after_ctx;

#ifdef HAVE_HIPCC
    err = ms_ssim_hip_module_load(s);
    if (err != 0)
        goto fail_after_lc;

    err = ms_ssim_hip_bufs_alloc(s);
    if (err != 0)
        goto fail_after_module;
#else
    err = -ENOSYS;
    if (err != 0)
        goto fail_after_lc;
#endif /* HAVE_HIPCC */

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        err = -ENOMEM;
#ifdef HAVE_HIPCC
        ms_ssim_hip_bufs_free(s);
        goto fail_after_module;
#else
        goto fail_after_lc;
#endif
    }
    return 0;

#ifdef HAVE_HIPCC
fail_after_module:
    (void)hipModuleUnload(s->module);
    s->module = NULL;
#endif
fail_after_lc:
    (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
fail_after_ctx:
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    MsSsimStateHip *s = fex->priv;
    int rc = 0;

#ifdef HAVE_HIPCC
    ms_ssim_hip_bufs_free(s);
    if (s->module != NULL) {
        int e = ms_ssim_hip_rc(hipModuleUnload(s->module));
        s->module = NULL;
        if (rc == 0)
            rc = e;
    }
#endif /* HAVE_HIPCC */

    int e = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
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
    MsSsimStateHip *s = fex->priv;
    s->index = index;
    const hipStream_t str = (hipStream_t)s->lc.str;

    /* Normalise + upload both luma planes to float device buffers.
     * picture_copy() converts uint → float [0,255] into the pinned
     * host staging buffers (h_ref / h_cmp), then hipMemcpyAsync
     * uploads them to d_ref0 / d_cmp0.  Pictures arrive as CPU
     * VmafPictures (VMAF_FEATURE_EXTRACTOR_HIP flag cleared, T7-10b
     * posture). */
    int err = ms_ssim_hip_upload_plane(s, str, ref_pic, s->h_ref, s->d_ref0);
    if (err != 0)
        return err;
    err = ms_ssim_hip_upload_plane(s, str, dist_pic, s->h_cmp, s->d_cmp0);
    if (err != 0)
        return err;

    /* Copy the normalised float level-0 planes into the pyramid. */
    const size_t l0_bytes = (size_t)s->scale_w[0] * s->scale_h[0] * sizeof(float);
    hipError_t hip_rc =
        hipMemcpyAsync(s->pyramid_ref[0], s->d_ref0, l0_bytes, hipMemcpyDeviceToDevice, str);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);
    hip_rc = hipMemcpyAsync(s->pyramid_cmp[0], s->d_cmp0, l0_bytes, hipMemcpyDeviceToDevice, str);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);

    /* Build pyramid levels 1..4 via the decimate kernel. */
    for (int i = 0; i < MS_SSIM_SCALES - 1; i++) {
        err = ms_ssim_hip_launch_decimate(s, str, s->pyramid_ref[i], s->pyramid_ref[i + 1],
                                          s->scale_w[i], s->scale_h[i], s->scale_w[i + 1],
                                          s->scale_h[i + 1]);
        if (err != 0)
            return err;
        err = ms_ssim_hip_launch_decimate(s, str, s->pyramid_cmp[i], s->pyramid_cmp[i + 1],
                                          s->scale_w[i], s->scale_h[i], s->scale_w[i + 1],
                                          s->scale_h[i + 1]);
        if (err != 0)
            return err;
    }

    /* Per-scale horiz + vert_lcs + DtoH. All enqueued on the same stream. */
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        err = ms_ssim_hip_launch_scale(s, str, i);
        if (err != 0)
            return err;
    }

    /* Record the submit event and register with the lifecycle. */
    hip_rc = hipEventRecord((hipEvent_t)s->lc.submit, str);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
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
    MsSsimStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

    /* Accumulate per-block partials in double per scale, then apply
     * the Wang weights for the final product combine. */
    double l_means[MS_SSIM_SCALES] = {0};
    double c_means[MS_SSIM_SCALES] = {0};
    double s_means[MS_SSIM_SCALES] = {0};

    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        double total_l = 0.0, total_c = 0.0, total_s = 0.0;
        for (unsigned j = 0; j < s->scale_block_count[i]; j++) {
            total_l += s->h_l_partials[i][j];
            total_c += s->h_c_partials[i][j];
            total_s += s->h_s_partials[i][j];
        }
        const double n_pix = (double)s->scale_w_final[i] * (double)s->scale_h_final[i];
        l_means[i] = total_l / n_pix;
        c_means[i] = total_c / n_pix;
        s_means[i] = total_s / n_pix;
    }

    double msssim = 1.0;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        msssim *= pow(l_means[i], (double)g_alphas[i]) * pow(c_means[i], (double)g_betas[i]) *
                  pow(fabs(s_means[i]), (double)g_gammas[i]);
    }

    /* dB conversion — mirrors CPU float_ms_ssim.c exactly. */
    double score = msssim;
    if (s->enable_db) {
        if (s->clip_db) {
            score = score < 0.0 ? 0.0 : (score > 1.0 ? 1.0 : score);
        }
        score = -10.0 * log10(1.0 - score);
    }

    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "float_ms_ssim", score, index);
    if (s->enable_lcs) {
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
        for (int i = 0; i < MS_SSIM_SCALES; i++) {
            err |= vmaf_feature_collector_append(feature_collector, l_names[i], l_means[i], index);
            err |= vmaf_feature_collector_append(feature_collector, c_names[i], c_means[i], index);
            err |= vmaf_feature_collector_append(feature_collector, s_names[i], s_means[i], index);
        }
    }
    return err;
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"float_ms_ssim", NULL};

/* Load-bearing: the feature extractor is registered via
 * `extern VmafFeatureExtractor vmaf_fex_integer_ms_ssim_hip;` in
 * `libvmaf/src/feature/feature_extractor.c`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern every CUDA / HIP feature extractor uses (see e.g.
 * `vmaf_fex_float_ssim_hip` in float_ssim_hip.c). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_ms_ssim_hip = {
    .name = "integer_ms_ssim_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(MsSsimStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    /* 5 scales × (decimate×2 + horiz + vert_lcs) = 20 dispatches/frame. */
    .chars =
        {
            .n_dispatches_per_frame = 20,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
