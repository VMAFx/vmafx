/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  CUDA compute kernels for the float_ms_ssim feature extractor
 *  (T7-23 / batch 2 part 2b / ADR-0190). Mirror of the GLSL
 *  shaders in ms_ssim_decimate.comp + ms_ssim.comp byte-for-byte
 *  modulo language differences. Three kernels:
 *
 *    1. ms_ssim_decimate — 9-tap 9/7 biorthogonal LPF + 2×
 *       downsample, period-2n mirror boundary. Reads input
 *       float buffer, writes downsampled float buffer.
 *
 *    2. ms_ssim_horiz — horizontal 11-tap separable Gaussian over
 *       ref / cmp / ref² / cmp² / ref·cmp; same as ssim_score.cu's
 *       horizontal pass but operates on float input (already
 *       picture_copy-normalised) instead of doing the uint→float
 *       conversion inline. Pyramid scales pre-built by the
 *       decimate kernel are already float.
 *
 *    3. ms_ssim_vert_lcs — vertical 11-tap on intermediates +
 *       per-pixel l/c/s formulas + per-block float partials × 3
 *       (l, c, s). Mirrors ms_ssim.comp's vertical-with-lcs pass
 *       including the σ² ≥ 0 clamp before sqrt.
 *
 *  Host accumulates partials in `double` per scale, applies the
 *  Wang weights for the final product combine. See
 *  integer_ms_ssim_cuda.c.
 */

#include "cuda_helper.cuh"
#include "cuda/integer_ms_ssim_cuda.h"
#include "common.h"

#define BLOCK_X 16
#define BLOCK_Y 8
#define BLOCK_SIZE (BLOCK_X * BLOCK_Y)
#define K 11
#define LPF_LEN 9
#define LPF_HALF 4

