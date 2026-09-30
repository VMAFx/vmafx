/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  CUDA compute kernels for the float_ssim feature extractor
 *  (T7-23 / batch 2 part 1b / ADR-0188 / ADR-0189). CUDA twin of
 *  ssim_vulkan (PR #139). Two-pass design mirrors the GLSL
 *  shader byte-for-byte modulo language differences:
 *
 *    1. calculate_ssim_horiz_{8,16}bpc — reads the picture's
 *       data[0] (uint8 or uint16) at the appropriate bpc, does
 *       the picture_copy normalisation inline (uint → float
 *       in [0, 255]), then horizontal 11-tap separable Gaussian
 *       over ref / cmp / ref² / cmp² / ref·cmp.
 *
 *    2. calculate_ssim_vert_combine — vertical 11-tap on the
 *       five intermediates + per-pixel SSIM combine + per-block
 *       float partial sum (tree reduce in shared memory).
 *       calculate_ssim_vert_combine_lcs is the `enable_lcs` variant:
 *       the same SSIM value plus the per-pixel luminance, contrast and
 *       structure terms of iqa/ssim_tools.c, reduced per block next to it
 *       (ADR-1373).
 *
 *  Mirrors the precision pattern of ciede_cuda + ssim_vulkan:
 *  per-block float partials, host accumulates in `double`,
 *  divides by (W-10)·(H-10) to recover mean SSIM.
 *
 *  v1: scale=1 only — same constraint as ssim_vulkan. Auto-
 *  decimation rejection happens host-side.
 */

#include "cuda_helper.cuh"
#include "cuda/integer_ssim_cuda.h"
#include "common.h"

#define BLOCK_X 16
#define BLOCK_Y 8
#define BLOCK_SIZE (BLOCK_X * BLOCK_Y)
#define WARPS_PER_BLOCK (BLOCK_SIZE / 32)
#define K 11
#define LCS_TERMS 3

namespace
{

__device__ const float G[K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

/* Read normalised float from the picture's luma plane. Mirrors
 * picture_copy / picture_copy_hbd: 8bpc → uint8 cast, 10/12/16bpc
 * → uint16 / scaler where scaler = 4 / 16 / 256. */
__device__ inline float read_norm_8bpc(const VmafPicture &pic, unsigned x, unsigned y)
{
    const uint8_t *row = reinterpret_cast<const uint8_t *>(pic.data[0]) + y * pic.stride[0];
    return (float)row[x];
}

__device__ inline float read_norm_16bpc(const VmafPicture &pic, unsigned x, unsigned y,
                                        float scaler)
{
    const uint16_t *row = reinterpret_cast<const uint16_t *>(
        reinterpret_cast<const uint8_t *>(pic.data[0]) + y * pic.stride[0]);
    return (float)row[x] / scaler;
}

__device__ inline float scaler_for_bpc(unsigned bpc)
{
    if (bpc == 10)
        return 4.0f;
    if (bpc == 12)
        return 16.0f;
    if (bpc == 16)
        return 256.0f;
    return 1.0f;
}

/* The five vertical-pass inputs, raw pointers extracted once per kernel so
 * the compiler can route every tap through the read-only cache (ADR-0754). */
struct SsimVertInputs {
    const float *__restrict__ ref_mu;
    const float *__restrict__ cmp_mu;
    const float *__restrict__ ref_sq;
    const float *__restrict__ cmp_sq;
    const float *__restrict__ refcmp;
};

struct SsimMoments {
    float ref_mu;
    float cmp_mu;
    float ref_sq;
    float cmp_sq;
    float refcmp;
};

__device__ inline SsimVertInputs vert_inputs(const VmafCudaBuffer &ref_mu,
                                             const VmafCudaBuffer &cmp_mu,
                                             const VmafCudaBuffer &ref_sq,
                                             const VmafCudaBuffer &cmp_sq,
                                             const VmafCudaBuffer &refcmp)
{
    return {
        reinterpret_cast<const float *>(ref_mu.data), reinterpret_cast<const float *>(cmp_mu.data),
        reinterpret_cast<const float *>(ref_sq.data), reinterpret_cast<const float *>(cmp_sq.data),
        reinterpret_cast<const float *>(refcmp.data)};
}

/* Vertical 11-tap of the five horizontal-pass moments at (x, y). */
__device__ inline SsimMoments vertical_moments(const SsimVertInputs &in, unsigned x, unsigned y,
                                               unsigned w_horiz)
{
    SsimMoments m = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int v = 0; v < K; v++) {
        const unsigned src_idx = (y + (unsigned)v) * w_horiz + x;
        const float w = G[v];
        m.ref_mu += w * __ldg(&in.ref_mu[src_idx]);
        m.cmp_mu += w * __ldg(&in.cmp_mu[src_idx]);
        m.ref_sq += w * __ldg(&in.ref_sq[src_idx]);
        m.cmp_sq += w * __ldg(&in.cmp_sq[src_idx]);
        m.refcmp += w * __ldg(&in.refcmp[src_idx]);
    }
    return m;
}

/* The three products of the SSIM combine, each rounded on its own:
 * __fmul_rn is never contracted into an FMA, so the numerator and the
 * denominator below run the same operations mirrored. */
struct SsimProducts {
    float ref_mean_sq;
    float cmp_mean_sq;
    float mean_product;
};

__device__ inline SsimProducts ssim_products(const SsimMoments &m)
{
    return {__fmul_rn(m.ref_mu, m.ref_mu), __fmul_rn(m.cmp_mu, m.cmp_mu),
            __fmul_rn(m.ref_mu, m.cmp_mu)};
}

/* SSIM of one window. With the mirrored operations an identical window
 * compares numerator == denominator and scores exactly 1, as the CPU does,
 * so `enable_db` reports the CPU's +inf / `clip_db` ceiling for identical
 * frames instead of the dB of an fp32 rounding residue (ADR-1221, ADR-1373). */
__device__ inline float ssim_from_moments(const SsimMoments &m, float c1, float c2)
{
    const SsimProducts p = ssim_products(m);
    const float ref_var = m.ref_sq - p.ref_mean_sq;
    const float cmp_var = m.cmp_sq - p.cmp_mean_sq;
    const float covar = m.refcmp - p.mean_product;
    const float num = (2.0f * p.mean_product + c1) * (2.0f * covar + c2);
    const float den = (p.ref_mean_sq + p.cmp_mean_sq + c1) * (ref_var + cmp_var + c2);
    return (num == den) ? 1.0f : num / den;
}

/* Per-pixel luminance, contrast and structure of CPU iqa/ssim_tools.c in
 * fp32: ssim_variance_scalar clamps both variances at zero, and
 * ssim_accumulate_default_scalar takes sigma_ref * sigma_cmp as one square
 * root and clamps a negative covariance to zero on a flat window, with
 * C3 = C2 / 2. The same arithmetic as the SYCL twin (ADR-1365). */
__device__ inline void ssim_lcs(const SsimMoments &m, float c1, float c2, float (&lcs)[LCS_TERMS])
{
    const SsimProducts p = ssim_products(m);
    const float ref_raw = m.ref_sq - p.ref_mean_sq;
    const float cmp_raw = m.cmp_sq - p.cmp_mean_sq;
    const float ref_var = ref_raw < 0.0f ? 0.0f : ref_raw;
    const float cmp_var = cmp_raw < 0.0f ? 0.0f : cmp_raw;
    const float covar = m.refcmp - p.mean_product;
    const float sigma_product = sqrtf(__fmul_rn(ref_var, cmp_var));
    const float c3 = c2 / 2.0f;
    const float flat_covar = (covar < 0.0f && sigma_product <= 0.0f) ? 0.0f : covar;
    lcs[0] = (2.0f * p.mean_product + c1) / (p.ref_mean_sq + p.cmp_mean_sq + c1);
    lcs[1] = (2.0f * sigma_product + c2) / (ref_var + cmp_var + c2);
    lcs[2] = (flat_covar + c3) / (sigma_product + c3);
}

/* Sum `value` over the block into thread 0's return value: warp shuffle,
 * then the warp sums in warp order (ciede_cuda precision pattern). Every
 * thread must call it; `scratch` is reusable once it returns. */
__device__ inline float block_sum(float value, float (&scratch)[WARPS_PER_BLOCK])
{
    for (int off = 16; off > 0; off >>= 1)
        value += __shfl_down_sync(0xffffffff, value, off);
    const int tid = threadIdx.y * blockDim.x + threadIdx.x;
    if (tid % 32 == 0)
        scratch[tid / 32] = value;
    __syncthreads();
    float total = 0.0f;
    if (tid == 0) {
        for (int i = 0; i < WARPS_PER_BLOCK; i++)
            total += scratch[i];
    }
    __syncthreads();
    return total;
}

__device__ inline unsigned block_index()
{
    return blockIdx.y * gridDim.x + blockIdx.x;
}

__device__ inline bool is_block_leader()
{
    return threadIdx.x == 0 && threadIdx.y == 0;
}

} // namespace

extern "C" {

/* Pass 1 — horizontal: each thread is one output pixel of the
 * (W-10) × H "valid" buffer. Reads input columns [x, x+10] and
 * writes the 5 horizontal-pass values. */
__global__ void calculate_ssim_horiz_8bpc(const VmafPicture ref, const VmafPicture cmp,
                                          VmafCudaBuffer h_ref_mu, VmafCudaBuffer h_cmp_mu,
                                          VmafCudaBuffer h_ref_sq, VmafCudaBuffer h_cmp_sq,
                                          VmafCudaBuffer h_refcmp, unsigned w_horiz,
                                          unsigned h_horiz, unsigned width)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz)
        return;
    (void)width;

    float ref_mu_h = 0.0f;
    float cmp_mu_h = 0.0f;
    float ref_sq_h = 0.0f;
    float cmp_sq_h = 0.0f;
    float refcmp_h = 0.0f;

    for (int u = 0; u < K; u++) {
        const unsigned src_x = x + (unsigned)u;
        const float r = read_norm_8bpc(ref, src_x, y);
        const float c = read_norm_8bpc(cmp, src_x, y);
        const float w = G[u];
        ref_mu_h += w * r;
        cmp_mu_h += w * c;
        ref_sq_h += w * (r * r);
        cmp_sq_h += w * (c * c);
        refcmp_h += w * (r * c);
    }
    const unsigned dst_idx = y * w_horiz + x;
    reinterpret_cast<float *>(h_ref_mu.data)[dst_idx] = ref_mu_h;
    reinterpret_cast<float *>(h_cmp_mu.data)[dst_idx] = cmp_mu_h;
    reinterpret_cast<float *>(h_ref_sq.data)[dst_idx] = ref_sq_h;
    reinterpret_cast<float *>(h_cmp_sq.data)[dst_idx] = cmp_sq_h;
    reinterpret_cast<float *>(h_refcmp.data)[dst_idx] = refcmp_h;
}

