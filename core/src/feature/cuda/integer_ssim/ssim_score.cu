/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  CUDA compute kernels for the float_ssim feature extractor
 *  (T7-23 / batch 2 part 1b / ADR-0188 / ADR-0189; decimation ADR-1399).
 *  The pipeline is the CPU's (ssim.c, iqa/decimate.c, iqa/convolve.c,
 *  iqa/ssim_tools.c), stage for stage and rounding for rounding:
 *
 *    0. calculate_ssim_decimate_{8,16}bpc — scale > 1 only. ssim.c's box
 *       low-pass and iqa_decimate(): each output is the scale x scale
 *       window at (x * scale, y * scale) with KBND_SYMMETRIC edges, the
 *       fp32 product `sample * (1.0f / (scale * scale))` summed exactly in
 *       int64 units of 2^-52 and rounded to fp32 once. Writes the two
 *       decimated fp32 planes.
 *
 *    1. calculate_ssim_horiz_{8,16}bpc (scale 1: reads the picture's
 *       data[0] with the picture_copy normalisation inline) or
 *       calculate_ssim_horiz_planes (scale > 1: reads the decimated
 *       planes) — horizontal 11-tap Gaussian over ref / cmp / ref² /
 *       cmp² / ref·cmp, each fp32 product added to a double sum that is
 *       rounded to fp32 once, as iqa_convolve() does.
 *
 *    2. calculate_ssim_vert_combine — vertical 11-tap on the five
 *       intermediates (same double sum), then the CPU's per-window
 *       l * c * s with the CPU's types and rounding points
 *       (ssim_terms()), stored at the window's raster position.
 *       calculate_ssim_vert_combine_lcs is the `enable_lcs` variant: the
 *       same SSIM value followed by the window's luminance, contrast and
 *       structure terms (ADR-1373).
 *
 *  Nothing is reduced on the device. iqa_ssim() adds every window's term
 *  into one double per sum, left to right and top to bottom, and a sum of
 *  doubles is its order: the host reads the terms back and adds them in
 *  index order (integer_ssim_cuda.c::float_ssim_frame_sum(), following
 *  ADR-1424), divides by (W'-10)·(H'-10) of the decimated plane and rounds
 *  the mean to fp32, as iqa_ssim() returns it. A per-block sum of the same
 *  terms rounded one frame's mean to the neighbouring float
 *  (T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02).
 *
 *  NVCC contracts `a * b + c` by default (--fmad=true): every rounding
 *  that matters here is spelled with an intrinsic.
 */

#include "cuda_device_ptr.cuh"
#include "cuda_helper.cuh"
#include "cuda/integer_ssim_cuda.h"
#include "common.h"

#define BLOCK_X 16
#define BLOCK_Y 8
#define BLOCK_SIZE (BLOCK_X * BLOCK_Y)
#define K 11
/* Doubles per window in the `enable_lcs` term plane: SSIM, L, C, S. Mirrors
 * FLOAT_SSIM_LCS_SUMS in integer_ssim_cuda.c. */
#define LCS_TERMS 4u

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
    SsimVertInputs in;
    in.ref_mu = VMAF_CUDA_DPTR(const float, ref_mu.data);
    in.cmp_mu = VMAF_CUDA_DPTR(const float, cmp_mu.data);
    in.ref_sq = VMAF_CUDA_DPTR(const float, ref_sq.data);
    in.cmp_sq = VMAF_CUDA_DPTR(const float, cmp_sq.data);
    in.refcmp = VMAF_CUDA_DPTR(const float, refcmp.data);
    return in;
}

/* The five sums of one convolution pass, in iqa/convolve.c's type. */
struct MomentSums {
    double ref_mu = 0.0;
    double cmp_mu = 0.0;
    double ref_sq = 0.0;
    double cmp_sq = 0.0;
    double refcmp = 0.0;
};

/* One tap of iqa/convolve.c: `const float prod = img * kernel;
 * sum += (double)prod;` — the product rounds to fp32, the sum is a double. */
__device__ inline double add_tap(double sum, float sample, float weight)
{
    return __dadd_rn(sum, (double)__fmul_rn(sample, weight));
}

