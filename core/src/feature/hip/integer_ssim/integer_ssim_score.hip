/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  HIP compute kernels for the integer_ssim feature extractor (ADR-0564).
 *  Port of the CUDA twin `cuda/integer_ssim/integer_ssim_score.cu`, which in
 *  turn ports `calc_ssim()` from the CPU reference `integer_ssim.c`.
 *
 *  The CPU algorithm, and therefore this one:
 *    - a 9-tap Gaussian with INTEGER weights [2,9,28,55,68,55,28,9,2]
 *      (`gaussian_filter_init(1.5, 5)`, KERNEL_WEIGHT = 256);
 *    - int64 accumulation of the moments mux, muy, x2, xy, y2 and the
 *      window weight w;
 *    - boundary truncation: taps that fall outside the frame are skipped
 *      and w counts only the in-bounds taps (no clamping, no mirroring);
 *    - the SSIM term of each pixel in double, from the int64 moments;
 *    - ssim = sum(term) / sum(w) over every pixel of the frame.
 *
 *  Two passes, both over the full W x H frame:
 *    1. integer_ssim_horiz_{8,16}bpc — horizontal 9-tap moments of one row
 *       into six W x H int64 planes (mux, muy, x2, xy, y2, w).
 *    2. the vertical 9-tap over those planes, the per-pixel SSIM term, and
 *       the reduction, in one of two forms the host picks by frame size:
 *       - integer_ssim_vert_combine — one (double term sum, int64 weight
 *         sum) pair per block; the host adds the block pairs.
 *       - integer_ssim_vert_terms — one (double term, int64 weight) pair
 *         per pixel, unreduced, for frames of at most
 *         ISSIM_HIP_RASTER_MAX_PIXELS pixels; the host adds them in raster
 *         order (ADR-1400).
 *  The host divides the term sum by the weight sum.
 *
 *  Exactness against the CPU:
 *    - The moments are int64 sums of int64 products, so they are exact and
 *      identical to the CPU's.
 *    - `issim_cpu_term` evaluates the expression of
 *      `ssim_reduce_row_range()` operand for operand, and meson builds this
 *      file with `-ffp-contract=off` (`hip_strict_fp_args`), so no product
 *      is fused into an FMA and each per-pixel term rounds exactly as the
 *      CPU's does.
 *    - The per-pixel form leaves the sum to the host, which adds the terms
 *      in the CPU's order, so the frame score is the CPU's double. That matters most on identical frames:
 *      the CPU's quotient on an identical window is the weight up to an ulp,
 *      and whether its running sum absorbs the ulp depends on the frame (an
 *      identical 1x1 frame of zeros scores 1 - 2^-52, 156.54 dB with
 *      `enable_db`).
 *    - The per-block form differs in the summation order only: the CPU adds
 *      the terms in raster order into one running double, the kernel adds
 *      them in a per-block tree and the host adds the blocks. A tree does
 *      not absorb the ulp of an identical window, so there the term is the
 *      weight exactly (`issim_pixel_term`, ADR-1382) and an identical frame
 *      scores exactly 1, which is what the CPU reports on all but a few in
 *      a million identical frames of that size (Research-1400). The weights
 *      are integers and add exactly on both sides.
 *
 *  HIP adaptation from the CUDA twin: the block reduction is a shared-memory
 *  tree over all ISSIM_BLOCK_SZ threads instead of a 32-lane
 *  `__shfl_down_sync` reduction, so it is independent of the wavefront size
 *  (wave32 on RDNA, wave64 on GCN / CDNA) and adds in the same order on
 *  every target.
 */

#include <hip/hip_runtime.h>

#include <cstddef>
#include <cstdint>

/* Launch geometry. integer_ssim_hip.c launches every kernel here with
 * ISSIM_HIP_BLOCK_X x ISSIM_HIP_BLOCK_Y threads; the two must agree, the
 * shared-memory reduction below is sized for exactly that block. */
#define ISSIM_BLOCK_X 16
#define ISSIM_BLOCK_Y 8
#define ISSIM_BLOCK_SZ (ISSIM_BLOCK_X * ISSIM_BLOCK_Y)

