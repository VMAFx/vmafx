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
 *       structure terms (ssim_pixel()). One double per window in `terms`,
 *       at the window's raster position; nothing is reduced on the device.
 *
 *    The window sums of both passes and the l / c / s terms are the CPU's
 *    (ADR-1441): iqa_convolve() adds the eleven fp32 products of a window in
 *    fp64 and rounds once per pass, and so do the kernels, through the
 *    arithmetic float_ms_ssim_hip shares with the CPU
 *    (../integer_ms_ssim/ms_ssim_arith.h: an exact fp32 pair in place of the
 *    fp64 sum). An fp32 running sum, which these kernels had, rounds at
 *    every tap and left the score up to 4.8e-7 from the CPU's.
 *
 *    2b. calculate_ssim_hip_vert_combine_lcs (`enable_lcs`, ADR-1382): the
 *       same pass 2, plus each window's l, c and s, one plane each.
 *
 *  The host adds each plane of terms in raster order into one double, divides
 *  by (W-10)*(H-10) and rounds the mean to fp32, as the CPU's iqa_ssim() does
 *  (the construction of ADR-1438 for the fixed-point twin). The CPU's sum is one sequential chain of fp64 adds that round,
 *  so a sum in another order is another double: block sums added on the host,
 *  which these kernels had, put one mean in 1.5e7 noise frames on the
 *  neighbouring float (T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02). There is no
 *  identical-window shortcut: on identical frames the CPU's fp32 luminance
 *  denominator can leave the frame mean at 1 - 2^-24 (72.247 dB with
 *  `enable_db`), and this twin reproduces that arithmetic instead of forcing
 *  1 (ADR-1382, Research-1377).
 *
 *  HIP adaptation from CUDA:
 *  - VmafCudaBuffer / VmafPicture replaced by raw pointer parameters.
 */

#include <hip/hip_runtime.h>
#include <cstdint>

#include "../integer_ms_ssim/ms_ssim_arith.h"
#include "ssim_decimate.h"

#define SSIM_BX 16
#define SSIM_BY 8
#define SSIM_K 11

/* The window is iqa/ssim_tools.h's 11-tap Gaussian, whose taps and sums live
 * in ms_ssim_arith.h. */
static_assert(SSIM_K == VMAF_HIP_MS_SSIM_WINDOW, "float_ssim and float_ms_ssim share the window");

/* Device helpers are called only by the kernels of this translation unit,
 * so they live in an anonymous namespace (internal linkage). */
namespace
{

/* Vertical 11-tap pass over the five horizontal planes at (x, y):
 * iqa_convolve()'s vertical pass, through the shared arithmetic. */
__device__ __forceinline__ VmafHipMsWindow
ssim_vertical_moments(const float *__restrict__ h_ref_mu, const float *__restrict__ h_cmp_mu,
                      const float *__restrict__ h_ref_sq, const float *__restrict__ h_cmp_sq,
                      const float *__restrict__ h_refcmp, unsigned w_horiz, unsigned x, unsigned y)
{
    const VmafHipMsPlanes planes = {h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp};
    return vmaf_hip_ms_ssim_vertical(&planes, (size_t)y * w_horiz + x, w_horiz);
}

/* One pixel's SSIM term as the CPU adds it into ssim_sum: (l * c) * s in
 * double, from ssim_accumulate_default_scalar()'s l, c and s with C3 = C2 / 2
 * (the shared arithmetic). Writes the three factors to `lcs` for the
 * enable_lcs kernel. */
__device__ __forceinline__ double ssim_pixel(const VmafHipMsWindow &m, float c1, float c2,
                                             double lcs[3])
{
    const VmafHipMsLcs terms = vmaf_hip_ms_ssim_lcs(&m, c1, c2, c2 / 2.0f);
    lcs[0] = terms.l;
    lcs[1] = terms.c;
    lcs[2] = terms.s;
    return lcs[0] * lcs[1] * lcs[2];
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
    /* The window's eleven samples of each row as picture_copy() values;
     * iqa_convolve()'s horizontal pass over them. */
    float ref_taps[SSIM_K];
    float cmp_taps[SSIM_K];
    for (int u = 0; u < SSIM_K; u++) {
        const unsigned src_x = x + (unsigned)u;
        ref_taps[u] = (float)ref_row[src_x] * sample_scale;
        cmp_taps[u] = (float)cmp_row[src_x] * sample_scale;
    }
    const VmafHipMsWindow sums = vmaf_hip_ms_ssim_horizontal(ref_taps, cmp_taps);

    const unsigned dst_idx = y * w_horiz + x;
    out.ref_mu[dst_idx] = sums.ref_mu;
    out.cmp_mu[dst_idx] = sums.cmp_mu;
    out.ref_sq[dst_idx] = sums.ref_sq;
    out.cmp_sq[dst_idx] = sums.cmp_sq;
    out.refcmp[dst_idx] = sums.refcmp;
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
    if (bpc == 10u) {
        scaler = 4.0f;
    } else if (bpc == 12u) {
        scaler = 16.0f;
    } else if (bpc == 16u) {
        scaler = 256.0f;
    }
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
 * Pass 2: vertical 11-tap Gaussian + the CPU's per-window SSIM term. Grid
 * sized over (W-10) x (H-10) = w_final x h_final. `terms` receives one double
 * per window at its raster position (y * w_final + x): the value
 * ssim_accumulate_default_scalar() adds into `ssim_sum` for that window. The
 * host adds them in that order.
 */
__global__ void calculate_ssim_hip_vert_combine(
    const float *__restrict__ h_ref_mu, const float *__restrict__ h_cmp_mu,
    const float *__restrict__ h_ref_sq, const float *__restrict__ h_cmp_sq,
    const float *__restrict__ h_refcmp, double *__restrict__ terms, unsigned w_horiz,
    unsigned w_final, unsigned h_final, float c1, float c2)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_final || y >= h_final)
        return;
    const VmafHipMsWindow m =
        ssim_vertical_moments(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp, w_horiz, x, y);
    double lcs[3];
    terms[(size_t)y * w_final + x] = ssim_pixel(m, c1, c2, lcs);
}

/**
 * Pass 2 with `enable_lcs` (ADR-1382): the SSIM term of the default kernel,
 * plus the window's L, C and S in `lcs_terms`, three planes of
 * w_final * h_final doubles laid out [l | c | s], each in raster order.
 */
__global__ void calculate_ssim_hip_vert_combine_lcs(
    const float *__restrict__ h_ref_mu, const float *__restrict__ h_cmp_mu,
    const float *__restrict__ h_ref_sq, const float *__restrict__ h_cmp_sq,
    const float *__restrict__ h_refcmp, double *__restrict__ terms, double *__restrict__ lcs_terms,
    unsigned w_horiz, unsigned w_final, unsigned h_final, float c1, float c2)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_final || y >= h_final)
        return;
    const VmafHipMsWindow m =
        ssim_vertical_moments(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp, w_horiz, x, y);
    const size_t windows = (size_t)w_final * h_final;
    const size_t window = (size_t)y * w_final + x;
    double lcs[3];
    terms[window] = ssim_pixel(m, c1, c2, lcs);
    for (unsigned k = 0u; k < 3u; k++)
        lcs_terms[(size_t)k * windows + window] = lcs[k];
}

} /* extern "C" */