/* Horizontal pass: the CPU convolves ref, cmp and the fp32 products
 * ref * ref, cmp * cmp and ref * cmp (ssim_tools.c::ssim_precompute_scalar). */
__device__ inline void add_horizontal_tap(MomentSums &sums, float ref, float cmp, float weight)
{
    sums.ref_mu = add_tap(sums.ref_mu, ref, weight);
    sums.cmp_mu = add_tap(sums.cmp_mu, cmp, weight);
    sums.ref_sq = add_tap(sums.ref_sq, __fmul_rn(ref, ref), weight);
    sums.cmp_sq = add_tap(sums.cmp_sq, __fmul_rn(cmp, cmp), weight);
    sums.refcmp = add_tap(sums.refcmp, __fmul_rn(ref, cmp), weight);
}

/* A pass result: iqa/convolve.c's `(float)(sum * scale)` with scale 1. */
__device__ inline SsimMoments round_moments(const MomentSums &sums)
{
    SsimMoments m;
    m.ref_mu = __double2float_rn(sums.ref_mu);
    m.cmp_mu = __double2float_rn(sums.cmp_mu);
    m.ref_sq = __double2float_rn(sums.ref_sq);
    m.cmp_sq = __double2float_rn(sums.cmp_sq);
    m.refcmp = __double2float_rn(sums.refcmp);
    return m;
}

/* Vertical 11-tap of the five horizontal-pass moments at (x, y). */
__device__ inline SsimMoments vertical_moments(const SsimVertInputs &in, unsigned x, unsigned y,
                                               unsigned w_horiz)
{
    MomentSums sums;
    for (int v = 0; v < K; v++) {
        const unsigned src_idx = (y + (unsigned)v) * w_horiz + x;
        const float w = G[v];
        sums.ref_mu = add_tap(sums.ref_mu, __ldg(&in.ref_mu[src_idx]), w);
        sums.cmp_mu = add_tap(sums.cmp_mu, __ldg(&in.cmp_mu[src_idx]), w);
        sums.ref_sq = add_tap(sums.ref_sq, __ldg(&in.ref_sq[src_idx]), w);
        sums.cmp_sq = add_tap(sums.cmp_sq, __ldg(&in.cmp_sq[src_idx]), w);
        sums.refcmp = add_tap(sums.refcmp, __ldg(&in.refcmp[src_idx]), w);
    }
    return round_moments(sums);
}

/* Pass-1 sample sources: the picture's luma at scale 1, the decimated fp32
 * planes above it. */
struct LumaPicture8 {
    const VmafPicture *ref;
    const VmafPicture *cmp;
};

struct LumaPicture16 {
    const VmafPicture *ref;
    const VmafPicture *cmp;
    float scaler;
};

struct LumaPlanes {
    const float *__restrict__ ref;
    const float *__restrict__ cmp;
    unsigned width;
};

__device__ inline float luma_reference(const LumaPicture8 &s, unsigned x, unsigned y)
{
    return read_norm_8bpc(*s.ref, x, y);
}

__device__ inline float luma_comparison(const LumaPicture8 &s, unsigned x, unsigned y)
{
    return read_norm_8bpc(*s.cmp, x, y);
}

__device__ inline float luma_reference(const LumaPicture16 &s, unsigned x, unsigned y)
{
    return read_norm_16bpc(*s.ref, x, y, s.scaler);
}

__device__ inline float luma_comparison(const LumaPicture16 &s, unsigned x, unsigned y)
{
    return read_norm_16bpc(*s.cmp, x, y, s.scaler);
}

__device__ inline float luma_reference(const LumaPlanes &s, unsigned x, unsigned y)
{
    return __ldg(&s.ref[y * s.width + x]);
}

__device__ inline float luma_comparison(const LumaPlanes &s, unsigned x, unsigned y)
{
    return __ldg(&s.cmp[y * s.width + x]);
}

/* The five pass-1 output planes, w_horiz x h_horiz each. */
struct SsimHorizOutputs {
    float *ref_mu;
    float *cmp_mu;
    float *ref_sq;
    float *cmp_sq;
    float *refcmp;
};

