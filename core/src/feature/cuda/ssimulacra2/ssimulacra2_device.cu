/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  Device stages of the ssimulacra2 CUDA twin that used to run on the host
 *  (ADR-1391, the CUDA port of the ADR-1363 chain): YUV -> linear RGB, linear
 *  RGB -> XYB, the per-pixel SSIM / edge-difference terms with their
 *  per-channel sums, and the 2x2 downsample. The products and blurs are in
 *  ssimulacra2_blur.cu.
 *
 *  Bit-exactness: built with `--fmad=false` (no contraction), the cube root
 *  and the sRGB EOTF are the shared helpers of ssimulacra2_math.h with a
 *  correctly rounded division (`__fdiv_rn`), and every product that feeds an
 *  add sits in its own expression, as in ssimulacra2.c. The per-pixel terms
 *  that the CPU evaluates in fp64 are evaluated in fp64 with the CPU's
 *  expressions; only their summation order differs (a fixed tree, so the
 *  result is deterministic).
 */

#include <stdint.h>

#include "cuda/ssimulacra2_cuda.h"

/* The shared helpers of ssimulacra2_math.h / ssimulacra2_score.h run in device
 * code here, the EOTF table is compiled into device memory with the same
 * values, and the cube root divides correctly rounded (ADR-1391). */
#define VMAF_SS2_FUNC static __device__ __forceinline__
#define VMAF_SS2_FDIV(a, b) __fdiv_rn((a), (b))
#define VMAF_SS2_EOTF_LUT_STORAGE static __device__ const
#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_score.h"

