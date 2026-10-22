/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  ssimulacra2 feature kernel on the CUDA backend (ADR-0206), device-resident
 *  since ADR-1391, the CUDA port of the ADR-1363 chain of ssimulacra2_sycl.
 *
 *  Per frame, one in-order chain on the picture stream with no host compute
 *  and no wait mid-frame:
 *    1. YUV -> linear RGB from the raw device planes of both pictures.
 *    2. Per scale (up to 6, stops before a side drops below 8):
 *       a. linear RGB -> XYB;
 *       b. the five separable 3-pole IIR blurs of ref, dis, ref^2, dis^2
 *          and ref*dis, one horizontal and one vertical launch
 *          (ssimulacra2_blur.cu);
 *       c. the per-pixel SSIM and edge-difference terms in fp64, summed per
 *          channel in the CPU's order (four launches, ADR-1433);
 *       d. 2x2 downsample of the linear-RGB pyramid.
 *    3. One 864-byte readback of the per-scale sums. collect() waits once,
 *       forms the 108 norms and pools the score exactly as ssimulacra2.c
 *       does.
 *
 *  Numerical contract (ADR-1391, ADR-1433): the twin returns the CPU
 *  extractor's score bit for bit. Stages 1, 2a, 2b and 2d reproduce the CPU's
 *  planes (the device TUs build with --fmad=false and use the shared helpers
 *  of ssimulacra2_math.h). The per-pixel terms of 2c are the CPU's fp64
 *  expressions, and their sums are the sums of the CPU's loops: ssim_map()
 *  and edge_diff_map() add each term pixel after pixel into one double, and
 *  feature/ordered_sum.h reproduces that rounding from chunk-wise integer
 *  increments the device forms in parallel, adding the few chunks in which
 *  the running sum crosses a binade term by term.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "log.h"

#include "cuda/kernel_template.h"
#include "cuda/ssimulacra2_cuda.h"
#include "cuda_helper.cuh"
#include "feature/ssimulacra2_pixel_format.h"
#include "feature/ssimulacra2_gaussian.h"
#include "feature/ssimulacra2_score.h"
#include "feature/ssimulacra2_weights.h"
#include "picture.h"
#include "picture_cuda.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define SS2C_XYB_BLOCK 256
#define SS2C_SIGMA 1.5
#define SS2C_PI 3.14159265358979323846
#define SS2C_TOTALS_PER_SCALE ((size_t)SS2C_CHANNELS * SS2C_SUMS)

enum yuv_matrix_c {
    SS2C_MATRIX_BT709_LIMITED = 0,
    SS2C_MATRIX_BT601_LIMITED = 1,
    SS2C_MATRIX_BT709_FULL = 2,
    SS2C_MATRIX_BT601_FULL = 3,
};

typedef struct Ssimu2StateCuda {
    /* Option. */
    int yuv_matrix;

    /* Geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned plane_w[SS2C_CHANNELS];
    unsigned plane_h[SS2C_CHANNELS];
    unsigned scale_w[SS2C_NUM_SCALES];
    unsigned scale_h[SS2C_NUM_SCALES];
    int num_scales;

    /* Recursive Gaussian coefficients (sigma = 1.5). */
    float rg_n2[3];
    float rg_d1[3];
    int rg_radius;

    Ss2cYuvCoefficients yuv;

    /* CUDA lifecycle (ADR-0246). */
    VmafCudaKernelLifecycle lc;
    CUmodule module_blur;
    CUmodule module_device;
    CUfunction func_blur_h;
    CUfunction func_blur_v;
    CUfunction func_yuv;
    CUfunction func_xyb;
    CUfunction func_chunk_sums;
    CUfunction func_chunk_plan;
    CUfunction func_chunk_units;
    CUfunction func_totals;
    CUfunction func_down;

    /* Device buffers; every three-plane buffer holds compact planes of the
     * current scale. The linear-RGB pyramid ping-pongs per image: [img][0]
     * holds scales 0, 2 and 4 (full size), [img][1] scales 1, 3 and 5. */
    VmafCudaBuffer *d_lin[SS2C_IMAGES][2];
    VmafCudaBuffer *d_xyb[SS2C_IMAGES];
    VmafCudaBuffer *d_pass[SS2C_BLUR_JOBS];    /* horizontal-pass output */
    VmafCudaBuffer *d_blurred[SS2C_BLUR_JOBS]; /* mu1, mu2, s11, s22, s12 */
    VmafCudaBuffer *d_chunk_sums;              /* [channel][chunk][sum] */
    VmafCudaBuffer *d_plan;                    /* [channel][sum][chunk] */
    VmafCudaBuffer *d_units;                   /* [channel][sum][chunk][2] */
    VmafCudaBuffer *d_totals;                  /* [scale][channel][sum] */
    double *h_totals;                          /* pinned readback of d_totals */
} Ssimu2StateCuda;

