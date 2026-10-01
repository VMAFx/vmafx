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
 *  Two-pass design mirrors the GLSL Vulkan shader and the CUDA twin, with a
 *  decimation ahead of it when the resolved scale is above 1:
 *
 *    0. calculate_ssim_hip_decimate_{8,16}bpc (scale > 1, ADR-1405) — both
 *       raw luma planes to the fp32 planes ssim.c's iqa_decimate() produces,
 *       bit for bit (ssim_decimate.h). W and H below are then the decimated
 *       size.
 *
 *    1. calculate_ssim_hip_horiz_{8,16}bpc (scale 1, raw samples) or
 *       calculate_ssim_hip_horiz_f32 (decimated planes) — horizontal 11-tap
 *       separable Gaussian over ref / cmp / ref^2 / cmp^2 / ref*cmp.
 *       Output: five intermediate float buffers, each (W-10) x H.
 *
 *    2. calculate_ssim_hip_vert_combine — vertical 11-tap on the five
 *       intermediates, then each pixel's SSIM term exactly as the CPU's
 *       iqa/ssim_tools.c ssim_accumulate_default_scalar() forms it:
 *       l * c * s in double from the CPU-typed luminance, contrast and
 *       structure terms (ssim_pixel()). One double per block in `partials`
 *       (shared-memory tree).
 *
 *    2b. calculate_ssim_hip_vert_combine_lcs (`enable_lcs`, ADR-1382): the
 *       same pass 2, plus the block sums of the per-pixel l, c and s.
 *
 *  The host adds the block partials in double, divides by (W-10)*(H-10) and
 *  rounds the mean to fp32, as the CPU's iqa_ssim() returns it. There is no
 *  identical-window shortcut: on identical frames the CPU's fp32 luminance
 *  denominator can leave the frame mean at 1 - 2^-24 (72.247 dB with
 *  `enable_db`), and this twin reproduces that arithmetic instead of forcing
 *  1 (ADR-1382, Research-1377).
 *
 *  HIP adaptation from CUDA:
 *  - Warp size varies: 64 on GCN/RDNA1 (wave64), 32 on RDNA2+ (wave32).
 *    Pass 2 reduces through a shared-memory tree over all SSIM_BLOCK_SIZE
 *    threads (ssim_block_sum()), so its order is the same on both.
 *  - VmafCudaBuffer / VmafPicture replaced by raw pointer parameters.
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include "ssim_decimate.h"

#define SSIM_BX 16
#define SSIM_BY 8
#define SSIM_K 11
#define SSIM_BLOCK_SIZE (SSIM_BX * SSIM_BY)

/* 11-tap Gaussian kernel (same constants as the CUDA twin). */
__device__ static const float SSIM_G[SSIM_K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

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

/* One pixel's SSIM term as the CPU adds it into ssim_sum: (l * c) * s in
 * double. Writes the three factors to `lcs` for the enable_lcs kernel. */
__device__ __forceinline__ double ssim_pixel(const SsimMoments &m, float c1, float c2,
                                             double lcs[3])
{
    ssim_lcs(m, c1, c2, &lcs[0], &lcs[1], &lcs[2]);
    return lcs[0] * lcs[1] * lcs[2];
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

/* Store the block sum of `v` at this block's slot of `out` (lane 0 only).
 * Every thread must call it: ssim_block_sum() synchronises the block. */
__device__ __forceinline__ void ssim_store_block_sum(double v, double *__restrict__ out,
                                                     double *scratch, unsigned lid)
{
    const double total = ssim_block_sum(v, scratch, lid);
    if (lid == 0u)
        out[blockIdx.y * gridDim.x + blockIdx.x] = total;
}

/* The five pass-1 output planes, each w_horiz x h_horiz floats. */
struct SsimHorizOut {
    float *ref_mu;
    float *cmp_mu;
    float *ref_sq;
    float *cmp_sq;
    float *refcmp;
};

/* Pass 1 body, shared by the three entry points: horizontal 11-tap Gaussian
 * over ref / cmp / ref^2 / cmp^2 / ref*cmp at (x, y). A sample is
 * `(float)Sample * sample_scale`: the picture_copy() value of an 8-bit
 * (scale 1) or 16-bit sample, or a decimated fp32 sample as it is. */
template <typename Sample>
__device__ __forceinline__ void
ssim_horiz(const uint8_t *ref, ptrdiff_t ref_stride, const uint8_t *cmp, ptrdiff_t cmp_stride,
           const SsimHorizOut &out, unsigned w_horiz, unsigned h_horiz, float sample_scale)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz)
        return;

    const Sample *ref_row = reinterpret_cast<const Sample *>(ref + (ptrdiff_t)y * ref_stride);
    const Sample *cmp_row = reinterpret_cast<const Sample *>(cmp + (ptrdiff_t)y * cmp_stride);
    float ref_mu_h = 0.0f;
    float cmp_mu_h = 0.0f;
    float ref_sq_h = 0.0f;
    float cmp_sq_h = 0.0f;
    float refcmp_h = 0.0f;
    for (int u = 0; u < SSIM_K; u++) {
        const unsigned src_x = x + (unsigned)u;
        const float r = (float)ref_row[src_x] * sample_scale;
        const float c = (float)cmp_row[src_x] * sample_scale;
        const float w = SSIM_G[u];
        ref_mu_h += w * r;
        cmp_mu_h += w * c;
        ref_sq_h += w * (r * r);
        cmp_sq_h += w * (c * c);
        refcmp_h += w * (r * c);
    }

    const unsigned dst_idx = y * w_horiz + x;
    out.ref_mu[dst_idx] = ref_mu_h;
    out.cmp_mu[dst_idx] = cmp_mu_h;
    out.ref_sq[dst_idx] = ref_sq_h;
    out.cmp_sq[dst_idx] = cmp_sq_h;
    out.refcmp[dst_idx] = refcmp_h;
}

