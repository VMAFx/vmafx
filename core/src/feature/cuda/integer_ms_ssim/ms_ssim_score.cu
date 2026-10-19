/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  CUDA compute kernels for the float_ms_ssim feature extractor
 *  (T7-23 / batch 2 part 2b / ADR-0190). Mirror of the GLSL
 *  shaders in ms_ssim_decimate.comp + ms_ssim.comp byte-for-byte
 *  modulo language differences. Four kernels:
 *
 *    0. ms_ssim_picture_to_float — level 0 of a pyramid from a picture
 *       plane: float_ms_ssim.c's picture_copy() of the plane, on the
 *       device, so no plane goes through the host
 *       (T-CUDA-MS-SSIM-HOST-STAGING-2026-10-06).
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
 *       per-window l/c/s formulas, each term stored at the window's
 *       raster position (l and c as doubles, s as the float it is).
 *       Includes the σ² ≥ 0 clamp before sqrt.
 *
 *  Nothing is reduced on the device. iqa_ssim() adds every window's l, c
 *  and s into one double each, left to right and top to bottom, and a sum of
 *  doubles is its order: the host reads the three planes of a scale back and
 *  adds them in index order (integer_ms_ssim_cuda.c::ms_ssim_scale_sums(),
 *  ADR-1465), then applies the Wang weights for the final product combine.
 *
 *  Numerical contract (ADR-1403). Each kernel reproduces its CPU reference
 *  operation for operation and type for type, and the fatbin builds with
 *  --fmad=false, so a plain `a * b + c` rounds twice as it does on the host:
 *    - decimate: ms_ssim_decimate.c, one fused multiply-add per tap, spelled
 *      __fmaf_rn();
 *    - window sums: iqa_convolve() (iqa/convolve.c), fp32 products
 *      accumulated in fp64 and rounded to fp32 once per pass; carried here
 *      as an exact fp32 pair (MsPair below);
 *    - l / c / s: ssim_variance_scalar() and
 *      ssim_accumulate_default_scalar() (iqa/ssim_tools.c), with their mixed
 *      fp32 / fp64 operands, and the square root as __fsqrt_rn().
 *    - frame sums: the terms are stored, not reduced, and the host adds
 *      them in iqa_ssim()'s raster order (ADR-1465). A per-block sum of the
 *      same terms rounded a per-scale mean to the neighbouring float on rare
 *      frames (T-CUDA-FLOAT-MS-SSIM-FRAME-SUM-ORDER-2026-10-02).
 *  What is left of ADR-1403's caveat is the 48-bit window sums. Measured on
 *  an RTX 4090: every frame of the Netflix 576x324 pair, both 1080p
 *  checkerboard pairs and BBB 3840x2160 is bit-identical to the CPU
 *  extractor, built with nvcc and with clang's CUDA driver alike.
 */

#include "cuda_device_ptr.cuh"
#include "cuda_helper.cuh"
#include "cuda/integer_ms_ssim_cuda.h"
#include "common.h"

#define BLOCK_X 16
#define BLOCK_Y 8
#define BLOCK_SIZE (BLOCK_X * BLOCK_Y)
#define K 11
#define LPF_LEN 9
#define LPF_HALF 4