static const VmafOption options[] = {
    {
        .name = "yuv_matrix",
        .help = "YUV→RGB matrix: 0=bt709_limited (default), 1=bt601_limited, "
                "2=bt709_full, 3=bt601_full",
        .offset = offsetof(Ssimu2StateCuda, yuv_matrix),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = SS2C_MATRIX_BT709_LIMITED,
        .min = 0,
        .max = 3,
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Recursive Gaussian coefficient setup — bit-identical port of
 * create_recursive_gaussian in ssimulacra2.c.                        */
/* ------------------------------------------------------------------ */

/* Setup recursive Gaussian coefficients via shared ssimu2_compute_gaussian_coeffs(). */
static void ss2c_setup_gaussian(Ssimu2StateCuda *s, double sigma)
{
    Ssimu2GaussianCoeffs coeffs;
    ssimu2_compute_gaussian_coeffs(sigma, &coeffs);
    s->rg_radius = coeffs.radius;
    for (int i = 0; i < 3; i++) {
        s->rg_n2[i] = coeffs.n2[i];
        s->rg_d1[i] = coeffs.d1[i];
    }
}

/* YUV -> RGB constants with the float expressions of
 * ssimulacra2.c::picture_to_linear_rgb (BT.601 limited is its default
 * branch). */
static void ss2c_yuv_coeffs(int yuv_matrix, unsigned bpc, Ss2cYuvCoefficients *c)
{
    float kr = 0.299f;
    float kg = 0.587f;
    float kb = 0.114f;
    if (yuv_matrix == SS2C_MATRIX_BT709_LIMITED || yuv_matrix == SS2C_MATRIX_BT709_FULL) {
        kr = 0.2126f;
        kg = 0.7152f;
        kb = 0.0722f;
    }
    const bool limited =
        yuv_matrix != SS2C_MATRIX_BT709_FULL && yuv_matrix != SS2C_MATRIX_BT601_FULL;
    const float peak = (float)((1u << bpc) - 1u);
    c->inv_peak = 1.0f / peak;
    c->cr_r = 2.0f * (1.0f - kr);
    c->cb_b = 2.0f * (1.0f - kb);
    c->cb_g = -(2.0f * kb * (1.0f - kb)) / kg;
    c->cr_g = -(2.0f * kr * (1.0f - kr)) / kg;
    c->y_scale = limited ? (255.0f / 219.0f) : 1.0f;
    c->c_scale = limited ? (255.0f / 224.0f) : 1.0f;
    c->y_off = limited ? (16.0f / 255.0f) : 0.0f;
    c->c_off = 0.5f;
}

/* Plane sizes of the input pictures (picture.c: ceiling division). init()
 * has refused 4:0:0 (vmaf_ss2_check_pixel_format()); check_context_cuda sends
 * it to the CPU extractor. */
static void ss2c_configure_planes(Ssimu2StateCuda *s, enum VmafPixelFormat pix_fmt)
{
    const unsigned ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1u : 0u;
    const unsigned ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1u : 0u;
    for (unsigned p = 0; p < SS2C_CHANNELS; p++) {
        const unsigned sh = (p == 0u) ? 0u : ss_hor;
        const unsigned sv = (p == 0u) ? 0u : ss_ver;
        s->plane_w[p] = (s->width + sh) >> sh;
        s->plane_h[p] = (s->height + sv) >> sv;
    }
}

static void ss2c_configure_scales(Ssimu2StateCuda *s)
{
    s->scale_w[0] = s->width;
    s->scale_h[0] = s->height;
    for (int i = 1; i < SS2C_NUM_SCALES; i++) {
        s->scale_w[i] = (s->scale_w[i - 1] + 1u) / 2u;
        s->scale_h[i] = (s->scale_h[i - 1] + 1u) / 2u;
    }
    /* ssimulacra2.c::extract stops before the first scale below 8x8. */
    s->num_scales = 0;
    while (s->num_scales < SS2C_NUM_SCALES && s->scale_w[s->num_scales] >= 8u &&
           s->scale_h[s->num_scales] >= 8u)
        s->num_scales++;
}

/* Chunks of one plane of `pixels` pixels; the last may be partial. */
static unsigned ss2c_chunks(size_t pixels)
{
    const size_t chunk_pixels = (size_t)SS2C_CHUNK_PIXELS;
    return (unsigned)((pixels + chunk_pixels - 1u) / chunk_pixels);
}

/* The six sums of each channel to the norms of ssim_map / edge_diff_map. */
static void ss2c_scale_norms(const double *totals, unsigned cw, unsigned ch, double avg_ssim[6],
                             double avg_ed[12])
{
    const double one_per_pixels = 1.0 / (double)((size_t)cw * (size_t)ch);
    for (unsigned c = 0; c < SS2C_CHANNELS; c++) {
        const double *sum = totals + (size_t)c * SS2C_SUMS;
        avg_ssim[c * 2u + 0u] = one_per_pixels * sum[0];
        avg_ssim[c * 2u + 1u] = sqrt(sqrt(one_per_pixels * sum[1]));
        avg_ed[c * 4u + 0u] = one_per_pixels * sum[2];
        avg_ed[c * 4u + 1u] = sqrt(sqrt(one_per_pixels * sum[3]));
        avg_ed[c * 4u + 2u] = one_per_pixels * sum[4];
        avg_ed[c * 4u + 3u] = sqrt(sqrt(one_per_pixels * sum[5]));
    }
}

/* ssimulacra2.c::pool_score. */
static double ss2c_pool_score(const double avg_ssim[6][6], const double avg_ed[6][12],
                              int num_scales)
{
    double ssim = 0.0;
    size_t i = 0;
    for (int c = 0; c < 3; c++) {
        for (int scale = 0; scale < 6; scale++) {
            for (int n = 0; n < 2; n++) {
                const double s_term = scale < num_scales ? avg_ssim[scale][c * 2 + n] : 0.0;
                const double r_term = scale < num_scales ? avg_ed[scale][c * 4 + n] : 0.0;
                const double b_term = scale < num_scales ? avg_ed[scale][c * 4 + n + 2] : 0.0;
                ssim += kWeights[i++] * fabs(s_term);
                ssim += kWeights[i++] * fabs(r_term);
                ssim += kWeights[i++] * fabs(b_term);
            }
        }
    }
    ssim = ssim * 0.9562382616834844;
    ssim = 2.326765642916932 * ssim - 0.020884521182843837 * ssim * ssim +
           6.248496625763138e-05 * ssim * ssim * ssim;
    return vmaf_ss2_finalize_score(ssim);
}

/* ------------------------------------------------------------------ */
/* Kernel launches: everything goes to the picture stream in order.   */
/* ------------------------------------------------------------------ */

/* Typed device pointer of a buffer for a kernel-argument struct. The Driver
 * API hands out allocations as `CUdeviceptr` integers while the argument
 * structs of ssimulacra2_cuda.h hold typed device pointers, so the
 * integer-to-pointer conversion is inherent to dispatching through the Driver
 * API, which the fork keeps (ADR-0747). The pointer is never dereferenced on
 * the host. */
static void *ss2c_ptr(const VmafCudaBuffer *buf)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr): Driver API device address (ADR-0747)
    return (void *)(uintptr_t)buf->data;
}