namespace
{

/* ssimulacra2.c constants. */
constexpr float kC2 = 0.0009f;
constexpr float kM00 = 0.30f;
constexpr float kM02 = 0.078f;
constexpr float kM10 = 0.23f;
constexpr float kM12 = 0.078f;
constexpr float kM20 = 0.24342268924547819f;
constexpr float kM21 = 0.20476744424496821f;
constexpr float kOpsinBias = 0.0037930732552754493f;

/* ssimulacra2.c::read_plane coordinate mapping (nearest neighbour). */
__device__ __forceinline__ unsigned ss2c_map(unsigned v, unsigned plane_dim, unsigned luma_dim)
{
    uint64_t s = v;
    if (plane_dim != luma_dim) {
        s = (plane_dim * 2u == luma_dim) ? (uint64_t)(v >> 1) : (uint64_t)v * plane_dim / luma_dim;
    }
    return (s >= plane_dim) ? plane_dim - 1u : (unsigned)s;
}

__device__ __forceinline__ float ss2c_sample(const Ss2cYuvArgs &a, unsigned img, unsigned p,
                                             unsigned x, unsigned y)
{
    const unsigned sx = ss2c_map(x, a.plane_w[p], a.width);
    const unsigned sy = ss2c_map(y, a.plane_h[p], a.height);
    // SAFETY: sx < plane_w[p] and sy < plane_h[p] (ss2c_map clamps), and the
    // picture holds plane_h[p] rows of pitch[img][p] bytes.
    const char *row = (const char *)a.plane[img][p] + (size_t)sy * a.pitch[img][p];
    if (a.wide)
        return (float)((const uint16_t *)row)[sx];
    return (float)((const uint8_t *)row)[sx];
}

__device__ __forceinline__ float ss2c_clampf(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

__device__ __forceinline__ double ss2c_quartic(double x)
{
    x *= x;
    return x * x;
}

/* Per-pixel terms of ssimulacra2.c::ssim_map and ::edge_diff_map, added to
 * the six running sums of one channel. */
__device__ __forceinline__ void ss2c_accumulate(const Ss2cCombineArgs &a, size_t idx,
                                                double sums[SS2C_SUMS])
{
    const float mu1 = a.mu1[idx];
    const float mu2 = a.mu2[idx];
    const float mu11 = mu1 * mu1;
    const float mu22 = mu2 * mu2;
    const float mu12 = mu1 * mu2;
    const float diff = mu1 - mu2;
    const float diff_sq = diff * diff;
    const float num_m = 1.0f - diff_sq;
    const float cov = a.s12[idx] - mu12;
    const float twice_cov = 2.0f * cov;
    const float num_s = twice_cov + kC2;
    const float denom_s = (a.s11[idx] - mu11) + (a.s22[idx] - mu22) + kC2;
    double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
    if (d < 0.0)
        d = 0.0;

    const double ed1 = fabs((double)a.img1[idx] - (double)mu1);
    const double ed2 = fabs((double)a.img2[idx] - (double)mu2);
    const double d1 = (1.0 + ed2) / (1.0 + ed1) - 1.0;
    double artifact;
    double detail;
    vmaf_ss2_split_edge_difference(d1, &artifact, &detail);

    sums[0] += d;
    sums[1] += ss2c_quartic(d);
    sums[2] += artifact;
    sums[3] += ss2c_quartic(artifact);
    sums[4] += detail;
    sums[5] += ss2c_quartic(detail);
}

/* Fixed-shape tree over the SS2C_REDUCE_BLOCK threads of a block; thread 0
 * ends with the block's six sums in shared[0..5]. */
__device__ __forceinline__ void ss2c_block_tree(double *shared, unsigned lane,
                                                const double sums[SS2C_SUMS])
{
    for (unsigned k = 0; k < SS2C_SUMS; k++)
        shared[lane * SS2C_SUMS + k] = sums[k];
    __syncthreads();
    for (unsigned half = SS2C_REDUCE_BLOCK / 2u; half > 0u; half >>= 1) {
        if (lane < half) {
            for (unsigned k = 0; k < SS2C_SUMS; k++)
                shared[lane * SS2C_SUMS + k] += shared[(lane + half) * SS2C_SUMS + k];
        }
        __syncthreads();
    }
}

} // namespace

extern "C" {

/* One pixel of ssimulacra2.c::picture_to_linear_rgb per thread; blockIdx.z
 * selects the image. */
__global__ void __launch_bounds__(SS2C_PIX_BX *SS2C_PIX_BY)
    ssimulacra2_yuv_to_linear(const Ss2cYuvArgs a)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned img = blockIdx.z;
    if (x >= a.width || y >= a.height)
        return;
    const Ss2cYuvCoefficients &k = a.k;
    const float Y = ss2c_sample(a, img, 0, x, y) * k.inv_peak;
    const float U = ss2c_sample(a, img, 1, x, y) * k.inv_peak;
    const float V = ss2c_sample(a, img, 2, x, y) * k.inv_peak;
    const float Yn = (Y - k.y_off) * k.y_scale;
    const float Un = (U - k.c_off) * k.c_scale;
    const float Vn = (V - k.c_off) * k.c_scale;
    /* Single-rounded FMAs in this order (ADR-0891 / ADR-1205). */
    const float R = __fmaf_rn(k.cr_r, Vn, Yn);
    const float G0 = __fmaf_rn(k.cb_g, Un, Yn);
    const float G = __fmaf_rn(k.cr_g, Vn, G0);
    const float B = __fmaf_rn(k.cb_b, Un, Yn);
    const size_t plane = (size_t)a.width * a.height;
    const size_t idx = (size_t)y * a.width + x;
    float *out = a.out[img];
    out[idx] = vmaf_ss2_srgb_eotf(ss2c_clampf(R));
    out[plane + idx] = vmaf_ss2_srgb_eotf(ss2c_clampf(G));
    out[2u * plane + idx] = vmaf_ss2_srgb_eotf(ss2c_clampf(B));
}

/* One pixel of ssimulacra2.c::linear_rgb_to_xyb per thread; blockIdx.y
 * selects the image. `pixels` = width x height (compact planes). */
__global__ void __launch_bounds__(256)
    ssimulacra2_xyb(const float *__restrict__ lin_ref, const float *__restrict__ lin_dis,
                    float *__restrict__ xyb_ref, float *__restrict__ xyb_dis, unsigned pixels)
{
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= pixels)
        return;
    const float *lin = (blockIdx.y == 0u) ? lin_ref : lin_dis;
    float *xyb = (blockIdx.y == 0u) ? xyb_ref : xyb_dis;
    const float r = lin[i];
    const float g = lin[pixels + i];
    const float b = lin[2u * pixels + i];
    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);
    /* Products in named temporaries: `kM00 * r + m01 * g + ...` with
     * contraction off, left to right. */
    const float l_r = kM00 * r;
    const float l_g = m01 * g;
    const float l_b = kM02 * b;
    const float m_r = kM10 * r;
    const float m_g = m11 * g;
    const float m_b = kM12 * b;
    const float s_r = kM20 * r;
    const float s_g = kM21 * g;
    const float s_b = m22 * b;
    float l = ((l_r + l_g) + l_b) + kOpsinBias;
    float m = ((m_r + m_g) + m_b) + kOpsinBias;
    float s = ((s_r + s_g) + s_b) + kOpsinBias;
    if (l < 0.0f)
        l = 0.0f;
    if (m < 0.0f)
        m = 0.0f;
    if (s < 0.0f)
        s = 0.0f;
    const float L = vmaf_ss2_cbrtf(l) - cbrt_bias;
    const float M = vmaf_ss2_cbrtf(m) - cbrt_bias;
    const float S = vmaf_ss2_cbrtf(s) - cbrt_bias;
    const float X = 0.5f * (L - M);
    const float Yv = 0.5f * (L + M);
    /* MakePositiveXYB, libjxl order (B uses Y before Y is offset). */
    const float X14 = X * 14.0f;
    xyb[i] = X14 + 0.42f;
    xyb[pixels + i] = Yv + 0.01f;
    xyb[2u * pixels + i] = (S - Yv) + 0.55f;
}