/* kernel_len = 4 and kernel_sz = 2 * kernel_len + 1 = 9 in calc_ssim(). */
#define ISSIM_HALF_K 4
#define ISSIM_K_SZ 9

/* SSIM_K1 / SSIM_K2 of integer_ssim.c, spelled the same way so the constant
 * folds to the same double. The literals 0.0001 / 0.0009 are different
 * doubles. */
#define ISSIM_K1 (0.01 * 0.01)
#define ISSIM_K2 (0.03 * 0.03)

/* gaussian_filter_init(1.5, 5) with KERNEL_WEIGHT 256: the outer taps are
 * round(256 * exp(-i^2 / 4.5) / (1.5 * sqrt(2 * pi))) and the centre tap is
 * 256 minus the rest, giving a sum of exactly 256. */
__device__ static const int32_t ISSIM_KERNEL[ISSIM_K_SZ] = {2, 9, 28, 55, 68, 55, 28, 9, 2};

namespace
{

/* The taps [k_min, k_max) of the window centred on `pos` that fall inside
 * [0, len): the k_min / k_max formulas of ssim_accumulate_row() and
 * calc_ssim(). */
__device__ __forceinline__ int issim_tap_min(int pos)
{
    return pos < ISSIM_HALF_K ? ISSIM_HALF_K - pos : 0;
}

__device__ __forceinline__ int issim_tap_max(int pos, int len)
{
    return pos + ISSIM_HALF_K >= len ? ISSIM_K_SZ - (pos + ISSIM_HALF_K - len + 1) : ISSIM_K_SZ;
}

/* Pass 1 body, shared by the 8-bit and the 16-bit entry point. Reads one
 * packed row of `Sample` values and writes the six horizontal moments of
 * pixel (x, y). */
template <typename Sample>
__device__ __forceinline__ void
issim_horiz(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride, const uint8_t *__restrict__ cmp,
            ptrdiff_t cmp_stride, int64_t *__restrict__ d_mux, int64_t *__restrict__ d_muy,
            int64_t *__restrict__ d_x2, int64_t *__restrict__ d_xy, int64_t *__restrict__ d_y2,
            int64_t *__restrict__ d_w, unsigned width, unsigned height)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;

    const Sample *ref_row = reinterpret_cast<const Sample *>(ref + (ptrdiff_t)y * ref_stride);
    const Sample *cmp_row = reinterpret_cast<const Sample *>(cmp + (ptrdiff_t)y * cmp_stride);
    const int k_min = issim_tap_min((int)x);
    const int k_max = issim_tap_max((int)x, (int)width);

    int64_t mux = 0;
    int64_t muy = 0;
    int64_t x2 = 0;
    int64_t xy = 0;
    int64_t y2 = 0;
    int64_t w = 0;
    for (int k = k_min; k < k_max; k++) {
        const int src_x = (int)x - ISSIM_HALF_K + k;
        const int64_t s = ref_row[src_x];
        const int64_t d = cmp_row[src_x];
        const int64_t wk = ISSIM_KERNEL[k];
        mux += wk * s;
        muy += wk * d;
        x2 += wk * s * s;
        xy += wk * s * d;
        y2 += wk * d * d;
        w += wk;
    }

    const size_t idx = (size_t)y * width + x;
    d_mux[idx] = mux;
    d_muy[idx] = muy;
    d_x2[idx] = x2;
    d_xy[idx] = xy;
    d_y2[idx] = y2;
    d_w[idx] = w;
}

/* The six moments of one window. */
struct IssimMoments {
    int64_t mux;
    int64_t muy;
    int64_t x2;
    int64_t xy;
    int64_t y2;
    int64_t w;
};

/* The four factors of one pixel's SSIM term and its window weight. */
struct IssimFactors {
    double w_d;
    double lum_num;
    double cs_num;
    double lum_den;
    double cs_den;
};

/* Vertical 9-tap of pixel (x, y) over the pass-1 planes: the moment loop of
 * ssim_reduce_row_range(). Taps outside the frame are skipped. */