static int ss2c_launch_yuv(const Ssimu2StateCuda *s, CudaFunctions *cu_f, const VmafPicture *ref,
                           const VmafPicture *dist, CUstream stream)
{
    Ss2cYuvArgs a;
    memset(&a, 0, sizeof(a));
    const VmafPicture *pics[SS2C_IMAGES] = {ref, dist};
    for (unsigned img = 0; img < SS2C_IMAGES; img++) {
        for (unsigned p = 0; p < SS2C_CHANNELS; p++) {
            a.plane[img][p] = pics[img]->data[p];
            a.pitch[img][p] = (size_t)pics[img]->stride[p];
        }
        a.out[img] = (float *)ss2c_ptr(s->d_lin[img][0]);
    }
    for (unsigned p = 0; p < SS2C_CHANNELS; p++) {
        a.plane_w[p] = s->plane_w[p];
        a.plane_h[p] = s->plane_h[p];
    }
    a.width = s->width;
    a.height = s->height;
    a.wide = (s->bpc > 8u) ? 1u : 0u;
    a.k = s->yuv;
    void *args[] = {&a};
    const unsigned gx = (s->width + SS2C_PIX_BX - 1u) / SS2C_PIX_BX;
    const unsigned gy = (s->height + SS2C_PIX_BY - 1u) / SS2C_PIX_BY;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_yuv, gx, gy, SS2C_IMAGES, SS2C_PIX_BX,
                                           SS2C_PIX_BY, 1, 0, stream, args, NULL));
    return 0;
}