__global__ void calculate_ssim_horiz_16bpc(const VmafPicture ref, const VmafPicture cmp,
                                           VmafCudaBuffer h_ref_mu, VmafCudaBuffer h_cmp_mu,
                                           VmafCudaBuffer h_ref_sq, VmafCudaBuffer h_cmp_sq,
                                           VmafCudaBuffer h_refcmp, unsigned w_horiz,
                                           unsigned h_horiz, unsigned bpc, unsigned width)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz)
        return;
    (void)width;
    const float scaler = scaler_for_bpc(bpc);

    float ref_mu_h = 0.0f;
    float cmp_mu_h = 0.0f;
    float ref_sq_h = 0.0f;
    float cmp_sq_h = 0.0f;
    float refcmp_h = 0.0f;

    for (int u = 0; u < K; u++) {
        const unsigned src_x = x + (unsigned)u;
        const float r = read_norm_16bpc(ref, src_x, y, scaler);
        const float c = read_norm_16bpc(cmp, src_x, y, scaler);
        const float w = G[u];
        ref_mu_h += w * r;
        cmp_mu_h += w * c;
        ref_sq_h += w * (r * r);
        cmp_sq_h += w * (c * c);
        refcmp_h += w * (r * c);
    }
    const unsigned dst_idx = y * w_horiz + x;
    reinterpret_cast<float *>(h_ref_mu.data)[dst_idx] = ref_mu_h;
    reinterpret_cast<float *>(h_cmp_mu.data)[dst_idx] = cmp_mu_h;
    reinterpret_cast<float *>(h_ref_sq.data)[dst_idx] = ref_sq_h;
    reinterpret_cast<float *>(h_cmp_sq.data)[dst_idx] = cmp_sq_h;
    reinterpret_cast<float *>(h_refcmp.data)[dst_idx] = refcmp_h;
}

