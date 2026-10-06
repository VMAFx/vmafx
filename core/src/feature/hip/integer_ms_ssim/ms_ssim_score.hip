/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  HIP compute kernels for the float_ms_ssim feature extractor
 *  (integer_ms_ssim_hip, ADR-0285). Four kernels:
 *
 *    0. ms_ssim_picture_to_float — level 0 of a pyramid from a plane in HIP
 *       device memory (a frame imported through the VMAFx API, ADR-2092):
 *       float_ms_ssim.c's picture_copy() of the plane on the device, so the
 *       plane never goes through the host. Host pictures are still converted
 *       by picture_copy() on the host and uploaded.
 *
 *    1. ms_ssim_decimate — 9-tap 9/7 biorthogonal LPF + 2x downsample,
 *       period-2n mirror boundary. One thread per output sample.
 *
 *    2. ms_ssim_horiz — horizontal 11-tap Gaussian over ref / cmp / ref^2 /
 *       cmp^2 / ref*cmp of one pyramid level. Output: five float planes,
 *       each (W-10) x H.
 *
 *    3. ms_ssim_vert_lcs — vertical 11-tap over those planes and the l / c / s
 *       terms of each window, stored at the window's raster position. Grid
 *       sized over (W-10) x (H-10).
 *
 *  Numerical contract (ADR-1403, ADR-1407). The arithmetic of one sample is
 *  in ms_ssim_arith.h and is the CPU extractor's, operation for operation:
 *  fused decimate taps (ms_ssim_decimate.c), fp32 products summed in fp64
 *  and rounded once per pass (iqa_convolve()), and the mixed fp32 / fp64
 *  l / c / s of ssim_accumulate_default_scalar(). The HSACO builds with
 *  `hip_strict_fp_args`, so nothing is contracted and fp32 division and
 *  square root are correctly rounded.
 *
 *  The fp64 l / c / s sums of a scale are not formed here. The reference adds
 *  the windows in raster order into one double per sum, every add rounds,
 *  and a sum of the same terms per lane, wave and block is another double:
 *  on the pair of core/test/float_ms_ssim_order_frame.h its mean rounded to
 *  the neighbouring float (T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02). The
 *  kernel therefore stores every term and the host adds them in that order,
 *  the construction of ADR-1438.
 *
 *  HIP specifics: raw pointer parameters.
 */

#include <hip/hip_runtime.h>

#include "ms_ssim_arith.h"

extern "C" {

/* picture_copy() (core/src/feature/picture_copy.cpp) of one plane, offset 0:
 * a 16-bit sample divided by `scaler` (4, 16 or 256 at 10, 12 and 16 bits),
 * any other depth read a byte at a time as the reference does. A division by
 * a power of two and a conversion: exact, so bit for bit the host's value.
 * `src` is the plane (`pitch` bytes per row), `dst` the packed level 0. */
__global__ void ms_ssim_picture_to_float(const unsigned char *__restrict__ src, size_t pitch,
                                         float *__restrict__ dst, unsigned w, unsigned h,
                                         unsigned two_byte, float scaler)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h)
        return;
    const unsigned char *const row = src + (size_t)y * pitch;
    float *const out = dst + (size_t)y * w;
    if (two_byte) {
        const size_t at = (size_t)2u * x;
        const unsigned v = (unsigned)row[at] | ((unsigned)row[at + 1u] << 8u);
        out[x] = (float)v / scaler;
    } else {
        out[x] = (float)row[x];
    }
}

/* Decimate: one thread per output sample of ms_ssim_decimate().
 * src: float input (w x h), dst: float output (w_out x h_out). */
__global__ void ms_ssim_decimate(const float *__restrict__ src, float *__restrict__ dst, unsigned w,
                                 unsigned h, unsigned w_out, unsigned h_out)
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

/* Vertical pass and the l / c / s terms of each window. `terms` holds three
 * planes of w_final * h_final doubles laid out [l | c | s], each in raster
 * order; nothing is reduced on the device, the host adds each plane in that
 * order. Grid sized over w_final x h_final = (W-10) x (H-10).
 *
 * c1..c3 arrive as doubles holding iqa_ssim()'s fp32 constants exactly
 * (the launch argument list predates ADR-1403) and are narrowed back
 * without loss. */
__global__ void ms_ssim_vert_lcs(const float *__restrict__ h_ref_mu_buf,
                                 const float *__restrict__ h_cmp_mu_buf,
                                 const float *__restrict__ h_ref_sq_buf,
                                 const float *__restrict__ h_cmp_sq_buf,
                                 const float *__restrict__ h_refcmp_buf, double *__restrict__ terms,
                                 unsigned w_horiz, unsigned w_final, unsigned h_final, double c1,
                                 double c2, double c3)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_final || y >= h_final)
        return;

    const VmafHipMsPlanes planes = {h_ref_mu_buf, h_cmp_mu_buf, h_ref_sq_buf, h_cmp_sq_buf,
                                    h_refcmp_buf};
    const VmafHipMsWindow stats =
        vmaf_hip_ms_ssim_vertical(&planes, (size_t)y * w_horiz + x, w_horiz);
    const VmafHipMsLcs lcs = vmaf_hip_ms_ssim_lcs(&stats, (float)c1, (float)c2, (float)c3);
    const size_t windows = (size_t)w_final * h_final;
    const size_t window = (size_t)y * w_final + x;
    terms[window] = lcs.l;
    terms[windows + window] = lcs.c;
    terms[2u * windows + window] = lcs.s;
}

} /* extern "C" */