/* Decimation body: output sample (x, y) of both planes, the window centred
 * on (x * scale, y * scale) of the raw plane (ssim_decimate.h). */
__device__ __forceinline__ void
ssim_decimate_pair(const uint8_t *ref, const uint8_t *cmp, float *ref_out, float *cmp_out,
                   unsigned width, unsigned height, unsigned out_width, unsigned out_height,
                   int scale, unsigned sample_bytes, float sample_scale, float tap_weight)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= out_width || y >= out_height || scale > VMAF_HIP_SSIM_MAX_EXACT_SCALE)
        return;

    VmafHipSsimDecimate d = {ref, sample_bytes, width, height, scale, sample_scale, tap_weight};
    const int centre_x = (int)x * scale;
    const int centre_y = (int)y * scale;
    const size_t index = (size_t)y * out_width + x;
    ref_out[index] = vmaf_hip_ssim_decimate_sample(&d, centre_x, centre_y);
    d.plane = cmp;
    cmp_out[index] = vmaf_hip_ssim_decimate_sample(&d, centre_x, centre_y);
}

} // namespace

extern "C" {

/**
 * Decimation (8-bpc), scale > 1 only: both raw luma planes (`width` x
 * `height`, one byte per sample, packed) to `out_width` x `out_height` fp32
 * planes holding iqa_decimate()'s output bit for bit (ssim_decimate.h,
 * ADR-1405). `tap_weight` is ssim.c's `1.0f / (scale * scale)`.
 */
__global__ void calculate_ssim_hip_decimate_8bpc(const uint8_t *__restrict__ ref,
                                                 const uint8_t *__restrict__ cmp,
                                                 float *__restrict__ ref_out,
                                                 float *__restrict__ cmp_out, unsigned width,
                                                 unsigned height, unsigned out_width,
                                                 unsigned out_height, int scale, float tap_weight)
{
    ssim_decimate_pair(ref, cmp, ref_out, cmp_out, width, height, out_width, out_height, scale, 1u,
                       1.0f, tap_weight);
}

/**
 * Decimation (10 / 12 / 16-bpc): as the 8-bpc kernel, with two-byte samples
 * and picture_copy()'s divisor (4 / 16 / 256) as `sample_scale`, its exact
 * reciprocal.
 */
__global__ void calculate_ssim_hip_decimate_16bpc(
    const uint8_t *__restrict__ ref, const uint8_t *__restrict__ cmp, float *__restrict__ ref_out,
    float *__restrict__ cmp_out, unsigned width, unsigned height, unsigned out_width,
    unsigned out_height, int scale, float tap_weight, float sample_scale)
{
    ssim_decimate_pair(ref, cmp, ref_out, cmp_out, width, height, out_width, out_height, scale, 2u,
                       sample_scale, tap_weight);
}

/**
 * Pass 1 (8-bpc): horizontal 11-tap Gaussian.
 * ref/dis are uint8_t row-major, ref_stride/dis_stride bytes per row.
 * Writes h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp (each
 * w_horiz x h_horiz floats, stride = w_horiz).
 */
__global__ void
calculate_ssim_hip_horiz_8bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                              const uint8_t *__restrict__ cmp, ptrdiff_t cmp_stride,
                              float *__restrict__ h_ref_mu, float *__restrict__ h_cmp_mu,
                              float *__restrict__ h_ref_sq, float *__restrict__ h_cmp_sq,
                              float *__restrict__ h_refcmp, unsigned w_horiz, unsigned h_horiz)
{
    const SsimHorizOut out = {h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp};
    ssim_horiz<uint8_t>(ref, ref_stride, cmp, cmp_stride, out, w_horiz, h_horiz, 1.0f);
}

