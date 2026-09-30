/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  HIP compute kernels for the float_ssim feature extractor
 *  (T7-10b batch-3 / ADR-0374). Direct port of the CUDA twin at
 *  `core/src/feature/cuda/integer_ssim/ssim_score.cu`.
 *
 *  Two-pass design mirrors the GLSL Vulkan shader and the CUDA twin:
 *
 *    1. calculate_ssim_hip_horiz_{8,16}bpc — horizontal 11-tap
 *       separable Gaussian over ref / cmp / ref^2 / cmp^2 / ref*cmp.
 *       Output: five intermediate float buffers, each (W-10) x H.
 *
 *    2. calculate_ssim_hip_vert_combine — vertical 11-tap on the five
 *       intermediates, then each pixel's SSIM term exactly as the CPU's
 *       iqa/ssim_tools.c ssim_accumulate_default_scalar() forms it:
 *       l * c * s in double from the CPU-typed luminance, contrast and
 *       structure terms (ssim_pixel()). One double per block in `partials`
 *       (shared-memory tree).
 *
 *    2b. calculate_ssim_hip_vert_combine_lcs (`enable_lcs`, ADR-1382): the
 *       same pass 2, plus the block sums of the per-pixel l, c and s.
 *
 *  The host adds the block partials in double, divides by (W-10)*(H-10) and
 *  rounds the mean to fp32, as the CPU's iqa_ssim() returns it. There is no
 *  identical-window shortcut: on identical frames the CPU's fp32 luminance
 *  denominator can leave the frame mean at 1 - 2^-24 (72.247 dB with
 *  `enable_db`), and this twin reproduces that arithmetic instead of forcing
 *  1 (ADR-1382, Research-1377).
 *
 *  HIP adaptation from CUDA:
 *  - Warp size varies: 64 on GCN/RDNA1 (wave64), 32 on RDNA2+ (wave32).
 *    Pass 2 reduces through a shared-memory tree over all SSIM_BLOCK_SIZE
 *    threads (ssim_block_sum()), so its order is the same on both.
 *  - VmafCudaBuffer / VmafPicture replaced by raw pointer parameters.
 *
 *  v1 constraint: scale=1 only — same as ssim_vulkan / ssim_cuda.
 *  The host init() rejects scale != 1 with -EINVAL before dispatching.
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#define SSIM_BX 16
#define SSIM_BY 8
#define SSIM_K  11
#define SSIM_BLOCK_SIZE (SSIM_BX * SSIM_BY)

/* 11-tap Gaussian kernel (same constants as the CUDA twin). */
__device__ static const float SSIM_G[SSIM_K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

/* Device helpers are called only by the kernels of this translation unit,
 * so they live in an anonymous namespace (internal linkage). */
namespace
{

/* The five vertically filtered moments of one output pixel. */
struct SsimMoments {
    float ref_mu;
    float cmp_mu;
    float ref_sq;
    float cmp_sq;
    float refcmp;
};

/* Vertical 11-tap pass over the five horizontal planes at (x, y). */
__device__ __forceinline__ SsimMoments
ssim_vertical_moments(const float *__restrict__ h_ref_mu, const float *__restrict__ h_cmp_mu,
                      const float *__restrict__ h_ref_sq, const float *__restrict__ h_cmp_sq,
                      const float *__restrict__ h_refcmp, unsigned w_horiz, unsigned x, unsigned y)
{
    SsimMoments m = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int v = 0; v < SSIM_K; v++) {
        const unsigned src_idx = (y + (unsigned)v) * w_horiz + x;
        const float w = SSIM_G[v];
        m.ref_mu += w * h_ref_mu[src_idx];
        m.cmp_mu += w * h_cmp_mu[src_idx];
        m.ref_sq += w * h_ref_sq[src_idx];
        m.cmp_sq += w * h_cmp_sq[src_idx];
        m.refcmp += w * h_refcmp[src_idx];
    }
    return m;
}

/* Per-pixel L / C / S of CPU iqa/ssim_tools.c in its own types:
 * ssim_variance_scalar() clamps both variances at zero;
 * ssim_accumulate_default_scalar() takes sigma_ref * sigma_cmp as one fp32
 * square root, forms L and C in double over fp32 denominators, clamps a
 * negative covariance to zero on a flat window, and divides S in fp32, with
 * C3 = C2 / 2. */
__device__ __forceinline__ void ssim_lcs(const SsimMoments &m, float c1, float c2, double *l,
                                         double *c, double *s)
{
#pragma clang fp contract(off)
    const float ref_raw = m.ref_sq - m.ref_mu * m.ref_mu;
    const float cmp_raw = m.cmp_sq - m.cmp_mu * m.cmp_mu;
    const float ref_var = ref_raw < 0.0f ? 0.0f : ref_raw;
    const float cmp_var = cmp_raw < 0.0f ? 0.0f : cmp_raw;
    const float covar = m.refcmp - m.ref_mu * m.cmp_mu;
    const float sigma_product = sqrtf(ref_var * cmp_var);
    const float c3 = c2 / 2.0f;
    const float flat_covar = (covar < 0.0f && sigma_product <= 0.0f) ? 0.0f : covar;
    *l = (2.0 * m.ref_mu * m.cmp_mu + c1) / (m.ref_mu * m.ref_mu + m.cmp_mu * m.cmp_mu + c1);
    *c = (2.0 * sigma_product + c2) / (ref_var + cmp_var + c2);
    *s = (flat_covar + c3) / (sigma_product + c3);
}

/* One pixel's SSIM term as the CPU adds it into ssim_sum: (l * c) * s in
 * double. Writes the three factors to `lcs` for the enable_lcs kernel. */
__device__ __forceinline__ double ssim_pixel(const SsimMoments &m, float c1, float c2,
                                             double lcs[3])
{
    ssim_lcs(m, c1, c2, &lcs[0], &lcs[1], &lcs[2]);
    return lcs[0] * lcs[1] * lcs[2];
}

/* Block sum of a double over all SSIM_BLOCK_SIZE threads through shared
 * memory, the same order on wave32 and wave64. Every thread must call it. */
__device__ __forceinline__ double ssim_block_sum(double v, double *scratch, unsigned lid)
{
    scratch[lid] = v;
    __syncthreads();
    for (unsigned half = SSIM_BLOCK_SIZE / 2u; half > 0u; half >>= 1u) {
        if (lid < half)
            scratch[lid] += scratch[lid + half];
        __syncthreads();
    }
    const double total = scratch[0];
    __syncthreads();
    return total;
}

/* Store the block sum of `v` at this block's slot of `out` (lane 0 only).
 * Every thread must call it: ssim_block_sum() synchronises the block. */
__device__ __forceinline__ void ssim_store_block_sum(double v, double *__restrict__ out,
                                                     double *scratch, unsigned lid)
{
    const double total = ssim_block_sum(v, scratch, lid);
    if (lid == 0u)
        out[blockIdx.y * gridDim.x + blockIdx.x] = total;
}

} // namespace