/* Pass 2 — vertical + SSIM combine + per-block partial sum.
 * __launch_bounds__(128) hints nvcc to budget registers for
 * 128-thread blocks; per ADR-0754 / ADR-0743 precedent. */
__launch_bounds__(BLOCK_SIZE) __global__
    void calculate_ssim_vert_combine(VmafCudaBuffer h_ref_mu_buf, VmafCudaBuffer h_cmp_mu_buf,
                                     VmafCudaBuffer h_ref_sq_buf, VmafCudaBuffer h_cmp_sq_buf,
                                     VmafCudaBuffer h_refcmp_buf, VmafCudaBuffer partials,
                                     unsigned w_horiz, unsigned w_final, unsigned h_final, float c1,
                                     float c2)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const SsimVertInputs in =
        vert_inputs(h_ref_mu_buf, h_cmp_mu_buf, h_ref_sq_buf, h_cmp_sq_buf, h_refcmp_buf);

    float my_ssim = 0.0f;
    if (x < w_final && y < h_final)
        my_ssim = ssim_from_moments(vertical_moments(in, x, y, w_horiz), c1, c2);

    /* Per-block tree reduction in shared memory. Same precision
     * pattern as ciede_cuda — partial-per-block + host double sum. */
    __shared__ float s_warp_sums[WARPS_PER_BLOCK];
    const float block_ssim = block_sum(my_ssim, s_warp_sums);
    if (is_block_leader())
        reinterpret_cast<float *>(partials.data)[block_index()] = block_ssim;
}