__device__ __forceinline__ IssimMoments
issim_vert_moments(const int64_t *__restrict__ d_mux_h, const int64_t *__restrict__ d_muy_h,
                   const int64_t *__restrict__ d_x2_h, const int64_t *__restrict__ d_xy_h,
                   const int64_t *__restrict__ d_y2_h, const int64_t *__restrict__ d_w_h,
                   unsigned x, unsigned y, unsigned width, unsigned height)
{
    const int k_min = issim_tap_min((int)y);
    const int k_max = issim_tap_max((int)y, (int)height);
    IssimMoments m = {0, 0, 0, 0, 0, 0};
    for (int k = k_min; k < k_max; k++) {
        const size_t hidx = (size_t)((int)y - ISSIM_HALF_K + k) * width + x;
        const int64_t vk = ISSIM_KERNEL[k];
        m.mux += vk * d_mux_h[hidx];
        m.muy += vk * d_muy_h[hidx];
        m.x2 += vk * d_x2_h[hidx];
        m.xy += vk * d_xy_h[hidx];
        m.y2 += vk * d_y2_h[hidx];
        m.w += vk * d_w_h[hidx];
    }
    return m;
}

/* The factors of one pixel's SSIM term: the operands of the expression in
 * ssim_reduce_row_range() (integer_ssim.c), each formed in the CPU's order
 * (requires -ffp-contract=off). */
__device__ __forceinline__ IssimFactors issim_factors(const IssimMoments &m, double sm)
{
    IssimFactors f;
    f.w_d = (double)m.w;
    const double c1 = sm * sm * ISSIM_K1 * f.w_d * f.w_d;
    const double c2 = sm * sm * ISSIM_K2 * f.w_d * f.w_d;
    const double mx2 = (double)m.mux * (double)m.mux;
    const double mxy = (double)m.mux * (double)m.muy;
    const double my2 = (double)m.muy * (double)m.muy;
    f.lum_num = 2.0 * mxy + c1;
    f.cs_num = c2 + 2.0 * ((double)m.xy * f.w_d - mxy);
    f.lum_den = mx2 + my2 + c1;
    f.cs_den = (double)m.x2 * f.w_d - mx2 + (double)m.y2 * f.w_d - my2 + c2;
    return f;
}

/* One pixel's SSIM term, weighted by its window weight, exactly as the CPU
 * forms it: the product and the quotient in the CPU's order, which gives the
 * same double. The per-pixel form writes these. */
__device__ __forceinline__ double issim_cpu_term(const IssimFactors &f)
{
    return f.w_d * f.lum_num * f.cs_num / (f.lum_den * f.cs_den);
}

/* The term the per-block tree adds. An identical window has equal numerator
 * and denominator factors (exactly, whenever the moments are exact in
 * double: 8- and 10-bit input), and its term is its weight. The CPU's
 * quotient can round that by an ulp, which its raster-order sum absorbs on
 * nearly every frame above the raster bound; a tree does not, so it adds the
 * weight itself. Identical frames then score exactly 1, and `enable_db`
 * reports +inf / the clip_db ceiling for them (ADR-1382, ADR-1221). */
__device__ __forceinline__ double issim_pixel_term(const IssimMoments &m, double sm)
{
    const IssimFactors f = issim_factors(m, sm);
    if (f.lum_num == f.lum_den && f.cs_num == f.cs_den)
        return f.w_d;
    return issim_cpu_term(f);
}

} /* namespace */

extern "C" {

/* Pass 1, 8-bit samples. */
__global__ void integer_ssim_horiz_8bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                        const uint8_t *__restrict__ cmp, ptrdiff_t cmp_stride,
                                        int64_t *__restrict__ d_mux, int64_t *__restrict__ d_muy,
                                        int64_t *__restrict__ d_x2, int64_t *__restrict__ d_xy,
                                        int64_t *__restrict__ d_y2, int64_t *__restrict__ d_w,
                                        unsigned width, unsigned height)
{
    issim_horiz<uint8_t>(ref, ref_stride, cmp, cmp_stride, d_mux, d_muy, d_x2, d_xy, d_y2, d_w,
                         width, height);
}

