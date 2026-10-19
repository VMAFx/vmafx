/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  CUDA compute kernels for the real integer_ssim feature extractor
 *  (ADR-0564). Port of `libvmaf/src/feature/integer_ssim.c`: the int64
 *  moments are the CPU's and every per-pixel SSIM term is the CPU's double
 *  expression, operand for operand (this TU builds with --fmad=false,
 *  ADR-1373). The terms leave the device unreduced and the host adds them in
 *  the CPU's order, so the score is the CPU's bit for bit (ADR-1424).
 *
 *  The CPU integer_ssim uses a 9-tap Gaussian kernel with INTEGER weights
 *  (sigma=1.5, KERNEL_WEIGHT=256, kernel=[2,9,28,55,68,55,28,9,2]).
 *  Moments are accumulated as int64_t; the SSIM formula is applied in
 *  double at the final step. This is distinct from float_ssim which uses
 *  floating-point Gaussian weights in an 11-tap window.
 *
 *  Two-pass design:
 *    Pass 1 (integer_ssim_horiz_{8,16}bpc): for each pixel (x,y), compute
 *      the 5 horizontal 1-D moments (mux, muy, x2, xy, y2) and w as int64_t,
 *      using the 9-tap integer kernel with boundary-truncation (not clamping).
 *      Writes 6 int64_t arrays of size W×H.
 *
 *    Pass 2 (integer_ssim_vert_combine): for each pixel (x,y), accumulate
 *      the 5 vertical moments (plus w) from the horizontal moment arrays
 *      using the same 9-tap integer kernel with boundary-truncation.
 *      Computes the SSIM formula in double and stores the term at its
 *      raster position. The host adds the terms in raster order and divides
 *      by sum(w) to recover mean SSIM.
 *
 *  Boundary handling: mirrors the CPU ring-buffer approach — when the
 *  kernel window extends past the image boundary, the out-of-bounds taps
 *  are skipped and the per-pixel weight w reflects only in-bounds taps.
 *  This produces a per-pixel weight that varies near borders.
 *
 *  Integer kernel weights (sum=256, matching gaussian_filter_init(1.5, 5)):
 *    [2, 9, 28, 55, 68, 55, 28, 9, 2]  (kernel_len=4, kernel_sz=9)
 *
 *  The double-precision SSIM formula for a pixel:
 *    w_d = m.w (converted to double)
 *    c1  = samplemax^2 * K1^2 * w_d^2
 *    c2  = samplemax^2 * K2^2 * w_d^2
 *    mxy = m.mux * m.muy
 *    a   = 2*mxy + c1,  b = 2*(m.xy*w_d - mxy) + c2
 *    den = (m.mux^2 + m.muy^2 + c1) * (m.x2*w_d - m.mux^2 + m.y2*w_d - m.muy^2 + c2)
 *    contribution = ((m.w * a) * b) / den   (the CPU's grouping)
 *    weight = m.w
 *  Final: ssim = sum(contribution) / sum(weight)
 */

#include <cstdint>
#include "cuda_helper.cuh"

#define ISSIM_BLOCK_X 16
#define ISSIM_BLOCK_Y 8
#define ISSIM_BLOCK_SZ (ISSIM_BLOCK_X * ISSIM_BLOCK_Y)
#define ISSIM_HALF_K 4 /* kernel_len = 4, half-width */
#define ISSIM_K_SZ 9   /* kernel_sz = 2*kernel_len + 1 = 9 */

/* Integer Gaussian kernel weights (sigma=1.5, KERNEL_WEIGHT=256).
 * Computed by gaussian_filter_init in integer_ssim.c:
 *   scale = 1/(sqrt(2*pi)*1.5)
 *   w[i] = round(256 * scale * exp(-0.5/2.25 * (i-4)^2))
 *   w[4] = 256 - 2*(w[0]+w[1]+w[2]+w[3])
 * Result: [2, 9, 28, 55, 68, 55, 28, 9, 2], sum=256.
 */
namespace
{

__device__ const int32_t ISSIM_KERNEL[ISSIM_K_SZ] = {2, 9, 28, 55, 68, 55, 28, 9, 2};

/* The six vertical moments of one output pixel (CPU ssim_moments). */
struct IssimMoments {
    int64_t mux = 0LL;
    int64_t muy = 0LL;
    int64_t x2 = 0LL;
    int64_t xy = 0LL;
    int64_t y2 = 0LL;
    int64_t w = 0LL;
};

/* Vertical 9-tap accumulation over the horizontal moment arrays, with the
 * CPU's boundary truncation (out-of-plane taps skipped). */
__device__ inline IssimMoments
issim_vertical_moments(const int64_t *__restrict__ d_mux_h, const int64_t *__restrict__ d_muy_h,
                       const int64_t *__restrict__ d_x2_h, const int64_t *__restrict__ d_xy_h,
                       const int64_t *__restrict__ d_y2_h, const int64_t *__restrict__ d_w_h,
                       unsigned x, unsigned y, unsigned width, unsigned height)
{
    IssimMoments m;
    const int k_min = (int)y < ISSIM_HALF_K ? ISSIM_HALF_K - (int)y : 0;
    const int k_max = ((int)y + ISSIM_HALF_K >= (int)height) ?
                          ISSIM_K_SZ - ((int)y + ISSIM_HALF_K - (int)height + 1) :
                          ISSIM_K_SZ;
    for (int k = k_min; k < k_max; k++) {
        const int src_y = (int)y - ISSIM_HALF_K + k;
        const unsigned hidx = (unsigned)src_y * width + x;
        const int64_t vk = (int64_t)ISSIM_KERNEL[k];
        m.mux += vk * d_mux_h[hidx];
        m.muy += vk * d_muy_h[hidx];
        m.x2 += vk * d_x2_h[hidx];
        m.xy += vk * d_xy_h[hidx];
        m.y2 += vk * d_y2_h[hidx];
        m.w += vk * d_w_h[hidx];
    }
    return m;
}

/* One pixel's SSIM term in double, mirroring ssim_reduce_row_range operand
 * for operand (this TU builds with --fmad=false). The CPU groups the term as
 * ((m.w * a) * b) / den, not m.w * (a * b / den): on border pixels, where
 * m.w is not a power of two, the two round differently. Returns false for a
 * zero denominator, whose pixel then contributes neither term nor weight. */
__device__ inline bool issim_term(const IssimMoments &m, int64_t samplemax, double *term)
{
    const double w_d = (double)m.w;
    const double sm = (double)samplemax;
    const double c1 = sm * sm * 0.0001 * w_d * w_d; /* SSIM_K1=0.01, K1^2=1e-4 */
    const double c2 = sm * sm * 0.0009 * w_d * w_d; /* SSIM_K2=0.03, K2^2=9e-4 */
    const double dmux = (double)m.mux;
    const double dmuy = (double)m.muy;
    const double dx2 = (double)m.x2;
    const double dxy = (double)m.xy;
    const double dy2 = (double)m.y2;
    const double mxy = dmux * dmuy;
    const double a = 2.0 * mxy + c1;
    const double b = 2.0 * (dxy * w_d - mxy) + c2;
    const double den =
        (dmux * dmux + dmuy * dmuy + c1) * (dx2 * w_d - dmux * dmux + dy2 * w_d - dmuy * dmuy + c2);
    if (den == 0.0) {
        return false;
    }
    *term = w_d * a * b / den;
    return true;
}

} /* namespace */

/*
 * Pass 1 — horizontal 9-tap integer moment accumulation (8bpc).
 *
 * Each thread handles one (x,y) pixel. The horizontal window is
 * [x - ISSIM_HALF_K, x + ISSIM_HALF_K], clamped to [0, width-1].
 * Only in-bounds taps contribute; the per-pixel weight w accumulates
 * only those tap weights (boundary-truncation, not border-clamp).
 *
 * Writes six arrays of width*height int64_t:
 *   d_mux, d_muy, d_x2, d_xy, d_y2, d_w
 */
extern "C" {

__global__ void integer_ssim_horiz_8bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                        const uint8_t *__restrict__ cmp, ptrdiff_t cmp_stride,
                                        int64_t *__restrict__ d_mux, int64_t *__restrict__ d_muy,
                                        int64_t *__restrict__ d_x2, int64_t *__restrict__ d_xy,
                                        int64_t *__restrict__ d_y2, int64_t *__restrict__ d_w,
                                        unsigned width, unsigned height)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    int64_t mux = 0;
    int64_t muy = 0;
    int64_t x2 = 0;
    int64_t xy = 0;
    int64_t y2 = 0;
    int64_t w = 0;
    const int k_min = (int)x < ISSIM_HALF_K ? ISSIM_HALF_K - (int)x : 0;
    const int k_max = ((int)x + ISSIM_HALF_K >= (int)width) ?
                          ISSIM_K_SZ - ((int)x + ISSIM_HALF_K - (int)width + 1) :
                          ISSIM_K_SZ;

    for (int k = k_min; k < k_max; k++) {
        const int src_x = (int)x - ISSIM_HALF_K + k;
        const int64_t s = (int64_t)ref[(ptrdiff_t)y * ref_stride + src_x];
        const int64_t d = (int64_t)cmp[(ptrdiff_t)y * cmp_stride + src_x];
        const int64_t wk = (int64_t)ISSIM_KERNEL[k];
        mux += wk * s;
        muy += wk * d;
        x2 += wk * s * s;
        xy += wk * s * d;
        y2 += wk * d * d;
        w += wk;
    }

    const unsigned idx = y * width + x;
    d_mux[idx] = mux;
    d_muy[idx] = muy;
    d_x2[idx] = x2;
    d_xy[idx] = xy;
    d_y2[idx] = y2;
    d_w[idx] = w;
}