/* Per-channel partial sums of the SSIM / edge terms: blockIdx.y = channel,
 * blockIdx.x = group. Each thread adds a fixed strided subset of the plane,
 * then the block's fixed tree; one row of six sums per (channel, group). */
__global__ void __launch_bounds__(SS2C_REDUCE_BLOCK)
    ssimulacra2_combine_partials(const Ss2cCombineArgs a)
{
    __shared__ double shared[SS2C_REDUCE_BLOCK * SS2C_SUMS];
    const unsigned c = blockIdx.y;
    const unsigned group = blockIdx.x;
    const unsigned lane = threadIdx.x;
    const size_t offset = (size_t)c * a.pixels;
    const size_t stride = (size_t)a.groups * SS2C_REDUCE_BLOCK;
    double sums[SS2C_SUMS] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (size_t i = (size_t)group * SS2C_REDUCE_BLOCK + lane; i < a.pixels; i += stride)
        ss2c_accumulate(a, offset + i, sums);
    ss2c_block_tree(shared, lane, sums);
    if (lane == 0u) {
        double *out = a.partials + ((size_t)c * SS2C_MAX_GROUPS + group) * SS2C_SUMS;
        for (unsigned k = 0; k < SS2C_SUMS; k++)
            out[k] = shared[k];
    }
}

/* One block per channel: the fixed tree over that channel's group rows. */
__global__ void __launch_bounds__(SS2C_REDUCE_BLOCK)
    ssimulacra2_combine_final(const double *__restrict__ partials, double *__restrict__ totals,
                              unsigned groups)
{
    __shared__ double shared[SS2C_REDUCE_BLOCK * SS2C_SUMS];
    const unsigned c = blockIdx.x;
    const unsigned lane = threadIdx.x;
    double sums[SS2C_SUMS] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (unsigned g = lane; g < groups; g += SS2C_REDUCE_BLOCK) {
        const double *row = partials + ((size_t)c * SS2C_MAX_GROUPS + g) * SS2C_SUMS;
        for (unsigned k = 0; k < SS2C_SUMS; k++)
            sums[k] += row[k];
    }
    ss2c_block_tree(shared, lane, sums);
    if (lane == 0u) {
        for (unsigned k = 0; k < SS2C_SUMS; k++)
            totals[(size_t)c * SS2C_SUMS + k] = shared[k];
    }
}

/* One output pixel of ssimulacra2.c::downsample_2x2 per thread, all three
 * channels; blockIdx.z selects the image. Compact planes on both sides. */
__global__ void __launch_bounds__(SS2C_PIX_BX *SS2C_PIX_BY)
    ssimulacra2_downsample(const float *__restrict__ in_ref, const float *__restrict__ in_dis,
                           float *__restrict__ out_ref, float *__restrict__ out_dis, unsigned iw,
                           unsigned ih, unsigned ow, unsigned oh)
{
    const unsigned ox = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned oy = blockIdx.y * blockDim.y + threadIdx.y;
    if (ox >= ow || oy >= oh)
        return;
    const float *in = (blockIdx.z == 0u) ? in_ref : in_dis;
    float *out = (blockIdx.z == 0u) ? out_ref : out_dis;
    const size_t in_plane = (size_t)iw * ih;
    const size_t out_plane = (size_t)ow * oh;
    for (unsigned c = 0; c < SS2C_CHANNELS; c++) {
        const float *ip = in + (size_t)c * in_plane;
        float sum = 0.0f;
        for (unsigned dy = 0; dy < 2u; dy++) {
            for (unsigned dx = 0; dx < 2u; dx++) {
                unsigned ix = ox * 2u + dx;
                unsigned iy = oy * 2u + dy;
                if (ix >= iw)
                    ix = iw - 1u;
                if (iy >= ih)
                    iy = ih - 1u;
                sum += ip[(size_t)iy * iw + ix];
            }
        }
        out[(size_t)c * out_plane + (size_t)oy * ow + ox] = sum * 0.25f;
    }
}

} /* extern "C" */
