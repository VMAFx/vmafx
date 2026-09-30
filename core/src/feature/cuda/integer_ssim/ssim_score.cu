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
 *       five intermediates, then the CPU's per-pixel l * c * s with the
 *       CPU's types and rounding points (ssim_terms()), then a per-block
 *       double partial sum (tree reduce in shared memory).
 *       calculate_ssim_vert_combine_lcs is the `enable_lcs` variant:
 *       the same SSIM value plus the per-pixel luminance, contrast and
 *       structure terms, reduced per block next to it (ADR-1373).
 *
 *  The host sums the double partials, divides by (W-10)·(H-10) and rounds
 *  the mean to fp32, as iqa_ssim() returns it.
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

/* One pixel's SSIM and its luminance, contrast and structure terms. */
struct SsimTerms {
    double ssim;
    double l;
    double c;
    double s;
};

/* The CPU default path, operand for operand: iqa/ssim_tools.c::
 * ssim_variance_scalar (both variances clamped at zero, the covariance not)
 * and iqa/ssim_accumulate_lane.h (ssim_accumulate_scalar_step +
 * ssim_accumulate_lane), which every CPU SIMD variant shares. The types are
 * the CPU's and each rounding is spelled with an intrinsic so NVCC cannot
 * fuse it:
 *
 *   l = (2.0 * mu_r * mu_c + C1) / (float)(mu_r^2 + mu_c^2 + C1)
 *   c = (2.0 * sqrtf(var_r * var_c) + C2) / (float)(var_r + var_c + C2)
 *   s = (float)((cov' + C3) / (sqrtf(var_r * var_c) + C3))
 *   ssim = l * c * s                                (double)
 *
 * with double numerators over fp32-rounded denominators, cov' the
 * covariance clamped to zero on a flat window, and C3 = C2 / 2. The CPU
 * does not score identical windows exactly 1: a flat window's l is
 * 1 - 4.3e-8 because the fp32 denominator rounds and the double numerator
 * does not, so identical flat frames report 72.247 dB, not +inf (ADR-1373). */
__device__ inline SsimTerms ssim_terms(const SsimMoments &m, float c1, float c2)
{
    const float c3 = __fdiv_rn(c2, 2.0f);
    const float ref_raw = __fsub_rn(m.ref_sq, __fmul_rn(m.ref_mu, m.ref_mu));
    const float cmp_raw = __fsub_rn(m.cmp_sq, __fmul_rn(m.cmp_mu, m.cmp_mu));
    const float ref_var = ref_raw < 0.0f ? 0.0f : ref_raw;
    const float cmp_var = cmp_raw < 0.0f ? 0.0f : cmp_raw;
    const float sigma_both = __fsub_rn(m.refcmp, __fmul_rn(m.ref_mu, m.cmp_mu));

    const float srsc = __fsqrt_rn(__fmul_rn(ref_var, cmp_var));
    const float l_den =
        __fadd_rn(__fadd_rn(__fmul_rn(m.ref_mu, m.ref_mu), __fmul_rn(m.cmp_mu, m.cmp_mu)), c1);
    const float c_den = __fadd_rn(__fadd_rn(ref_var, cmp_var), c2);
    const float csb = (sigma_both < 0.0f && srsc <= 0.0f) ? 0.0f : sigma_both;
    const float sv_f = __fdiv_rn(__fadd_rn(csb, c3), __fadd_rn(srsc, c3));

    SsimTerms t;
    t.l = __ddiv_rn(
        __dadd_rn(__dmul_rn(__dmul_rn(2.0, (double)m.ref_mu), (double)m.cmp_mu), (double)c1),
        (double)l_den);
    t.c = __ddiv_rn(__dadd_rn(__dmul_rn(2.0, (double)srsc), (double)c2), (double)c_den);
    t.s = (double)sv_f;
    t.ssim = __dmul_rn(__dmul_rn(t.l, t.c), t.s);
    return t;
}

/* Sum `value` over the block into thread 0's return value: warp shuffle,
 * then the warp sums in warp order. Double throughout, as the CPU's
 * accumulators are. Every thread must call it; `scratch` is reusable once
 * it returns. */
__device__ inline double block_sum(double value, double (&scratch)[WARPS_PER_BLOCK])
{
    for (int off = 16; off > 0; off >>= 1)
        value += __shfl_down_sync(0xffffffff, value, off);
    const int tid = threadIdx.y * blockDim.x + threadIdx.x;
    if (tid % 32 == 0)
        scratch[tid / 32] = value;
    __syncthreads();
    double total = 0.0;
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

/* Pass 2 — vertical + SSIM combine + per-block double partial sum.
 * __launch_bounds__(128) hints nvcc to budget registers for
 * 128-thread blocks; per ADR-0754 / ADR-0743 precedent. `partials` holds
 * gridDim.x * gridDim.y doubles. */
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

    double my_ssim = 0.0;
    if (x < w_final && y < h_final)
        my_ssim = ssim_terms(vertical_moments(in, x, y, w_horiz), c1, c2).ssim;

    __shared__ double s_warp_sums[WARPS_PER_BLOCK];
    const double block_ssim = block_sum(my_ssim, s_warp_sums);
    if (is_block_leader())
        reinterpret_cast<double *>(partials.data)[block_index()] = block_ssim;
}

/* `enable_lcs` pass 2: the same SSIM value, plus its L, C and S terms, each
 * reduced per block. `lcs_partials` holds three rows of
 * gridDim.x * gridDim.y doubles: L, then C, then S. Out-of-frame threads
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

    SsimTerms t = {0.0, 0.0, 0.0, 0.0};
    if (x < w_final && y < h_final)
        t = ssim_terms(vertical_moments(in, x, y, w_horiz), c1, c2);

    __shared__ double s_warp_sums[WARPS_PER_BLOCK];
    const double block_ssim = block_sum(t.ssim, s_warp_sums);
    const double block_l = block_sum(t.l, s_warp_sums);
    const double block_c = block_sum(t.c, s_warp_sums);
    const double block_s = block_sum(t.s, s_warp_sums);
    if (is_block_leader()) {
        const unsigned n_blocks = gridDim.x * gridDim.y;
        double *const lcs_out = reinterpret_cast<double *>(lcs_partials.data);
        lcs_out[block_index()] = block_l;
        lcs_out[n_blocks + block_index()] = block_c;
        lcs_out[(2u * n_blocks) + block_index()] = block_s;
        reinterpret_cast<double *>(partials.data)[block_index()] = block_ssim;
    }
}

} /* extern "C" */