/*
 * Pass 1 — horizontal 9-tap integer moment accumulation (>8bpc).
 * Same as above but reads uint16_t pixels (raw, not normalised — the
 * SSIM formula uses raw integer values; normalisation is baked into
 * samplemax in the SSIM formula, not in the pixels themselves).
 */
__global__ void integer_ssim_horiz_16bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                         const uint8_t *__restrict__ cmp, ptrdiff_t cmp_stride,
                                         int64_t *__restrict__ d_mux, int64_t *__restrict__ d_muy,
                                         int64_t *__restrict__ d_x2, int64_t *__restrict__ d_xy,
                                         int64_t *__restrict__ d_y2, int64_t *__restrict__ d_w,
                                         unsigned width, unsigned height)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    int64_t mux = 0;
    int64_t muy = 0;
    int64_t x2 = 0;
    int64_t xy = 0;
    int64_t y2 = 0;
    int64_t w = 0;
    const int k_min = (int)x < ISSIM_HALF_K ? ISSIM_HALF_K - (int)x : 0;
    const int k_max = ((int)x + ISSIM_HALF_K >= (int)width) ?
                          ISSIM_K_SZ - ((int)x + ISSIM_HALF_K - (int)width + 1) :
                          ISSIM_K_SZ;

    for (int k = k_min; k < k_max; k++) {
        const int src_x = (int)x - ISSIM_HALF_K + k;
        const int64_t s =
            (int64_t)(reinterpret_cast<const uint16_t *>(ref + (ptrdiff_t)y * ref_stride))[src_x];
        const int64_t d =
            (int64_t)(reinterpret_cast<const uint16_t *>(cmp + (ptrdiff_t)y * cmp_stride))[src_x];
        const int64_t wk = (int64_t)ISSIM_KERNEL[k];
        mux += wk * s;
        muy += wk * d;
        x2 += wk * s * s;
        xy += wk * s * d;
        y2 += wk * d * d;
        w += wk;
    }

    const unsigned idx = y * width + x;
    d_mux[idx] = mux;
    d_muy[idx] = muy;
    d_x2[idx] = x2;
    d_xy[idx] = xy;
    d_y2[idx] = y2;
    d_w[idx] = w;
}