/**
 * Pass 1 (16-bpc): horizontal 11-tap Gaussian.
 * Same as 8bpc but pixels are uint16_t (10/12/16bpc), normalised by
 * scaler (4 / 16 / 256) to the [0,255] float range the CUDA twin uses.
 */
__global__ void calculate_ssim_hip_horiz_16bpc(
    const uint8_t *__restrict__ ref, ptrdiff_t ref_stride, const uint8_t *__restrict__ cmp,
    ptrdiff_t cmp_stride, float *__restrict__ h_ref_mu, float *__restrict__ h_cmp_mu,
    float *__restrict__ h_ref_sq, float *__restrict__ h_cmp_sq, float *__restrict__ h_refcmp,
    unsigned w_horiz, unsigned h_horiz, unsigned bpc)
{
    float scaler = 1.0f;
    if (bpc == 10u)
        scaler = 4.0f;
    else if (bpc == 12u)
        scaler = 16.0f;
    else if (bpc == 16u)
        scaler = 256.0f;
    const SsimHorizOut out = {h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp};
    ssim_horiz<uint16_t>(ref, ref_stride, cmp, cmp_stride, out, w_horiz, h_horiz, 1.0f / scaler);
}

/**
 * Pass 1 over decimated planes (scale > 1): ref / cmp are the fp32 planes
 * the decimation kernel wrote, already picture_copy() values; the strides
 * are in bytes.
 */
__global__ void
calculate_ssim_hip_horiz_f32(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                             const uint8_t *__restrict__ cmp, ptrdiff_t cmp_stride,
                             float *__restrict__ h_ref_mu, float *__restrict__ h_cmp_mu,
                             float *__restrict__ h_ref_sq, float *__restrict__ h_cmp_sq,
                             float *__restrict__ h_refcmp, unsigned w_horiz, unsigned h_horiz)
{
    const SsimHorizOut out = {h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp};
    ssim_horiz<float>(ref, ref_stride, cmp, cmp_stride, out, w_horiz, h_horiz, 1.0f);
}

/**
 * Pass 2: vertical 11-tap Gaussian + the CPU's per-pixel SSIM term +
 * per-block double partial sum. Grid sized over (W-10) x (H-10) =
 * w_final x h_final. `partials` receives one double per block
 * (block_idx = blockIdx.y * gridDim.x + blockIdx.x).
 */
__global__ void calculate_ssim_hip_vert_combine(
    const float *__restrict__ h_ref_mu, const float *__restrict__ h_cmp_mu,
    const float *__restrict__ h_ref_sq, const float *__restrict__ h_cmp_sq,
    const float *__restrict__ h_refcmp, double *__restrict__ partials, unsigned w_horiz,
    unsigned w_final, unsigned h_final, float c1, float c2)
{
    __shared__ double s_scratch[SSIM_BLOCK_SIZE];
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    double my_ssim = 0.0;
    if (x < w_final && y < h_final) {
        const SsimMoments m =
            ssim_vertical_moments(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp, w_horiz, x, y);
        double lcs[3];
        my_ssim = ssim_pixel(m, c1, c2, lcs);
    }
    ssim_store_block_sum(my_ssim, partials, s_scratch, lid);
}

/**
 * Pass 2 with `enable_lcs` (ADR-1382): the SSIM partial of the default
 * kernel, plus the block sums of the per-pixel L, C and S terms in
 * `lcs_partials`, laid out [l | c | s] with `block_count` doubles each.
 * Out-of-frame threads contribute zeros.
 */
__global__ void calculate_ssim_hip_vert_combine_lcs(
    const float *__restrict__ h_ref_mu, const float *__restrict__ h_cmp_mu,
    const float *__restrict__ h_ref_sq, const float *__restrict__ h_cmp_sq,
    const float *__restrict__ h_refcmp, double *__restrict__ partials,
    double *__restrict__ lcs_partials, unsigned w_horiz, unsigned w_final, unsigned h_final,
    unsigned block_count, float c1, float c2)
{
    __shared__ double s_scratch[SSIM_BLOCK_SIZE];
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    double my_ssim = 0.0;
    double lcs[3] = {0.0, 0.0, 0.0};
    if (x < w_final && y < h_final) {
        const SsimMoments m =
            ssim_vertical_moments(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp, w_horiz, x, y);
        my_ssim = ssim_pixel(m, c1, c2, lcs);
    }
    ssim_store_block_sum(my_ssim, partials, s_scratch, lid);
    for (unsigned k = 0u; k < 3u; k++)
        ssim_store_block_sum(lcs[k], lcs_partials + ((size_t)k * block_count), s_scratch, lid);
}

} /* extern "C" */
