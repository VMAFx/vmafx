/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  HIP compute kernels for the float_ms_ssim feature extractor
 *  (integer_ms_ssim_hip, ADR-0285). Three kernels:
 *
 *    1. ms_ssim_decimate — 9-tap 9/7 biorthogonal LPF + 2x downsample,
 *       period-2n mirror boundary. One thread per output sample.
 *
 *    2. ms_ssim_horiz — horizontal 11-tap Gaussian over ref / cmp / ref^2 /
 *       cmp^2 / ref*cmp of one pyramid level. Output: five float planes,
 *       each (W-10) x H.
 *
 *    3. ms_ssim_vert_lcs — vertical 11-tap over those planes, the l / c / s
 *       terms of each sample, and one (l, c, s) sum per block. Grid sized
 *       over (W-10) x (H-10).
 *
 *  Numerical contract (ADR-1403, ADR-1407). The arithmetic of one sample is
 *  in ms_ssim_arith.h and is the CPU extractor's, operation for operation:
 *  fused decimate taps (ms_ssim_decimate.c), fp32 products summed in fp64
 *  and rounded once per pass (iqa_convolve()), and the mixed fp32 / fp64
 *  l / c / s of ssim_accumulate_default_scalar(). The HSACO builds with
 *  `hip_strict_fp_args`, so nothing is contracted and fp32 division and
 *  square root are correctly rounded. The one thing that differs from the
 *  reference is the order of the fp64 l / c / s sums (lanes, then waves,
 *  then blocks, where the CPU adds in raster order); the host rounds each
 *  per-scale mean to fp32 as iqa_ssim() does, which absorbs it.
 *
 *  HIP specifics:
 *  - The wave size varies: 64 on GCN / RDNA1, 32 on RDNA2+. `warpSize` is
 *    read at run time; shared memory is sized for the most waves per block
 *    (wave32: BLOCK_SIZE / MIN_WARP_SIZE = 4 slots).
 *  - Raw pointer parameters, `__shfl_down` without a mask.
 */

#include <hip/hip_runtime.h>

#include "ms_ssim_arith.h"

#define BLOCK_X 16
#define BLOCK_Y 8
#define BLOCK_SIZE (BLOCK_X * BLOCK_Y)

/* Minimum HIP wave size across supported targets (32 on RDNA2+, 64 on
 * GCN / RDNA1). The reduction reads the run-time `warpSize`, so one binary
 * serves both. */
#define MIN_WARP_SIZE 32
#define WARPS_PER_BLOCK (BLOCK_SIZE / MIN_WARP_SIZE)

extern "C" {

/* Decimate: one thread per output sample of ms_ssim_decimate().
 * src: float input (w x h), dst: float output (w_out x h_out). */
__global__ void ms_ssim_decimate(const float *__restrict__ src, float *__restrict__ dst,
                                 unsigned w, unsigned h, unsigned w_out, unsigned h_out)
{
    const unsigned x_out = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y_out = blockIdx.y * blockDim.y + threadIdx.y;
    if (x_out >= w_out || y_out >= h_out)
        return;
    dst[(size_t)y_out * w_out + x_out] =
        vmaf_hip_ms_ssim_decimate_sample(src, (int)w, (int)h, (int)x_out, (int)y_out);
}

/* Horizontal pass: reads float ref / cmp at (W x H), writes the five window
 * statistics at ((W-10) x H). */
__global__ void ms_ssim_horiz(const float *__restrict__ ref_in, const float *__restrict__ cmp_in,
                              float *__restrict__ h_ref_mu, float *__restrict__ h_cmp_mu,
                              float *__restrict__ h_ref_sq, float *__restrict__ h_cmp_sq,
                              float *__restrict__ h_refcmp, unsigned width, unsigned w_horiz,
                              unsigned h_horiz)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz)
        return;

    const size_t src_idx = (size_t)y * width + x;
    const VmafHipMsWindow sums = vmaf_hip_ms_ssim_horizontal(ref_in + src_idx, cmp_in + src_idx);
    const size_t dst_idx = (size_t)y * w_horiz + x;
    h_ref_mu[dst_idx] = sums.ref_mu;
    h_cmp_mu[dst_idx] = sums.cmp_mu;
    h_ref_sq[dst_idx] = sums.ref_sq;
    h_cmp_sq[dst_idx] = sums.cmp_sq;
    h_refcmp[dst_idx] = sums.refcmp;
}

/* Vertical pass, the l / c / s terms of each sample and one (l, c, s) sum
 * per block, written to l_partials / c_partials / s_partials at
 * [blockIdx.y * gridDim.x + blockIdx.x]. Grid sized over
 * w_final x h_final = (W-10) x (H-10).
 *
 * c1..c3 arrive as doubles holding iqa_ssim()'s fp32 constants exactly
 * (the launch argument list predates ADR-1403) and are narrowed back
 * without loss. The lane, wave and block sums run in double, like the
 * CPU's l / c / s sums. */
__global__ void ms_ssim_vert_lcs(const float *__restrict__ h_ref_mu_buf,
                                 const float *__restrict__ h_cmp_mu_buf,
                                 const float *__restrict__ h_ref_sq_buf,
                                 const float *__restrict__ h_cmp_sq_buf,
                                 const float *__restrict__ h_refcmp_buf,
                                 double *__restrict__ l_partials, double *__restrict__ c_partials,
                                 double *__restrict__ s_partials, unsigned w_horiz,
                                 unsigned w_final, unsigned h_final, double c1, double c2,
                                 double c3)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    double my_l = 0.0, my_c = 0.0, my_s = 0.0;
    if (x < w_final && y < h_final) {
        const VmafHipMsPlanes planes = {h_ref_mu_buf, h_cmp_mu_buf, h_ref_sq_buf, h_cmp_sq_buf,
                                        h_refcmp_buf};
        const VmafHipMsWindow stats =
            vmaf_hip_ms_ssim_vertical(&planes, (size_t)y * w_horiz + x, w_horiz);
        const VmafHipMsLcs terms = vmaf_hip_ms_ssim_lcs(&stats, (float)c1, (float)c2, (float)c3);
        my_l = terms.l;
        my_c = terms.c;
        my_s = terms.s;
    }

    /* Three per-block tree reductions: lanes of a wave by shuffle, then the
     * waves through shared memory. */
    __shared__ double s_l_warp[WARPS_PER_BLOCK];
    __shared__ double s_c_warp[WARPS_PER_BLOCK];
    __shared__ double s_s_warp[WARPS_PER_BLOCK];

    double wl = my_l, wc = my_c, ws = my_s;
    for (int off = (int)warpSize / 2; off > 0; off >>= 1) {
        wl += __shfl_down(wl, off);
        wc += __shfl_down(wc, off);
        ws += __shfl_down(ws, off);
    }

    const unsigned lane = lid % (unsigned)warpSize;
    const unsigned warp_id = lid / (unsigned)warpSize;
    if (lane == 0) {
        s_l_warp[warp_id] = wl;
        s_c_warp[warp_id] = wc;
        s_s_warp[warp_id] = ws;
    }
    __syncthreads();

    if (lid == 0) {
        double bl = 0.0, bc = 0.0, bs = 0.0;
        const int nwarps = (int)(blockDim.x * blockDim.y) / (int)warpSize;
        for (int i = 0; i < nwarps; i++) {
            bl += s_l_warp[i];
            bc += s_c_warp[i];
            bs += s_s_warp[i];
        }
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        l_partials[block_idx] = bl;
        c_partials[block_idx] = bc;
        s_partials[block_idx] = bs;
    }
}

} /* extern "C" */