extern "C" {

__device__ static const float G[K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

__device__ static const float LPF[LPF_LEN] = {
    0.026727f, -0.016828f, -0.078201f, 0.266846f, 0.602914f,
    0.266846f, -0.078201f, -0.016828f, 0.026727f,
};

/* Period-2n mirror — handles sub-kernel-radius inputs the
 * single-reflect form leaves out of bounds. Matches
 * ms_ssim_decimate_mirror() in ms_ssim_decimate.c. */
__device__ static inline int mirror_idx(int idx, int n)
{
    int period = 2 * n;
    int r = idx % period;
    if (r < 0)
        r += period;
    if (r >= n)
        r = period - r - 1;
    return r;
}

/* Decimate: 9-tap 9/7 biorthogonal separable LPF + 2× downsample.
 * One thread per output pixel. */
__global__ void ms_ssim_decimate(VmafCudaBuffer src, VmafCudaBuffer dst, unsigned w, unsigned h,
                                 unsigned w_out, unsigned h_out)
{
    const unsigned x_out = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y_out = blockIdx.y * blockDim.y + threadIdx.y;
    if (x_out >= w_out || y_out >= h_out)
        return;

    const float *src_buf = reinterpret_cast<const float *>(src.data);
    float *dst_buf = reinterpret_cast<float *>(dst.data);

    int x_src = (int)x_out * 2;
    int y_src = (int)y_out * 2;
    float acc = 0.0f;
    for (int kv = 0; kv < LPF_LEN; ++kv) {
        int yi = mirror_idx(y_src + kv - LPF_HALF, (int)h);
        float row_acc = 0.0f;
        for (int ku = 0; ku < LPF_LEN; ++ku) {
            int xi = mirror_idx(x_src + ku - LPF_HALF, (int)w);
            row_acc += src_buf[yi * (int)w + xi] * LPF[ku];
        }
        acc += row_acc * LPF[kv];
    }
    dst_buf[y_out * w_out + x_out] = acc;
}

/* SSIM horizontal pass: read float ref/cmp at (W × H), write
 * 5 horizontal-pass values at ((W-10) × H). Same shape as
 * ssim_score.cu's horiz_8bpc but operating on already-normalised
 * float input. */
__global__ void ms_ssim_horiz(VmafCudaBuffer ref_in, VmafCudaBuffer cmp_in, VmafCudaBuffer h_ref_mu,
                              VmafCudaBuffer h_cmp_mu, VmafCudaBuffer h_ref_sq,
                              VmafCudaBuffer h_cmp_sq, VmafCudaBuffer h_refcmp, unsigned width,
                              unsigned w_horiz, unsigned h_horiz)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz)
        return;

    const float *ref = reinterpret_cast<const float *>(ref_in.data);
    const float *cmp = reinterpret_cast<const float *>(cmp_in.data);

    float ref_mu_h = 0.0f, cmp_mu_h = 0.0f;
    float ref_sq_h = 0.0f, cmp_sq_h = 0.0f, refcmp_h = 0.0f;
    for (int u = 0; u < K; u++) {
        const unsigned src_idx = y * width + (x + (unsigned)u);
        const float r = ref[src_idx];
        const float c = cmp[src_idx];
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

/* Vertical pass + per-pixel l/c/s + per-block 3-output partial sums.
 * Mirrors ms_ssim.comp's main_vert_lcs byte-for-byte. */
__global__ void ms_ssim_vert_lcs(VmafCudaBuffer h_ref_mu_buf, VmafCudaBuffer h_cmp_mu_buf,
                                 VmafCudaBuffer h_ref_sq_buf, VmafCudaBuffer h_cmp_sq_buf,
                                 VmafCudaBuffer h_refcmp_buf, VmafCudaBuffer l_partials,
                                 VmafCudaBuffer c_partials, VmafCudaBuffer s_partials,
                                 unsigned w_horiz, unsigned w_final, unsigned h_final, float c1,
                                 float c2, float c3)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const float *h_ref_mu = reinterpret_cast<const float *>(h_ref_mu_buf.data);
    const float *h_cmp_mu = reinterpret_cast<const float *>(h_cmp_mu_buf.data);
    const float *h_ref_sq = reinterpret_cast<const float *>(h_ref_sq_buf.data);
    const float *h_cmp_sq = reinterpret_cast<const float *>(h_cmp_sq_buf.data);
    const float *h_refcmp = reinterpret_cast<const float *>(h_refcmp_buf.data);

    float my_l = 0.0f, my_c = 0.0f, my_s = 0.0f;
    if (x < w_final && y < h_final) {
        float ref_mu = 0.0f, cmp_mu = 0.0f, ref_sq = 0.0f, cmp_sq = 0.0f, refcmp = 0.0f;
        for (int v = 0; v < K; v++) {
            const unsigned src_idx = (y + (unsigned)v) * w_horiz + x;
            const float w = G[v];
            ref_mu += w * h_ref_mu[src_idx];
            cmp_mu += w * h_cmp_mu[src_idx];
            ref_sq += w * h_ref_sq[src_idx];
            cmp_sq += w * h_cmp_sq[src_idx];
            refcmp += w * h_refcmp[src_idx];
        }
        /* Clamp σ² ≥ 0 before sqrt — matches MAX(0, ...) in
         * iqa/ssim_tools.c::ssim_variance_scalar (line 165). */
        const float ref_var = fmaxf(ref_sq - ref_mu * ref_mu, 0.0f);
        const float cmp_var = fmaxf(cmp_sq - cmp_mu * cmp_mu, 0.0f);
        const float covar = refcmp - ref_mu * cmp_mu;
        const float sigma_xy_geom = sqrtf(ref_var * cmp_var);
        const float clamped_covar = (covar < 0.0f && sigma_xy_geom <= 0.0f) ? 0.0f : covar;

        my_l = (2.0f * ref_mu * cmp_mu + c1) / (ref_mu * ref_mu + cmp_mu * cmp_mu + c1);
        my_c = (2.0f * sigma_xy_geom + c2) / (ref_var + cmp_var + c2);
        my_s = (clamped_covar + c3) / (sigma_xy_geom + c3);
    }

    /* 3 parallel per-block tree reductions in shared memory. */
    __shared__ float s_l_warp[BLOCK_SIZE / 32];
    __shared__ float s_c_warp[BLOCK_SIZE / 32];
    __shared__ float s_s_warp[BLOCK_SIZE / 32];
    float wl = my_l, wc = my_c, ws = my_s;
    for (int off = 16; off > 0; off >>= 1) {
        wl += __shfl_down_sync(0xffffffff, wl, off);
        wc += __shfl_down_sync(0xffffffff, wc, off);
        ws += __shfl_down_sync(0xffffffff, ws, off);
    }
    const int tid = threadIdx.y * blockDim.x + threadIdx.x;
    const int lane = tid % 32;
    const int warp_id = tid / 32;
    if (lane == 0) {
        s_l_warp[warp_id] = wl;
        s_c_warp[warp_id] = wc;
        s_s_warp[warp_id] = ws;
    }
    __syncthreads();
    if (tid == 0) {
        float bl = 0.0f, bc = 0.0f, bs = 0.0f;
        for (int i = 0; i < BLOCK_SIZE / 32; i++) {
            bl += s_l_warp[i];
            bc += s_c_warp[i];
            bs += s_s_warp[i];
        }
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        reinterpret_cast<float *>(l_partials.data)[block_idx] = bl;
        reinterpret_cast<float *>(c_partials.data)[block_idx] = bc;
        reinterpret_cast<float *>(s_partials.data)[block_idx] = bs;
    }
}

} /* extern "C" */