__device__ inline SsimHorizOutputs horiz_outputs(const VmafCudaBuffer &ref_mu,
                                                 const VmafCudaBuffer &cmp_mu,
                                                 const VmafCudaBuffer &ref_sq,
                                                 const VmafCudaBuffer &cmp_sq,
                                                 const VmafCudaBuffer &refcmp)
{
    SsimHorizOutputs out;
    out.ref_mu = VMAF_CUDA_DPTR(float, ref_mu.data);
    out.cmp_mu = VMAF_CUDA_DPTR(float, cmp_mu.data);
    out.ref_sq = VMAF_CUDA_DPTR(float, ref_sq.data);
    out.cmp_sq = VMAF_CUDA_DPTR(float, cmp_sq.data);
    out.refcmp = VMAF_CUDA_DPTR(float, refcmp.data);
    return out;
}

/* Pass 1 for one thread: output pixel (x, y) of the (W-10) x H "valid"
 * buffer reads input columns [x, x+10] of row y. */
template <typename Source>
__device__ inline void horizontal_pass(const Source &source, const SsimHorizOutputs &out,
                                       unsigned w_horiz, unsigned h_horiz)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz)
        return;
    MomentSums sums;
    for (int u = 0; u < K; u++) {
        const unsigned src_x = x + (unsigned)u;
        add_horizontal_tap(sums, luma_reference(source, src_x, y),
                           luma_comparison(source, src_x, y), G[u]);
    }
    const SsimMoments m = round_moments(sums);
    const unsigned dst_idx = y * w_horiz + x;
    out.ref_mu[dst_idx] = m.ref_mu;
    out.cmp_mu[dst_idx] = m.cmp_mu;
    out.ref_sq[dst_idx] = m.ref_sq;
    out.cmp_sq[dst_idx] = m.cmp_sq;
    out.refcmp[dst_idx] = m.refcmp;
}

/* ADR-1399 fixed point of the decimation sum. ssim.c's low-pass tap is
 * 1.0f / (scale * scale) and every sample is a multiple of 2^-8 (16-bit
 * picture_copy()), so up to scale 128 each fp32 product is a multiple of
 * 2^-52 and a whole window sums below 2^8.001: the int64 sum in units of
 * 2^-52 is exact, as is the CPU's double sum over that range, and one
 * round-to-nearest conversion to fp32 gives the CPU's (float)sum bit for
 * bit (Research-2130). The host refuses scales above 128. */
#define DECIMATE_FIXED_ONE 0x1p52f
#define DECIMATE_FIXED_INV 0x1p-52f

/* What a decimation thread needs besides the two pictures. */
struct DecimateGeometry {
    int width;
    int height;
    unsigned out_width;
    unsigned out_height;
    int scale;
    /* picture_copy()'s divisor as an exact reciprocal: 1, 1/4, 1/16, 1/256. */
    float sample_scale;
    /* ssim.c::ssim_low_pass_alloc's inv2 = 1.0f / (float)(scale * scale). */
    float tap_weight;
};

/* iqa/convolve.c::KBND_SYMMETRIC: period-2n mirror, edge sample repeated.
 * Identity inside the plane, so it also covers iqa_filter_pixel()'s
 * direct-read interior path. */
__device__ inline int symmetric_index(int position, int extent)
{
    const int period = 2 * extent;
    int folded = position % period;
    if (folded < 0)
        folded += period;
    return folded >= extent ? period - folded - 1 : folded;
}

/* One output of iqa_decimate() with ssim.c's low-pass kernel. Row r of the
 * window is offset r - scale / 2: iqa_filter_pixel()'s -vc .. vc - kh_even
 * for odd and even scales alike. No float add, so nothing to contract. */
template <typename T>
__device__ inline float decimate_sample(const VmafPicture &pic, const DecimateGeometry &g,
                                        int centre_x, int centre_y)
{
    const int half = g.scale / 2;
    long long sum = 0;
    for (int row = 0; row < g.scale; row++) {
        const int src_y = symmetric_index(centre_y + row - half, g.height);
        const T *line = reinterpret_cast<const T *>(reinterpret_cast<const uint8_t *>(pic.data[0]) +
                                                    src_y * pic.stride[0]);
        for (int column = 0; column < g.scale; column++) {
            const int src_x = symmetric_index(centre_x + column - half, g.width);
            const float sample = __fmul_rn((float)line[src_x], g.sample_scale);
            const float product = __fmul_rn(sample, g.tap_weight);
            sum += __float2ll_rz(__fmul_rn(product, DECIMATE_FIXED_ONE));
        }
    }
    return __fmul_rn(__ll2float_rn(sum), DECIMATE_FIXED_INV);
}