/*
 * Pass 2 — vertical 9-tap integer moment accumulation + SSIM formula.
 *
 * Each thread handles one (x,y) pixel. The vertical window is
 * [y - ISSIM_HALF_K, y + ISSIM_HALF_K], clamped to [0, height-1].
 * Accumulates int64_t moments from the horizontal arrays, then
 * computes the SSIM contribution in double and stores it at its raster
 * position in `terms`. The terms are not reduced here: the CPU adds them
 * into one double, left to right and top to bottom, and a sum in any other
 * order rounds differently, so the host adds the plane it reads back
 * (ADR-1424).
 *
 * The weights are integers, so their sum does not depend on the order and
 * stays a block reduction. It uses warp_reduce(int64_t) from
 * cuda_helper.cuh. CUDA has no int64 shuffle intrinsic, so each step
 * shuffles the two 32-bit halves, and they must be reassembled into an int64
 * before adding. An earlier copy summed `lo` and `hi` as two int32
 * accumulators and recombined only at the end, dropping the carry out of the
 * low half and able to overflow `lo` (undefined behaviour). Not reachable
 * today (the per-pixel weight sum over a 9-tap window stays far below 2^31),
 * but wrong; keep the shared helper rather than a second, subtly different
 * copy. ADR-1224.
 *
 * Writes one double per pixel into `terms` (width * height, raster order).
 * Writes one int64_t per block into `partial_weights`.
 * Host: ssim = sum(terms in raster order) / sum(partial_weights).
 */
__global__ void
integer_ssim_vert_combine(const int64_t *__restrict__ d_mux_h, const int64_t *__restrict__ d_muy_h,
                          const int64_t *__restrict__ d_x2_h, const int64_t *__restrict__ d_xy_h,
                          const int64_t *__restrict__ d_y2_h, const int64_t *__restrict__ d_w_h,
                          double *__restrict__ terms, int64_t *__restrict__ partial_weights,
                          unsigned width, unsigned height, int64_t samplemax)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;

    int64_t my_weight = 0LL;

    if (x < width && y < height) {
        const IssimMoments m = issim_vertical_moments(d_mux_h, d_muy_h, d_x2_h, d_xy_h, d_y2_h,
                                                      d_w_h, x, y, width, height);
        double term = 0.0;
        if (issim_term(m, samplemax, &term)) {
            my_weight = m.w;
        }
        terms[(size_t)y * width + x] = term;
    }

    /* Per-block int64 weight reduction; see the function comment. */
    __shared__ int64_t s_wgt[ISSIM_BLOCK_SZ / 32];
    const int64_t warp_wgt = warp_reduce(my_weight);

    const int tid = threadIdx.y * (int)blockDim.x + threadIdx.x;
    const int lane = tid % 32;
    const int warp_id = tid / 32;
    if (lane == 0) {
        s_wgt[warp_id] = warp_wgt;
    }
    __syncthreads();

    if (tid == 0) {
        int64_t block_wgt = 0LL;
        for (const int64_t wgt : s_wgt) {
            block_wgt += wgt;
        }
        const unsigned blk = blockIdx.y * gridDim.x + blockIdx.x;
        partial_weights[blk] = block_wgt;
    }
}
} /* extern "C" */