static int ss2c_launch_xyb(const Ssimu2StateCuda *s, CudaFunctions *cu_f, int scale,
                           CUstream stream)
{
    const unsigned level = (unsigned)scale & 1u;
    CUdeviceptr lin_ref = s->d_lin[0][level]->data;
    CUdeviceptr lin_dis = s->d_lin[1][level]->data;
    CUdeviceptr xyb_ref = s->d_xyb[0]->data;
    CUdeviceptr xyb_dis = s->d_xyb[1]->data;
    unsigned pixels = s->scale_w[scale] * s->scale_h[scale];
    void *args[] = {&lin_ref, &lin_dis, &xyb_ref, &xyb_dis, &pixels};
    const unsigned gx = (pixels + SS2C_XYB_BLOCK - 1u) / SS2C_XYB_BLOCK;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_xyb, gx, SS2C_IMAGES, 1, SS2C_XYB_BLOCK, 1, 1, 0,
                                           stream, args, NULL));
    return 0;
}

/* The five blurs of ssimulacra2.c::extract (products formed on load): one
 * horizontal and one vertical launch covering every job and channel. */
static int ss2c_launch_blurs(const Ssimu2StateCuda *s, CudaFunctions *cu_f, int scale,
                             CUstream stream)
{
    Ss2cBlurArgs a;
    memset(&a, 0, sizeof(a));
    a.ref = (const float *)ss2c_ptr(s->d_xyb[0]);
    a.dis = (const float *)ss2c_ptr(s->d_xyb[1]);
    for (unsigned job = 0; job < SS2C_BLUR_JOBS; job++) {
        a.pass[job] = (float *)ss2c_ptr(s->d_pass[job]);
        a.out[job] = (float *)ss2c_ptr(s->d_blurred[job]);
    }
    a.width = s->scale_w[scale];
    a.height = s->scale_h[scale];
    for (unsigned k = 0; k < 3u; k++) {
        a.n2[k] = s->rg_n2[k];
        a.d1[k] = s->rg_d1[k];
    }
    a.radius = s->rg_radius;
    void *args[] = {&a};
    const unsigned gh = (a.height + SS2C_BLUR_TILE - 1u) / SS2C_BLUR_TILE;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_blur_h, gh, SS2C_BLUR_JOBS, SS2C_CHANNELS,
                                           SS2C_BLUR_TILE, 1, 1, 0, stream, args, NULL));
    const unsigned gv = (a.width + SS2C_BLUR_V_BLOCK - 1u) / SS2C_BLUR_V_BLOCK;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_blur_v, gv, SS2C_BLUR_JOBS, SS2C_CHANNELS,
                                           SS2C_BLUR_V_BLOCK, 1, 1, 0, stream, args, NULL));
    return 0;
}