/* Pass 0 for one thread: output pixel (x, y) of both decimated planes. The
 * windows do not overlap (stride == window), so there is no tap reuse a
 * shared-memory tile could exploit. */
template <typename T>
__device__ inline void decimate_pass(const VmafPicture &ref, const VmafPicture &cmp,
                                     const VmafCudaBuffer &ref_out, const VmafCudaBuffer &cmp_out,
                                     const DecimateGeometry &g)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= g.out_width || y >= g.out_height)
        return;
    const int centre_x = (int)x * g.scale;
    const int centre_y = (int)y * g.scale;
    const unsigned dst_idx = y * g.out_width + x;
    VMAF_CUDA_DPTR(float, ref_out.data)[dst_idx] = decimate_sample<T>(ref, g, centre_x, centre_y);
    VMAF_CUDA_DPTR(float, cmp_out.data)[dst_idx] = decimate_sample<T>(cmp, g, centre_x, centre_y);
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

/* The window this thread scores, or false for a thread past the plane. */
__device__ inline bool window_position(unsigned w_final, unsigned h_final, unsigned &x, unsigned &y)
{
    x = blockIdx.x * blockDim.x + threadIdx.x;
    y = blockIdx.y * blockDim.y + threadIdx.y;
    return x < w_final && y < h_final;
}

} // namespace