namespace
{

__device__ const float G[K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

__device__ const float LPF[LPF_LEN] = {
    0.026727f, -0.016828f, -0.078201f, 0.266846f, 0.602914f,
    0.266846f, -0.078201f, -0.016828f, 0.026727f,
};

/* Period-2n mirror — handles sub-kernel-radius inputs the
 * single-reflect form leaves out of bounds. Matches
 * ms_ssim_decimate_mirror() in ms_ssim_decimate.c. */
__device__ inline int mirror_idx(int idx, int n)
{
    const int period = 2 * n;
    int r = idx % period;
    if (r < 0) {
        r += period;
    }
    if (r >= n) {
        r = period - r - 1;
    }
    return r;
}

/* A window sum as iqa_convolve() forms it: fp32 terms added in fp64 and
 * rounded to fp32 once. The sum is carried as an fp32 pair instead of an
 * fp64 value: `hi` is the running fp32 sum and `lo` collects the exact
 * rounding error of every step (Knuth's two-sum, six fp32 operations), so
 * hi + lo holds the 11-term sum to about 48 bits and rounds to the fp32
 * value the reference's 53-bit sum rounds to, unless the exact sum lies
 * within about 2^-18 ulp of an fp32 rounding boundary. Measured on an RTX 4090
 * (ADR-1403): the pair and a plain fp64 accumulator give the same frame
 * scores, all bit-identical to the CPU, and the fp64 one costs 3.4 ms more per
 * 3840x2160 frame (10.7 -> 14.1), because consumer GPUs run fp64 at a small
 * fraction of their fp32 rate. The intrinsics are never contracted. */
struct MsPair {
    float hi = 0.0f;
    float lo = 0.0f;
};

__device__ __forceinline__ void ms_pair_add(MsPair &sum, float term)
{
    const float s = __fadd_rn(sum.hi, term);
    const float term_virtual = __fsub_rn(s, sum.hi);
    const float hi_virtual = __fsub_rn(s, term_virtual);
    const float error = __fadd_rn(__fsub_rn(sum.hi, hi_virtual), __fsub_rn(term, term_virtual));
    sum.hi = s;
    sum.lo = __fadd_rn(sum.lo, error);
}

__device__ __forceinline__ float ms_pair_round(const MsPair &sum)
{
    return __fadd_rn(sum.hi, sum.lo);
}

/* The five window statistics of one pixel after both passes. */
struct MsStats {
    float ref_mu;
    float cmp_mu;
    float ref_sq;
    float cmp_sq;
    float refcmp;
};

/* One pixel's luminance, contrast and structure terms. */
struct MsLcs {
    double l;
    double c;
    double s;
};

/* The five horizontal-pass planes ms_ssim_horiz wrote. */
struct MsHorizPlanes {
    const float *ref_mu;
    const float *cmp_mu;
    const float *ref_sq;
    const float *cmp_sq;
    const float *refcmp;
};

/* iqa_convolve_vertical_pass(): fp32 products, the reference's fp64 sum as
 * an MsPair, one rounding to fp32. `first` is the top tap's index and
 * `stride` the row pitch of the horizontal-pass planes. */
__device__ __forceinline__ MsStats ms_vertical_stats(const MsHorizPlanes &h, unsigned first,
                                                     unsigned stride)
{
    MsPair ref_mu_v;
    MsPair cmp_mu_v;
    MsPair ref_sq_v;
    MsPair cmp_sq_v;
    MsPair refcmp_v;
    for (int v = 0; v < K; v++) {
        const unsigned src_idx = first + (unsigned)v * stride;
        const float w = G[v];
        ms_pair_add(ref_mu_v, __fmul_rn(h.ref_mu[src_idx], w));
        ms_pair_add(cmp_mu_v, __fmul_rn(h.cmp_mu[src_idx], w));
        ms_pair_add(ref_sq_v, __fmul_rn(h.ref_sq[src_idx], w));
        ms_pair_add(cmp_sq_v, __fmul_rn(h.cmp_sq[src_idx], w));
        ms_pair_add(refcmp_v, __fmul_rn(h.refcmp[src_idx], w));
    }
    MsStats out;
    out.ref_mu = ms_pair_round(ref_mu_v);
    out.cmp_mu = ms_pair_round(cmp_mu_v);
    out.ref_sq = ms_pair_round(ref_sq_v);
    out.cmp_sq = ms_pair_round(cmp_sq_v);
    out.refcmp = ms_pair_round(refcmp_v);
    return out;
}

/* ssim_variance_scalar() then ssim_accumulate_default_scalar(), type for
 * type (ADR-0139 / ADR-0990 / ADR-1403): the variances and the covariance are
 * fp32 with the reference's clamp, the stabilisation constants are fp32,
 * `2.0 *` promotes the numerators of l and c to fp64, their denominators are
 * fp32 sums, and s is an fp32 quotient. */
__device__ __forceinline__ MsLcs ms_lcs_terms(const MsStats &st, float C1, float C2, float C3)
{
    const float ref_mu = st.ref_mu;
    const float cmp_mu = st.cmp_mu;
    /* Clamp σ² ≥ 0 before sqrt — matches MAX(0, ...) in
     * iqa/ssim_tools.c::ssim_variance_scalar. */
    const float ref_var = fmaxf(st.ref_sq - ref_mu * ref_mu, 0.0f);
    const float cmp_var = fmaxf(st.cmp_sq - cmp_mu * cmp_mu, 0.0f);
    const float covar = st.refcmp - ref_mu * cmp_mu;
    /* __fsqrt_rn(), not sqrtf(): nvcc rounds both correctly, clang's CUDA
     * driver turns sqrtf() into the approximate device square root. */
    const float sigma_xy_geom = __fsqrt_rn(ref_var * cmp_var);
    const float clamped_covar = (covar < 0.0f && sigma_xy_geom <= 0.0f) ? 0.0f : covar;
    MsLcs out;
    out.l = (2.0 * (double)ref_mu * (double)cmp_mu + (double)C1) /
            (double)(ref_mu * ref_mu + cmp_mu * cmp_mu + C1);
    out.c = (2.0 * (double)sigma_xy_geom + (double)C2) / (double)(ref_var + cmp_var + C2);
    out.s = (double)((clamped_covar + C3) / (sigma_xy_geom + C3));
    return out;
}

} /* namespace */

extern "C" {

/* Level 0 of a pyramid from a picture plane: float_ms_ssim.c's
 * picture_copy() of the plane, sample for sample (offset 0), into `dst`
 * (w x h floats, rows of w). With `two_byte` a little-endian 16-bit sample
 * divided by `scaler` (4, 16, 256 at 10, 12, 16 bits; a power of two, so the
 * quotient is exact); otherwise one byte per sample, as picture_copy() reads
 * at 8 bits and at the depths it has no case for. Bytes are loaded one at a
 * time, so a plane may start at any byte. The host passes the level's
 * CUdeviceptr as `dst`, a kernel parameter of pointer type. */
__global__ void ms_ssim_picture_to_float(const uint8_t *src, size_t pitch, float *dst, unsigned w,
                                         unsigned h, unsigned two_byte, float scaler)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    const uint8_t *const row = src + (size_t)y * pitch;
    float *const out = dst + (size_t)y * w;
    if (two_byte) {
        const size_t at = (size_t)2u * x;
        const unsigned v = (unsigned)row[at] | ((unsigned)row[at + 1u] << 8u);
        out[x] = (float)v / scaler;
    } else {
        out[x] = (float)row[x];
    }
}

/* Decimate: 9-tap 9/7 biorthogonal separable LPF + 2× downsample.
 * One thread per output pixel.
 *
 * Every tap is one fused multiply-add, `acc = fma(sample, tap, acc)`, because
 * the reference is: ms_ssim_decimate.c accumulates with vmaf_fmaf_exact() and
 * its AVX2 / AVX-512 / NEON twins with fmadd (ADR-0891). The fusion is part
 * of the arithmetic this kernel reproduces, so it is spelled with
 * __fmaf_rn() and does not depend on the compiler contracting `a * b + c`:
 * the fatbin builds with --fmad=false like every other CUDA kernel
 * (ADR-1403). The horizontal sum of each source row is the reference's
 * h-pass value for that row, and the nine row sums combine as its v-pass
 * does, in the same tap order. */
__global__ void ms_ssim_decimate(VmafCudaBuffer src, VmafCudaBuffer dst, unsigned w, unsigned h,
                                 unsigned w_out, unsigned h_out)
{
    const unsigned x_out = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y_out = blockIdx.y * blockDim.y + threadIdx.y;
    if (x_out >= w_out || y_out >= h_out) {
        return;
    }

    const float *src_buf = VMAF_CUDA_DPTR(const float, src.data);
    float *dst_buf = VMAF_CUDA_DPTR(float, dst.data);

    const int x_src = (int)x_out * 2;
    const int y_src = (int)y_out * 2;
    float acc = 0.0f;
    for (int kv = 0; kv < LPF_LEN; ++kv) {
        const int yi = mirror_idx(y_src + kv - LPF_HALF, (int)h);
        float row_acc = 0.0f;
        for (int ku = 0; ku < LPF_LEN; ++ku) {
            const int xi = mirror_idx(x_src + ku - LPF_HALF, (int)w);
            row_acc = __fmaf_rn(src_buf[yi * (int)w + xi], LPF[ku], row_acc);
        }
        acc = __fmaf_rn(row_acc, LPF[kv], acc);
    }
    dst_buf[y_out * w_out + x_out] = acc;
}

/* SSIM horizontal pass: read float ref/cmp at (W × H), write
 * 5 horizontal-pass values at ((W-10) × H). Same shape as
 * ssim_score.cu's horiz_8bpc but operating on already-normalised
 * float input.
 *
 * iqa_convolve_horizontal_pass(): `prod = img * tap` in fp32, `sum += prod`
 * in fp64, and the row value is the sum rounded to fp32 (the window is
 * normalised, so the reference's `* scale` multiplies by 1). The squares
 * and the cross product are fp32 products formed first, as
 * ssim_precompute_scalar() stores them. A plain fp32 running sum here was
 * only close to that, and only with the compiler fusing each step. */
__global__ void ms_ssim_horiz(VmafCudaBuffer ref_in, VmafCudaBuffer cmp_in, VmafCudaBuffer h_ref_mu,
                              VmafCudaBuffer h_cmp_mu, VmafCudaBuffer h_ref_sq,
                              VmafCudaBuffer h_cmp_sq, VmafCudaBuffer h_refcmp, unsigned width,
                              unsigned w_horiz, unsigned h_horiz)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz) {
        return;
    }

    const float *ref = VMAF_CUDA_DPTR(const float, ref_in.data);
    const float *cmp = VMAF_CUDA_DPTR(const float, cmp_in.data);

    MsPair ref_mu_h;
    MsPair cmp_mu_h;
    MsPair ref_sq_h;
    MsPair cmp_sq_h;
    MsPair refcmp_h;
    for (int u = 0; u < K; u++) {
        const unsigned src_idx = y * width + (x + (unsigned)u);
        const float r = ref[src_idx];
        const float c = cmp[src_idx];
        const float w = G[u];
        ms_pair_add(ref_mu_h, __fmul_rn(r, w));
        ms_pair_add(cmp_mu_h, __fmul_rn(c, w));
        ms_pair_add(ref_sq_h, __fmul_rn(__fmul_rn(r, r), w));
        ms_pair_add(cmp_sq_h, __fmul_rn(__fmul_rn(c, c), w));
        ms_pair_add(refcmp_h, __fmul_rn(__fmul_rn(r, c), w));
    }
    const unsigned dst_idx = y * w_horiz + x;
    VMAF_CUDA_DPTR(float, h_ref_mu.data)[dst_idx] = ms_pair_round(ref_mu_h);
    VMAF_CUDA_DPTR(float, h_cmp_mu.data)[dst_idx] = ms_pair_round(cmp_mu_h);
    VMAF_CUDA_DPTR(float, h_ref_sq.data)[dst_idx] = ms_pair_round(ref_sq_h);
    VMAF_CUDA_DPTR(float, h_cmp_sq.data)[dst_idx] = ms_pair_round(cmp_sq_h);
    VMAF_CUDA_DPTR(float, h_refcmp.data)[dst_idx] = ms_pair_round(refcmp_h);
}

