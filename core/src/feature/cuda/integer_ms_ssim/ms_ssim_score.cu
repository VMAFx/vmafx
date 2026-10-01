/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
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
 *  What is left differs from the CPU only below the last bit of an fp32
 *  per-scale mean: the order of the fp64 l / c / s sums (blocks here, raster
 *  order there) and the 48-bit window sums. The host rounds each mean to
 *  fp32 as iqa_ssim() does, which absorbs it. Measured on an RTX 4090: every
 *  frame of the Netflix 576x324 pair, both 1080p checkerboard pairs and BBB
 *  3840x2160 is bit-identical to the CPU extractor, built with nvcc and with
 *  clang's CUDA driver alike.
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
    float hi;
    float lo;
};

__device__ static __forceinline__ void ms_pair_add(MsPair &sum, float term)
{
    const float s = __fadd_rn(sum.hi, term);
    const float term_virtual = __fsub_rn(s, sum.hi);
    const float hi_virtual = __fsub_rn(s, term_virtual);
    const float error = __fadd_rn(__fsub_rn(sum.hi, hi_virtual), __fsub_rn(term, term_virtual));
    sum.hi = s;
    sum.lo = __fadd_rn(sum.lo, error);
}

__device__ static __forceinline__ float ms_pair_round(const MsPair &sum)
{
    return __fadd_rn(sum.hi, sum.lo);
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
    if (x >= w_horiz || y >= h_horiz)
        return;

    const float *ref = reinterpret_cast<const float *>(ref_in.data);
    const float *cmp = reinterpret_cast<const float *>(cmp_in.data);

    MsPair ref_mu_h = {0.0f, 0.0f};
    MsPair cmp_mu_h = {0.0f, 0.0f};
    MsPair ref_sq_h = {0.0f, 0.0f};
    MsPair cmp_sq_h = {0.0f, 0.0f};
    MsPair refcmp_h = {0.0f, 0.0f};
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
    reinterpret_cast<float *>(h_ref_mu.data)[dst_idx] = ms_pair_round(ref_mu_h);
    reinterpret_cast<float *>(h_cmp_mu.data)[dst_idx] = ms_pair_round(cmp_mu_h);
    reinterpret_cast<float *>(h_ref_sq.data)[dst_idx] = ms_pair_round(ref_sq_h);
    reinterpret_cast<float *>(h_cmp_sq.data)[dst_idx] = ms_pair_round(cmp_sq_h);
    reinterpret_cast<float *>(h_refcmp.data)[dst_idx] = ms_pair_round(refcmp_h);
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
__device__ static __forceinline__ MsStats ms_vertical_stats(const MsHorizPlanes &h, unsigned first,
                                                            unsigned stride)
{
    MsPair ref_mu_v = {0.0f, 0.0f};
    MsPair cmp_mu_v = {0.0f, 0.0f};
    MsPair ref_sq_v = {0.0f, 0.0f};
    MsPair cmp_sq_v = {0.0f, 0.0f};
    MsPair refcmp_v = {0.0f, 0.0f};
    for (int v = 0; v < K; v++) {
        const unsigned src_idx = first + (unsigned)v * stride;
        const float w = G[v];
        ms_pair_add(ref_mu_v, __fmul_rn(h.ref_mu[src_idx], w));
        ms_pair_add(cmp_mu_v, __fmul_rn(h.cmp_mu[src_idx], w));
        ms_pair_add(ref_sq_v, __fmul_rn(h.ref_sq[src_idx], w));
        ms_pair_add(cmp_sq_v, __fmul_rn(h.cmp_sq[src_idx], w));
        ms_pair_add(refcmp_v, __fmul_rn(h.refcmp[src_idx], w));
    }
    return {ms_pair_round(ref_mu_v), ms_pair_round(cmp_mu_v), ms_pair_round(ref_sq_v),
            ms_pair_round(cmp_sq_v), ms_pair_round(refcmp_v)};
}

/* ssim_variance_scalar() then ssim_accumulate_default_scalar(), type for
 * type (ADR-0139 / ADR-0990 / ADR-1403): the variances and the covariance are
 * fp32 with the reference's clamp, the stabilisation constants are fp32,
 * `2.0 *` promotes the numerators of l and c to fp64, their denominators are
 * fp32 sums, and s is an fp32 quotient. */
__device__ static __forceinline__ MsLcs ms_lcs_terms(const MsStats &st, float C1, float C2,
                                                     float C3)
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

/* Vertical pass + per-pixel l/c/s + per-block 3-output partial sums.
 *
 * ms_vertical_stats() and ms_lcs_terms() hold the reference's arithmetic.
 * The warp and block reductions run in double like the CPU's l / c / s sums;
 * their order is the one thing that differs from the reference. c1..c3
 * arrive as doubles holding the reference's fp32 constants exactly.
 * CUDA supports double __shfl_down_sync on sm_30+; all VMAF-supported
 * devices are sm_52+. */
__global__ void ms_ssim_vert_lcs(VmafCudaBuffer h_ref_mu_buf, VmafCudaBuffer h_cmp_mu_buf,
                                 VmafCudaBuffer h_ref_sq_buf, VmafCudaBuffer h_cmp_sq_buf,
                                 VmafCudaBuffer h_refcmp_buf, VmafCudaBuffer l_partials,
                                 VmafCudaBuffer c_partials, VmafCudaBuffer s_partials,
                                 unsigned w_horiz, unsigned w_final, unsigned h_final, double c1,
                                 double c2, double c3)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const MsHorizPlanes planes = {
        reinterpret_cast<const float *>(h_ref_mu_buf.data),
        reinterpret_cast<const float *>(h_cmp_mu_buf.data),
        reinterpret_cast<const float *>(h_ref_sq_buf.data),
        reinterpret_cast<const float *>(h_cmp_sq_buf.data),
        reinterpret_cast<const float *>(h_refcmp_buf.data),
    };

    double my_l = 0.0, my_c = 0.0, my_s = 0.0;
    if (x < w_final && y < h_final) {
        const MsStats stats = ms_vertical_stats(planes, y * w_horiz + x, w_horiz);
        const MsLcs terms = ms_lcs_terms(stats, (float)c1, (float)c2, (float)c3);
        my_l = terms.l;
        my_c = terms.c;
        my_s = terms.s;
    }

    /* 3 parallel per-block tree reductions in shared memory.
     * Shared arrays hold double so warp-reduction precision is preserved. */
    __shared__ double s_l_warp[BLOCK_SIZE / 32];
    __shared__ double s_c_warp[BLOCK_SIZE / 32];
    __shared__ double s_s_warp[BLOCK_SIZE / 32];
    double wl = my_l, wc = my_c, ws = my_s;
    for (int off = 16; off > 0; off >>= 1) {
        wl += __shfl_down_sync(0xffffffff, wl, (unsigned)off);
        wc += __shfl_down_sync(0xffffffff, wc, (unsigned)off);
        ws += __shfl_down_sync(0xffffffff, ws, (unsigned)off);
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
        double bl = 0.0, bc = 0.0, bs = 0.0;
        for (int i = 0; i < BLOCK_SIZE / 32; i++) {
            bl += s_l_warp[i];
            bc += s_c_warp[i];
            bs += s_s_warp[i];
        }
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        reinterpret_cast<double *>(l_partials.data)[block_idx] = bl;
        reinterpret_cast<double *>(c_partials.data)[block_idx] = bc;
        reinterpret_cast<double *>(s_partials.data)[block_idx] = bs;
    }
}

} /* extern "C" */