extern "C" {

/**
 * Pass 1 (8-bpc): horizontal 11-tap Gaussian.
 * ref/dis are uint8_t row-major, ref_stride/dis_stride bytes per row.
 * Writes h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp (each
 * w_horiz x h_horiz floats, stride = w_horiz).
 */
__global__ void calculate_ssim_hip_horiz_8bpc(const uint8_t *__restrict__ ref,
                                               ptrdiff_t ref_stride,
                                               const uint8_t *__restrict__ cmp,
                                               ptrdiff_t cmp_stride,
                                               float *__restrict__ h_ref_mu,
                                               float *__restrict__ h_cmp_mu,
                                               float *__restrict__ h_ref_sq,
                                               float *__restrict__ h_cmp_sq,
                                               float *__restrict__ h_refcmp,
                                               unsigned w_horiz,
                                               unsigned h_horiz)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz)
        return;

    float ref_mu_h = 0.0f, cmp_mu_h = 0.0f;
    float ref_sq_h = 0.0f, cmp_sq_h = 0.0f, refcmp_h = 0.0f;

    for (int u = 0; u < SSIM_K; u++) {
        const unsigned src_x = x + (unsigned)u;
        const float r = (float)ref[(ptrdiff_t)y * ref_stride + src_x];
        const float c = (float)cmp[(ptrdiff_t)y * cmp_stride + src_x];
        const float w = SSIM_G[u];
        ref_mu_h += w * r;
        cmp_mu_h += w * c;
        ref_sq_h += w * (r * r);
        cmp_sq_h += w * (c * c);
        refcmp_h += w * (r * c);
    }

    const unsigned dst_idx = y * w_horiz + x;
    h_ref_mu[dst_idx] = ref_mu_h;
    h_cmp_mu[dst_idx] = cmp_mu_h;
    h_ref_sq[dst_idx] = ref_sq_h;
    h_cmp_sq[dst_idx] = cmp_sq_h;
    h_refcmp[dst_idx] = refcmp_h;
}

/**
 * Pass 1 (16-bpc): horizontal 11-tap Gaussian.
 * Same as 8bpc but pixels are uint16_t (10/12/16bpc), normalised by
 * scaler (4 / 16 / 256) to the [0,255] float range the CUDA twin uses.
 */
