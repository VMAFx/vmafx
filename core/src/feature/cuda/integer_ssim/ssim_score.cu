/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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
#define K 11

extern "C" {

__device__ static const float G[K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

/* Read normalised float from the picture's luma plane. Mirrors
 * picture_copy / picture_copy_hbd: 8bpc → uint8 cast, 10/12/16bpc
 * → uint16 / scaler where scaler = 4 / 16 / 256. */
__device__ static inline float read_norm_8bpc(const VmafPicture &pic, unsigned x, unsigned y)
{
    const uint8_t *row = reinterpret_cast<const uint8_t *>(pic.data[0]) + y * pic.stride[0];
    return (float)row[x];
}

__device__ static inline float read_norm_16bpc(const VmafPicture &pic, unsigned x, unsigned y,
                                               float scaler)
{
    const uint16_t *row = reinterpret_cast<const uint16_t *>(
        reinterpret_cast<const uint8_t *>(pic.data[0]) + y * pic.stride[0]);
    return (float)row[x] / scaler;
}

__device__ static inline float scaler_for_bpc(unsigned bpc)
{
    if (bpc == 10)
        return 4.0f;
    if (bpc == 12)
        return 16.0f;
    if (bpc == 16)
        return 256.0f;
    return 1.0f;
}

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

/* Pass 2 — vertical + SSIM combine + per-block partial sum. */
__global__ void
calculate_ssim_vert_combine(VmafCudaBuffer h_ref_mu_buf, VmafCudaBuffer h_cmp_mu_buf,
                            VmafCudaBuffer h_ref_sq_buf, VmafCudaBuffer h_cmp_sq_buf,
                            VmafCudaBuffer h_refcmp_buf, VmafCudaBuffer partials, unsigned w_horiz,
                            unsigned w_final, unsigned h_final, float c1, float c2)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const float *h_ref_mu = reinterpret_cast<const float *>(h_ref_mu_buf.data);
    const float *h_cmp_mu = reinterpret_cast<const float *>(h_cmp_mu_buf.data);
    const float *h_ref_sq = reinterpret_cast<const float *>(h_ref_sq_buf.data);
    const float *h_cmp_sq = reinterpret_cast<const float *>(h_cmp_sq_buf.data);
    const float *h_refcmp = reinterpret_cast<const float *>(h_refcmp_buf.data);

    float my_ssim = 0.0f;
    if (x < w_final && y < h_final) {
        float ref_mu = 0.0f, cmp_mu = 0.0f, ref_sq = 0.0f, cmp_sq = 0.0f, refcmp = 0.0f;
        for (int v = 0; v < K; v++) {
            const unsigned src_y = y + (unsigned)v;
            const unsigned src_idx = src_y * w_horiz + x;
            const float w = G[v];
            ref_mu += w * h_ref_mu[src_idx];
            cmp_mu += w * h_cmp_mu[src_idx];
            ref_sq += w * h_ref_sq[src_idx];
            cmp_sq += w * h_cmp_sq[src_idx];
            refcmp += w * h_refcmp[src_idx];
        }
        const float ref_var = ref_sq - ref_mu * ref_mu;
        const float cmp_var = cmp_sq - cmp_mu * cmp_mu;
        const float covar = refcmp - ref_mu * cmp_mu;
        const float mu_xy = ref_mu * cmp_mu;
        const float num = (2.0f * mu_xy + c1) * (2.0f * covar + c2);
        const float den = (ref_mu * ref_mu + cmp_mu * cmp_mu + c1) * (ref_var + cmp_var + c2);
        my_ssim = num / den;
    }

    /* Per-block tree reduction in shared memory. Same precision
     * pattern as ciede_cuda — partial-per-block + host double sum. */
    __shared__ float s_warp_sums[BLOCK_SIZE / 32];
    float warp_sum = my_ssim;
    for (int off = 16; off > 0; off >>= 1)
        warp_sum += __shfl_down_sync(0xffffffff, warp_sum, off);
    const int tid = threadIdx.y * blockDim.x + threadIdx.x;
    const int lane = tid % 32;
    const int warp_id = tid / 32;
    if (lane == 0)
        s_warp_sums[warp_id] = warp_sum;
    __syncthreads();
    if (tid == 0) {
        float block_sum = 0.0f;
        for (int i = 0; i < BLOCK_SIZE / 32; i++)
            block_sum += s_warp_sums[i];
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        reinterpret_cast<float *>(partials.data)[block_idx] = block_sum;
    }
}

} /* extern "C" */
