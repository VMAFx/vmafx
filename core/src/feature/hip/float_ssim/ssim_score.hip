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
 *       intermediates + per-pixel SSIM combine + per-block float
 *       partial sum (tree reduce in shared memory). Output: one float
 *       per block in `partials`.
 *
 *  Host accumulates partials in double, divides by (W-10)*(H-10).
 *
 *    2b. calculate_ssim_hip_vert_combine_lcs (`enable_lcs`, ADR-1382):
 *       the same pass 2, plus the per-pixel luminance / contrast /
 *       structure terms of iqa/ssim_tools.c in the CPU's own types, reduced
 *       per block into three double partials next to the SSIM value.
 *
 *  An identical window scores exactly 1 (ssim_from_moments()), as on the
 *  CPU, so `enable_db` reports the CPU's +inf / clip_db ceiling for
 *  identical frames instead of the dB value of an fp32 rounding residue.
 *
 *  HIP adaptation from CUDA:
 *  - Warp size varies: 64 on GCN/RDNA1 (wave64), 32 on RDNA2+ (wave32).
 *    `warpSize` is read at runtime so the same binary runs on both.
 *    Shared-memory is sized for the minimum warp size (32) so it fits
 *    the maximum warps-per-block on wave32 hardware:
 *    BLOCK_SIZE / SSIM_MIN_WARP_SIZE = 128 / 32 = 4 slots.
 *  - VmafCudaBuffer / VmafPicture replaced by raw pointer parameters.
 *  - `__shfl_down` without mask (HIP convention).
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

/* Minimum HIP warp size across supported targets (32 on RDNA2+, 64 on GCN/RDNA1).
 * Shared memory is sized for the worst case (wave32 = most warps per block).
 * The runtime `warpSize` device variable is used in reduction loops and lane
 * detection so the same binary runs correctly on both wave32 and wave64. */
#define SSIM_MIN_WARP_SIZE    32
#define SSIM_WARPS_PER_BLOCK  (SSIM_BLOCK_SIZE / SSIM_MIN_WARP_SIZE)

/* 11-tap Gaussian kernel (same constants as the CUDA twin). */
__device__ static const float SSIM_G[SSIM_K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

__device__ __forceinline__ float ssim_warp_reduce(float v)
{
    for (int off = (int)warpSize / 2; off > 0; off >>= 1)
        v += __shfl_down(v, off);
    return v;
}

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

/* SSIM of one window. Contraction is off here and every product sits in a
 * named temporary, so the numerator and the denominator run the same
 * operations mirrored: an identical window compares equal and scores
 * exactly 1, as the CPU does, and `enable_db` reports the CPU's +inf /
 * clip_db ceiling for it instead of a finite dB value of an fp32 rounding
 * residue (ADR-1382, ADR-1221; the SYCL twin's ADR-1365 form). */
__device__ __forceinline__ float ssim_from_moments(const SsimMoments &m, float c1, float c2)
{
#pragma clang fp contract(off)
    const float ref_mu_sq = m.ref_mu * m.ref_mu;
    const float cmp_mu_sq = m.cmp_mu * m.cmp_mu;
    const float mu_xy = m.ref_mu * m.cmp_mu;
    const float ref_var = m.ref_sq - ref_mu_sq;
    const float cmp_var = m.cmp_sq - cmp_mu_sq;
    const float covar = m.refcmp - mu_xy;
    const float num = (2.0f * mu_xy + c1) * (2.0f * covar + c2);
    const float den = (ref_mu_sq + cmp_mu_sq + c1) * (ref_var + cmp_var + c2);
    return num == den ? 1.0f : num / den;
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

/* The float SSIM partial of one block: warp sums, then lane 0 of thread 0
 * adds the warps in order. Every thread must call it. */
__device__ __forceinline__ void ssim_store_block(float value, float *__restrict__ partials,
                                                 unsigned lid)
{
    __shared__ float s_warp_sums[SSIM_WARPS_PER_BLOCK];
    const float warp_sum = ssim_warp_reduce(value);
    const unsigned lane = lid % (unsigned)warpSize;
    const unsigned warp_id = lid / (unsigned)warpSize;
    if (lane == 0)
        s_warp_sums[warp_id] = warp_sum;
    __syncthreads();

    if (lid == 0) {
        float block_sum = 0.0f;
        const int nwarps = (int)(blockDim.x * blockDim.y) / (int)warpSize;
        for (int i = 0; i < nwarps; i++)
            block_sum += s_warp_sums[i];
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        partials[block_idx] = block_sum;
    }
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
 * Pass 2: vertical 11-tap Gaussian + per-pixel SSIM combine +
 * per-block float partial sum. Same algorithm as the CUDA twin, with the
 * mirrored per-pixel arithmetic of ssim_from_moments().
 * Grid sized over (W-10) x (H-10) = w_final x h_final.
 * `partials` receives one float per block (block_idx = blockIdx.y *
 * gridDim.x + blockIdx.x).
 */
__global__ void calculate_ssim_hip_vert_combine(const float *__restrict__ h_ref_mu,
                                                 const float *__restrict__ h_cmp_mu,
                                                 const float *__restrict__ h_ref_sq,
                                                 const float *__restrict__ h_cmp_sq,
                                                 const float *__restrict__ h_refcmp,
                                                 float *__restrict__ partials,
                                                 unsigned w_horiz,
                                                 unsigned w_final,
                                                 unsigned h_final,
                                                 float c1,
                                                 float c2)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    float my_ssim = 0.0f;
    if (x < w_final && y < h_final) {
        const SsimMoments m = ssim_vertical_moments(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq,
                                                    h_refcmp, w_horiz, x, y);
        my_ssim = ssim_from_moments(m, c1, c2);
    }
    ssim_store_block(my_ssim, partials, lid);
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
                                                     float *__restrict__ partials,
                                                     double *__restrict__ lcs_partials,
                                                     unsigned w_horiz, unsigned w_final,
                                                     unsigned h_final, unsigned block_count,
                                                     float c1, float c2)
{
    __shared__ double s_scratch[SSIM_BLOCK_SIZE];
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    float my_ssim = 0.0f;
    double lcs[3] = {0.0, 0.0, 0.0};
    if (x < w_final && y < h_final) {
        const SsimMoments m = ssim_vertical_moments(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq,
                                                    h_refcmp, w_horiz, x, y);
        my_ssim = ssim_from_moments(m, c1, c2);
        ssim_lcs(m, c1, c2, &lcs[0], &lcs[1], &lcs[2]);
    }
    ssim_store_block(my_ssim, partials, lid);
    const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
    for (unsigned k = 0u; k < 3u; k++) {
        const double total = ssim_block_sum(lcs[k], s_scratch, lid);
        if (lid == 0u)
            lcs_partials[(size_t)k * block_count + block_idx] = total;
    }
}

} /* extern "C" */