/* The six sums of every channel of one scale, each the sum of the CPU's loop
 * (ADR-1433): chunk sums, plan, increments, walk. */
static int ss2c_launch_combine(const Ssimu2StateCuda *s, CudaFunctions *cu_f, int scale,
                               CUstream stream)
{
    Ss2cCombineArgs a;
    memset(&a, 0, sizeof(a));
    a.mu1 = (const float *)ss2c_ptr(s->d_blurred[SS2C_MU1]);
    a.mu2 = (const float *)ss2c_ptr(s->d_blurred[SS2C_MU2]);
    a.s11 = (const float *)ss2c_ptr(s->d_blurred[SS2C_S11]);
    a.s22 = (const float *)ss2c_ptr(s->d_blurred[SS2C_S22]);
    a.s12 = (const float *)ss2c_ptr(s->d_blurred[SS2C_S12]);
    a.img1 = (const float *)ss2c_ptr(s->d_xyb[0]);
    a.img2 = (const float *)ss2c_ptr(s->d_xyb[1]);
    a.chunk_sums = (double *)ss2c_ptr(s->d_chunk_sums);
    a.plan = (int16_t *)ss2c_ptr(s->d_plan);
    a.units = (int64_t *)ss2c_ptr(s->d_units);
    a.totals = (double *)ss2c_ptr(s->d_totals) + (size_t)scale * SS2C_TOTALS_PER_SCALE;
    a.pixels = (size_t)s->scale_w[scale] * s->scale_h[scale];
    a.chunks = ss2c_chunks(a.pixels);
    void *args[] = {&a};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_chunk_sums, a.chunks, SS2C_CHANNELS, 1,
                                           SS2C_REDUCE_BLOCK, 1, 1, 0, stream, args, NULL));
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_chunk_plan, SS2C_SUMS, SS2C_CHANNELS, 1,
                                           SS2C_REDUCE_BLOCK, 1, 1, 0, stream, args, NULL));
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_chunk_units, a.chunks, SS2C_CHANNELS, 1,
                                           SS2C_REDUCE_BLOCK, 1, 1, 0, stream, args, NULL));
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_totals, SS2C_SUMS, SS2C_CHANNELS, 1,
                                           SS2C_REDUCE_BLOCK, 1, 1, 0, stream, args, NULL));
    return 0;
}

/* ssimulacra2.c::downsample_2x2 of both linear-RGB images into the other
 * pyramid level. */