__global__ void calculate_ssim_hip_horiz_16bpc(const uint8_t *__restrict__ ref,
                                                ptrdiff_t ref_stride,
                                                const uint8_t *__restrict__ cmp,
                                                ptrdiff_t cmp_stride,
                                                float *__restrict__ h_ref_mu,
                                                float *__restrict__ h_cmp_mu,
                                                float *__restrict__ h_ref_sq,
                                                float *__restrict__ h_cmp_sq,
                                                float *__restrict__ h_refcmp,
                                                unsigned w_horiz,
                                                unsigned h_horiz,
                                                unsigned bpc)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz)
        return;

    float scaler = 1.0f;
    if (bpc == 10u)
        scaler = 4.0f;
    else if (bpc == 12u)
        scaler = 16.0f;
    else if (bpc == 16u)
        scaler = 256.0f;
    const float inv_sc = 1.0f / scaler;

    float ref_mu_h = 0.0f, cmp_mu_h = 0.0f;
    float ref_sq_h = 0.0f, cmp_sq_h = 0.0f, refcmp_h = 0.0f;

    for (int u = 0; u < SSIM_K; u++) {
        const unsigned src_x = x + (unsigned)u;
        const float r = (float)reinterpret_cast<const uint16_t *>(
            ref + (ptrdiff_t)y * ref_stride)[src_x] * inv_sc;
        const float c = (float)reinterpret_cast<const uint16_t *>(
            cmp + (ptrdiff_t)y * cmp_stride)[src_x] * inv_sc;
        const float w = SSIM_G[u];
        ref_mu_h += w * r;
        cmp_mu_h += w * c;
        ref_sq_h += w * (r * r);
        cmp_sq_h += w * (c * c);
        refcmp_h += w * (r * c);
    }

    const unsigned dst_idx = y * w_horiz + x;
    h_ref_mu[dst_idx] = ref_mu_h;
    h_cmp_mu[dst_idx] = cmp_mu_h;
    h_ref_sq[dst_idx] = ref_sq_h;
    h_cmp_sq[dst_idx] = cmp_sq_h;
    h_refcmp[dst_idx] = refcmp_h;
}

/**
 * Pass 2: vertical 11-tap Gaussian + the CPU's per-pixel SSIM term +
 * per-block double partial sum. Grid sized over (W-10) x (H-10) =
 * w_final x h_final. `partials` receives one double per block
 * (block_idx = blockIdx.y * gridDim.x + blockIdx.x).
 */
__global__ void calculate_ssim_hip_vert_combine(const float *__restrict__ h_ref_mu,
                                                 const float *__restrict__ h_cmp_mu,
                                                 const float *__restrict__ h_ref_sq,
                                                 const float *__restrict__ h_cmp_sq,
                                                 const float *__restrict__ h_refcmp,
                                                 double *__restrict__ partials,
                                                 unsigned w_horiz,
                                                 unsigned w_final,
                                                 unsigned h_final,
                                                 float c1,
                                                 float c2)
{
    __shared__ double s_scratch[SSIM_BLOCK_SIZE];
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    double my_ssim = 0.0;
    if (x < w_final && y < h_final) {
        const SsimMoments m = ssim_vertical_moments(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq,
                                                    h_refcmp, w_horiz, x, y);
        double lcs[3];
        my_ssim = ssim_pixel(m, c1, c2, lcs);
    }
    ssim_store_block_sum(my_ssim, partials, s_scratch, lid);
}

/**
 * Pass 2 with `enable_lcs` (ADR-1382): the SSIM partial of the default
 * kernel, plus the block sums of the per-pixel L, C and S terms in
 * `lcs_partials`, laid out [l | c | s] with `block_count` doubles each.
 * Out-of-frame threads contribute zeros.
 */
__global__ void calculate_ssim_hip_vert_combine_lcs(const float *__restrict__ h_ref_mu,
                                                     const float *__restrict__ h_cmp_mu,
                                                     const float *__restrict__ h_ref_sq,
                                                     const float *__restrict__ h_cmp_sq,
                                                     const float *__restrict__ h_refcmp,
                                                     double *__restrict__ partials,
                                                     double *__restrict__ lcs_partials,
                                                     unsigned w_horiz, unsigned w_final,
                                                     unsigned h_final, unsigned block_count,
                                                     float c1, float c2)
{
    __shared__ double s_scratch[SSIM_BLOCK_SIZE];
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    double my_ssim = 0.0;
    double lcs[3] = {0.0, 0.0, 0.0};
    if (x < w_final && y < h_final) {
        const SsimMoments m = ssim_vertical_moments(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq,
                                                    h_refcmp, w_horiz, x, y);
        my_ssim = ssim_pixel(m, c1, c2, lcs);
    }
    ssim_store_block_sum(my_ssim, partials, s_scratch, lid);
    for (unsigned k = 0u; k < 3u; k++)
        ssim_store_block_sum(lcs[k], lcs_partials + ((size_t)k * block_count), s_scratch, lid);
}

} /* extern "C" */