extern "C" {

/* Pass 0 — decimation (scale > 1): each thread is one output pixel of the
 * out_width x out_height decimated planes. `width` x `height` is the luma
 * plane of both pictures. */
__global__ void calculate_ssim_decimate_8bpc(const VmafPicture ref, const VmafPicture cmp,
                                             VmafCudaBuffer ref_out, VmafCudaBuffer cmp_out,
                                             int width, int height, unsigned out_width,
                                             unsigned out_height, int scale, float sample_scale,
                                             float tap_weight)
{
    DecimateGeometry g;
    g.width = width;
    g.height = height;
    g.out_width = out_width;
    g.out_height = out_height;
    g.scale = scale;
    g.sample_scale = sample_scale;
    g.tap_weight = tap_weight;
    decimate_pass<uint8_t>(ref, cmp, ref_out, cmp_out, g);
}

__global__ void calculate_ssim_decimate_16bpc(const VmafPicture ref, const VmafPicture cmp,
                                              VmafCudaBuffer ref_out, VmafCudaBuffer cmp_out,
                                              int width, int height, unsigned out_width,
                                              unsigned out_height, int scale, float sample_scale,
                                              float tap_weight)
{
    DecimateGeometry g;
    g.width = width;
    g.height = height;
    g.out_width = out_width;
    g.out_height = out_height;
    g.scale = scale;
    g.sample_scale = sample_scale;
    g.tap_weight = tap_weight;
    decimate_pass<uint16_t>(ref, cmp, ref_out, cmp_out, g);
}

/* Pass 1 — horizontal: each thread is one output pixel of the
 * (W-10) × H "valid" buffer. Reads input columns [x, x+10] and
 * writes the 5 horizontal-pass values. */
__global__ void calculate_ssim_horiz_8bpc(const VmafPicture ref, const VmafPicture cmp,
                                          VmafCudaBuffer h_ref_mu, VmafCudaBuffer h_cmp_mu,
                                          VmafCudaBuffer h_ref_sq, VmafCudaBuffer h_cmp_sq,
                                          VmafCudaBuffer h_refcmp, unsigned w_horiz,
                                          unsigned h_horiz)
{
    LumaPicture8 source;
    source.ref = &ref;
    source.cmp = &cmp;
    horizontal_pass(source, horiz_outputs(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp),
                    w_horiz, h_horiz);
}

__global__ void calculate_ssim_horiz_16bpc(const VmafPicture ref, const VmafPicture cmp,
                                           VmafCudaBuffer h_ref_mu, VmafCudaBuffer h_cmp_mu,
                                           VmafCudaBuffer h_ref_sq, VmafCudaBuffer h_cmp_sq,
                                           VmafCudaBuffer h_refcmp, unsigned w_horiz,
                                           unsigned h_horiz, unsigned bpc)
{
    LumaPicture16 source;
    source.ref = &ref;
    source.cmp = &cmp;
    source.scaler = scaler_for_bpc(bpc);
    horizontal_pass(source, horiz_outputs(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp),
                    w_horiz, h_horiz);
}

/* Pass 1 above scale 1: the same taps over the decimated planes, `width`
 * samples per row. */
__global__ void calculate_ssim_horiz_planes(VmafCudaBuffer ref_plane, VmafCudaBuffer cmp_plane,
                                            VmafCudaBuffer h_ref_mu, VmafCudaBuffer h_cmp_mu,
                                            VmafCudaBuffer h_ref_sq, VmafCudaBuffer h_cmp_sq,
                                            VmafCudaBuffer h_refcmp, unsigned w_horiz,
                                            unsigned h_horiz, unsigned width)
{
    LumaPlanes source;
    source.ref = VMAF_CUDA_DPTR(const float, ref_plane.data);
    source.cmp = VMAF_CUDA_DPTR(const float, cmp_plane.data);
    source.width = width;
    horizontal_pass(source, horiz_outputs(h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp),
                    w_horiz, h_horiz);
}

/* Pass 2 — vertical + SSIM combine. Each thread stores its window's
 * l * c * s at the window's raster index of `terms` (w_final * h_final
 * doubles); the host adds them in that order, which is iqa_ssim()'s.
 * __launch_bounds__(128) hints nvcc to budget registers for 128-thread
 * blocks; per ADR-0754 / ADR-0743 precedent. */
__launch_bounds__(BLOCK_SIZE) __global__
    void calculate_ssim_vert_combine(VmafCudaBuffer h_ref_mu_buf, VmafCudaBuffer h_cmp_mu_buf,
                                     VmafCudaBuffer h_ref_sq_buf, VmafCudaBuffer h_cmp_sq_buf,
                                     VmafCudaBuffer h_refcmp_buf, VmafCudaBuffer terms_buf,
                                     unsigned w_horiz, unsigned w_final, unsigned h_final, float c1,
                                     float c2)
{
    unsigned x = 0u;
    unsigned y = 0u;
    if (!window_position(w_final, h_final, x, y))
        return;
    const SsimVertInputs in =
        vert_inputs(h_ref_mu_buf, h_cmp_mu_buf, h_ref_sq_buf, h_cmp_sq_buf, h_refcmp_buf);
    double *const terms = VMAF_CUDA_DPTR(double, terms_buf.data);
    terms[(size_t)y * w_final + x] = ssim_terms(vertical_moments(in, x, y, w_horiz), c1, c2).ssim;
}

/* `enable_lcs` pass 2: `terms` holds LCS_TERMS doubles per window in raster
 * order: the same SSIM value, then its L, C and S terms. iqa_ssim() keeps one
 * accumulator per sum, so the host adds each of the four in index order. */
__launch_bounds__(BLOCK_SIZE) __global__
    void calculate_ssim_vert_combine_lcs(VmafCudaBuffer h_ref_mu_buf, VmafCudaBuffer h_cmp_mu_buf,
                                         VmafCudaBuffer h_ref_sq_buf, VmafCudaBuffer h_cmp_sq_buf,
                                         VmafCudaBuffer h_refcmp_buf, VmafCudaBuffer terms_buf,
                                         unsigned w_horiz, unsigned w_final, unsigned h_final,
                                         float c1, float c2)
{
    unsigned x = 0u;
    unsigned y = 0u;
    if (!window_position(w_final, h_final, x, y))
        return;
    const SsimVertInputs in =
        vert_inputs(h_ref_mu_buf, h_cmp_mu_buf, h_ref_sq_buf, h_cmp_sq_buf, h_refcmp_buf);
    const SsimTerms t = ssim_terms(vertical_moments(in, x, y, w_horiz), c1, c2);
    double *const window =
        VMAF_CUDA_DPTR(double, terms_buf.data) + (((size_t)y * w_final + x) * LCS_TERMS);
    window[0] = t.ssim;
    window[1] = t.l;
    window[2] = t.c;
    window[3] = t.s;
}

} /* extern "C" */