static int ss2c_launch_downsample(const Ssimu2StateCuda *s, CudaFunctions *cu_f, int scale,
                                  CUstream stream)
{
    const unsigned level = (unsigned)scale & 1u;
    CUdeviceptr in_ref = s->d_lin[0][level]->data;
    CUdeviceptr in_dis = s->d_lin[1][level]->data;
    CUdeviceptr out_ref = s->d_lin[0][level ^ 1u]->data;
    CUdeviceptr out_dis = s->d_lin[1][level ^ 1u]->data;
    unsigned iw = s->scale_w[scale];
    unsigned ih = s->scale_h[scale];
    unsigned ow = s->scale_w[scale + 1];
    unsigned oh = s->scale_h[scale + 1];
    void *args[] = {&in_ref, &in_dis, &out_ref, &out_dis, &iw, &ih, &ow, &oh};
    const unsigned gx = (ow + SS2C_PIX_BX - 1u) / SS2C_PIX_BX;
    const unsigned gy = (oh + SS2C_PIX_BY - 1u) / SS2C_PIX_BY;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_down, gx, gy, SS2C_IMAGES, SS2C_PIX_BX,
                                           SS2C_PIX_BY, 1, 0, stream, args, NULL));
    return 0;
}

/* The whole frame, enqueued on `stream` without a wait. */
static int ss2c_enqueue_frame(const Ssimu2StateCuda *s, CudaFunctions *cu_f, const VmafPicture *ref,
                              const VmafPicture *dist, CUstream stream)
{
    int err = ss2c_launch_yuv(s, cu_f, ref, dist, stream);
    for (int scale = 0; !err && scale < s->num_scales; scale++) {
        err = ss2c_launch_xyb(s, cu_f, scale, stream);
        if (!err)
            err = ss2c_launch_blurs(s, cu_f, scale, stream);
        if (!err)
            err = ss2c_launch_combine(s, cu_f, scale, stream);
        if (!err && scale + 1 < s->num_scales)
            err = ss2c_launch_downsample(s, cu_f, scale, stream);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Extractor lifecycle                                                */
/* ------------------------------------------------------------------ */

/* The kernels read the device planes with the geometry init() configured; a
 * picture of another shape or depth is refused instead of read past its end
 * (the SYCL twin's ss2s_picture_matches). */
static bool ss2c_picture_matches(const Ssimu2StateCuda *s, const VmafPicture *pic)
{
    if (!pic || pic->bpc != s->bpc)
        return false;
    for (unsigned p = 0; p < SS2C_CHANNELS; p++) {
        if (!pic->data[p] || pic->w[p] != s->plane_w[p] || pic->h[p] != s->plane_h[p])
            return false;
    }
    return true;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    Ssimu2StateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    if (!ss2c_picture_matches(s, ref_pic) || !ss2c_picture_matches(s, dist_pic))
        return -EINVAL;

    CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                              CU_EVENT_WAIT_DEFAULT));
    const int err = ss2c_enqueue_frame(s, cu_f, ref_pic, dist_pic, stream);
    if (err)
        return err;

    const size_t bytes = (size_t)s->num_scales * SS2C_TOTALS_PER_SCALE * sizeof(double);
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->h_totals, s->d_totals->data, bytes, s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    Ssimu2StateCuda *s = fex->priv;
    const int err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (err)
        return err;

    double avg_ssim[6][6] = {{0}};
    double avg_ed[6][12] = {{0}};
    for (int scale = 0; scale < s->num_scales; scale++) {
        ss2c_scale_norms(s->h_totals + (size_t)scale * SS2C_TOTALS_PER_SCALE, s->scale_w[scale],
                         s->scale_h[scale], avg_ssim[scale], avg_ed[scale]);
    }
    const double score = ss2c_pool_score(avg_ssim, avg_ed, s->num_scales);
    if (!vmaf_ss2_score_is_finite(score)) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "ssimulacra2_cuda: non-finite score at frame %u (score=%g), failing frame\n",
                 index, score);
        return -EINVAL;
    }
    return vmaf_feature_collector_append(feature_collector, "ssimulacra2", score, index);
}

/* Frees every device buffer; the first error wins and a buffer whose free
 * fails stays owned for a later retry. */