/* `enable_lcs` pass 2: the same SSIM value, plus L, C and S per pixel from
 * the same moments, each reduced per block. `lcs_partials` holds three rows
 * of gridDim.x * gridDim.y floats: L, then C, then S. Out-of-frame threads
 * contribute zeros. */
__launch_bounds__(BLOCK_SIZE) __global__
    void calculate_ssim_vert_combine_lcs(VmafCudaBuffer h_ref_mu_buf, VmafCudaBuffer h_cmp_mu_buf,
                                         VmafCudaBuffer h_ref_sq_buf, VmafCudaBuffer h_cmp_sq_buf,
                                         VmafCudaBuffer h_refcmp_buf, VmafCudaBuffer partials,
                                         VmafCudaBuffer lcs_partials, unsigned w_horiz,
                                         unsigned w_final, unsigned h_final, float c1, float c2)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const SsimVertInputs in =
        vert_inputs(h_ref_mu_buf, h_cmp_mu_buf, h_ref_sq_buf, h_cmp_sq_buf, h_refcmp_buf);

    float my_ssim = 0.0f;
    float lcs[LCS_TERMS] = {0.0f, 0.0f, 0.0f};
    if (x < w_final && y < h_final) {
        const SsimMoments m = vertical_moments(in, x, y, w_horiz);
        my_ssim = ssim_from_moments(m, c1, c2);
        ssim_lcs(m, c1, c2, lcs);
    }

    __shared__ float s_warp_sums[WARPS_PER_BLOCK];
    const float block_ssim = block_sum(my_ssim, s_warp_sums);
    const unsigned n_blocks = gridDim.x * gridDim.y;
    float *const lcs_out = reinterpret_cast<float *>(lcs_partials.data);
    for (int t = 0; t < LCS_TERMS; t++) {
        const float block_term = block_sum(lcs[t], s_warp_sums);
        if (is_block_leader())
            lcs_out[(unsigned)t * n_blocks + block_index()] = block_term;
    }
    if (is_block_leader())
        reinterpret_cast<float *>(partials.data)[block_index()] = block_ssim;
}

} /* extern "C" */