/* Pass 1, 9..16-bit samples stored as little-endian uint16_t. The raw sample
 * values are used, as on the CPU; the bit depth only enters pass 2 through
 * `samplemax`. */
__global__ void integer_ssim_horiz_16bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                         const uint8_t *__restrict__ cmp, ptrdiff_t cmp_stride,
                                         int64_t *__restrict__ d_mux, int64_t *__restrict__ d_muy,
                                         int64_t *__restrict__ d_x2, int64_t *__restrict__ d_xy,
                                         int64_t *__restrict__ d_y2, int64_t *__restrict__ d_w,
                                         unsigned width, unsigned height)
{
    issim_horiz<uint16_t>(ref, ref_stride, cmp, cmp_stride, d_mux, d_muy, d_x2, d_xy, d_y2, d_w,
                          width, height);
}

/* Pass 2: vertical 9-tap over the pass-1 planes, the per-pixel SSIM term,
 * and one (term sum, weight sum) pair per block at
 * blockIdx.y * gridDim.x + blockIdx.x. `samplemax` is (1 << bpc) - 1. */
__global__ void
integer_ssim_vert_combine(const int64_t *__restrict__ d_mux_h, const int64_t *__restrict__ d_muy_h,
                          const int64_t *__restrict__ d_x2_h, const int64_t *__restrict__ d_xy_h,
                          const int64_t *__restrict__ d_y2_h, const int64_t *__restrict__ d_w_h,
                          double *__restrict__ partials, int64_t *__restrict__ partial_weights,
                          unsigned width, unsigned height, double samplemax)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;

    /* Threads outside the frame contribute (0, 0) and still take part in the
     * reduction: every thread of the block must reach the barriers. */
    double my_term = 0.0;
    int64_t my_weight = 0;
    if (x < width && y < height) {
        const IssimMoments m = issim_vert_moments(d_mux_h, d_muy_h, d_x2_h, d_xy_h, d_y2_h, d_w_h,
                                                  x, y, width, height);
        my_term = issim_pixel_term(m, samplemax);
        my_weight = m.w;
    }

    __shared__ double s_term[ISSIM_BLOCK_SZ];
    __shared__ int64_t s_weight[ISSIM_BLOCK_SZ];
    const unsigned tid = threadIdx.y * blockDim.x + threadIdx.x;
    s_term[tid] = my_term;
    s_weight[tid] = my_weight;
    __syncthreads();
    for (unsigned half = ISSIM_BLOCK_SZ / 2u; half > 0u; half >>= 1u) {
        if (tid < half) {
            s_term[tid] += s_term[tid + half];
            s_weight[tid] += s_weight[tid + half];
        }
        __syncthreads();
    }
    if (tid == 0u) {
        const unsigned blk = blockIdx.y * gridDim.x + blockIdx.x;
        partials[blk] = s_term[0];
        partial_weights[blk] = s_weight[0];
    }
}

/* Pass 2 for frames of at most ISSIM_HIP_RASTER_MAX_PIXELS pixels: every
 * pixel's CPU term and weight, unreduced, at partials[y * width + x] /
 * partial_weights[y * width + x]. The host adds them in index order, which is
 * the raster order of calc_ssim() / ssim_reduce_row_range(), so the sums are
 * the CPU's `ssim` and `ssimw` (ADR-1400). Launched over the pass-1 grid. */
__global__ void
integer_ssim_vert_terms(const int64_t *__restrict__ d_mux_h, const int64_t *__restrict__ d_muy_h,
                        const int64_t *__restrict__ d_x2_h, const int64_t *__restrict__ d_xy_h,
                        const int64_t *__restrict__ d_y2_h, const int64_t *__restrict__ d_w_h,
                        double *__restrict__ partials, int64_t *__restrict__ partial_weights,
                        unsigned width, unsigned height, double samplemax)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;

    const IssimMoments m =
        issim_vert_moments(d_mux_h, d_muy_h, d_x2_h, d_xy_h, d_y2_h, d_w_h, x, y, width, height);
    const size_t idx = (size_t)y * width + x;
    partials[idx] = issim_cpu_term(issim_factors(m, samplemax));
    partial_weights[idx] = m.w;
}

} /* extern "C" */