static int ss2c_free_device_buffers(VmafFeatureExtractor *fex, Ssimu2StateCuda *s)
{
    VmafCudaBuffer **owned[] = {
        &s->d_lin[0][0],  &s->d_lin[0][1],  &s->d_lin[1][0],  &s->d_lin[1][1],  &s->d_xyb[0],
        &s->d_xyb[1],     &s->d_pass[0],    &s->d_pass[1],    &s->d_pass[2],    &s->d_pass[3],
        &s->d_pass[4],    &s->d_blurred[0], &s->d_blurred[1], &s->d_blurred[2], &s->d_blurred[3],
        &s->d_blurred[4], &s->d_chunk_sums, &s->d_plan,       &s->d_units,      &s->d_totals,
    };
    int ret = 0;
    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); i++) {
        const int e = vmaf_cuda_buffer_free_owned(fex->cu_state, owned[i]);
        if (e && !ret)
            ret = e;
    }
    return ret;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    Ssimu2StateCuda *s = fex->priv;
    if (!s || !fex->cu_state)
        return 0;
    /* Drain the lifecycle stream first: device work may still read the
     * buffers and modules released below. */
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (ret) /* still live: keep everything owned for a retry (ADR-1336) */
        return ret;
    int e = ss2c_free_device_buffers(fex, s);
    if (e && !ret)
        ret = e;
    e = vmaf_cuda_buffer_host_free_owned(fex->cu_state, (void **)&s->h_totals);
    if (e && !ret)
        ret = e;
    e = vmaf_cuda_module_unload(fex->cu_state, &s->module_blur);
    if (e && !ret)
        ret = e;
    e = vmaf_cuda_module_unload(fex->cu_state, &s->module_device);
    if (e && !ret)
        ret = e;
    return ret;
}

/* Init failure: release what was created and report the cause. */
static int ss2c_init_unwind(VmafFeatureExtractor *fex, int cause)
{
    const int cleanup = close_fex_cuda(fex);
    return cause ? cause : cleanup;
}

static int ss2c_get_functions(Ssimu2StateCuda *s, CudaFunctions *cu_f)
{
    const struct {
        CUfunction *func;
        CUmodule module;
        const char *name;
    } table[] = {
        {&s->func_blur_h, s->module_blur, "ssimulacra2_blur_h"},
        {&s->func_blur_v, s->module_blur, "ssimulacra2_blur_v"},
        {&s->func_yuv, s->module_device, "ssimulacra2_yuv_to_linear"},
        {&s->func_xyb, s->module_device, "ssimulacra2_xyb"},
        {&s->func_chunk_sums, s->module_device, "ssimulacra2_chunk_sums"},
        {&s->func_chunk_plan, s->module_device, "ssimulacra2_chunk_plan"},
        {&s->func_chunk_units, s->module_device, "ssimulacra2_chunk_units"},
        {&s->func_totals, s->module_device, "ssimulacra2_ordered_totals"},
        {&s->func_down, s->module_device, "ssimulacra2_downsample"},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(table[i].func, table[i].module, table[i].name));
    }
    return 0;
}

/* Module loads with the context already current. */
static int ss2c_load_modules_current(Ssimu2StateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module_blur, ssimulacra2_blur_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module_device, ssimulacra2_device_ptx));
    return ss2c_get_functions(s, cu_f);
}

static int ss2c_load_modules(VmafFeatureExtractor *fex, Ssimu2StateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    int err = ss2c_load_modules_current(s, cu_f);
    const CUresult pop = cu_f->cuCtxPopCurrent(NULL);
    if (err == 0 && pop != CUDA_SUCCESS)
        err = vmaf_cuda_result_to_errno((int)pop);
    return err;
}