/* Vertical pass + per-window l / c / s, stored at the window's raster index.
 *
 * ms_vertical_stats() and ms_lcs_terms() hold the reference's arithmetic.
 * `l_terms` and `c_terms` hold w_final * h_final doubles, `s_terms` as many
 * floats: s is an fp32 quotient the reference widens when it adds it, and the
 * host widens it the same way. Nothing is reduced here; the host adds each
 * plane in index order, which is iqa_ssim()'s. c1..c3 arrive as doubles
 * holding the reference's fp32 constants exactly. */
__global__ void ms_ssim_vert_lcs(VmafCudaBuffer h_ref_mu_buf, VmafCudaBuffer h_cmp_mu_buf,
                                 VmafCudaBuffer h_ref_sq_buf, VmafCudaBuffer h_cmp_sq_buf,
                                 VmafCudaBuffer h_refcmp_buf, VmafCudaBuffer l_terms,
                                 VmafCudaBuffer c_terms, VmafCudaBuffer s_terms, unsigned w_horiz,
                                 unsigned w_final, unsigned h_final, double c1, double c2,
                                 double c3)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_final || y >= h_final) {
        return;
    }
    MsHorizPlanes planes;
    planes.ref_mu = VMAF_CUDA_DPTR(const float, h_ref_mu_buf.data);
    planes.cmp_mu = VMAF_CUDA_DPTR(const float, h_cmp_mu_buf.data);
    planes.ref_sq = VMAF_CUDA_DPTR(const float, h_ref_sq_buf.data);
    planes.cmp_sq = VMAF_CUDA_DPTR(const float, h_cmp_sq_buf.data);
    planes.refcmp = VMAF_CUDA_DPTR(const float, h_refcmp_buf.data);
    const MsStats stats = ms_vertical_stats(planes, y * w_horiz + x, w_horiz);
    const MsLcs terms = ms_lcs_terms(stats, (float)c1, (float)c2, (float)c3);
    const size_t window = (size_t)y * w_final + x;
    VMAF_CUDA_DPTR(double, l_terms.data)[window] = terms.l;
    VMAF_CUDA_DPTR(double, c_terms.data)[window] = terms.c;
    /* Exact: terms.s is the fp32 quotient, widened. */
    VMAF_CUDA_DPTR(float, s_terms.data)[window] = (float)terms.s;
}

} /* extern "C" */