static int ss2c_alloc_buffers(VmafFeatureExtractor *fex, Ssimu2StateCuda *s)
{
    const size_t full = (size_t)SS2C_CHANNELS * s->width * s->height * sizeof(float);
    const size_t half = (size_t)SS2C_CHANNELS * s->scale_w[1] * s->scale_h[1] * sizeof(float);
    const size_t totals = (size_t)SS2C_NUM_SCALES * SS2C_TOTALS_PER_SCALE * sizeof(double);
    /* Scale 0 has the most chunks; every (channel, sum, chunk) has a sum, a
     * plan and an increment pair. */
    const size_t slots = SS2C_TOTALS_PER_SCALE * (size_t)ss2c_chunks((size_t)s->width * s->height);
    const struct {
        VmafCudaBuffer **buf;
        size_t bytes;
    } table[] = {
        {&s->d_lin[0][0], full},
        {&s->d_lin[0][1], half},
        {&s->d_lin[1][0], full},
        {&s->d_lin[1][1], half},
        {&s->d_xyb[0], full},
        {&s->d_xyb[1], full},
        {&s->d_pass[0], full},
        {&s->d_pass[1], full},
        {&s->d_pass[2], full},
        {&s->d_pass[3], full},
        {&s->d_pass[4], full},
        {&s->d_blurred[0], full},
        {&s->d_blurred[1], full},
        {&s->d_blurred[2], full},
        {&s->d_blurred[3], full},
        {&s->d_blurred[4], full},
        {&s->d_chunk_sums, slots * sizeof(double)},
        {&s->d_plan, slots * sizeof(int16_t)},
        {&s->d_units, slots * 2u * sizeof(int64_t)},
        {&s->d_totals, totals},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const int err = vmaf_cuda_buffer_alloc(fex->cu_state, table[i].buf, table[i].bytes);
        if (err)
            return err;
    }
    return vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->h_totals, totals);
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    Ssimu2StateCuda *s = fex->priv;
    int err = vmaf_ss2_check_pixel_format(pix_fmt, "ssimulacra2_cuda");
    if (err)
        return err;
    if (!fex->cu_state)
        return -EINVAL;
    if (w < 8u || h < 8u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssimulacra2_cuda: input %ux%u below 8x8 lower bound\n", w,
                 h);
        return -EINVAL;
    }
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    ss2c_configure_planes(s, pix_fmt);
    ss2c_configure_scales(s);
    ss2c_setup_gaussian(s, SS2C_SIGMA);
    if (s->rg_radius < 1 || s->rg_radius > SS2C_BLUR_MAX_RADIUS)
        return -EINVAL; /* ssimulacra2_blur_h keeps two 32-column tiles */
    ss2c_yuv_coeffs(s->yuv_matrix, bpc, &s->yuv);

    err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;
    err = ss2c_load_modules(fex, s);
    if (!err)
        err = ss2c_alloc_buffers(fex, s);
    return err ? ss2c_init_unwind(fex, err) : 0;
}

/* ADR-1324 context check: 4:0:0 input (no chroma to convert) and frames below
 * 8x8, which init rejects, go to the CPU extractor when the twin was picked by
 * a model or by the ADR-1359 --backend mapping. */
static int check_context_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                              unsigned w, unsigned h)
{
    (void)fex;
    (void)bpc;
    return (vmaf_ss2_has_chroma(pix_fmt) && w >= 8u && h >= 8u) ? 0 : -ENOTSUP;
}

static const char *provided_features[] = {"ssimulacra2", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_ssimulacra2_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_ssimulacra2_cuda = {
    .name = "ssimulacra2_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(Ssimu2StateCuda),
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    .provided_features = provided_features,
    .context_check = check_context_cuda,
    .context_fallback_name = "ssimulacra2",
    .chars =
        {
            /* YUV conversion, then per scale XYB, blur H + V and the four
             * launches of the sums (6 x 7), and the 5 downsamples between
             * scales. */
            .n_dispatches_per_frame = 1 + SS2C_NUM_SCALES * 7 + (SS2C_NUM_SCALES - 1),
            .is_reduction_only = false,
            .min_useful_frame_area = 0,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
