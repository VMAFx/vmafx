/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP compute kernels for the integer_ms_ssim feature extractor
 *  (HIP ninth consumer / ADR-0285). Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/integer_ms_ssim/ms_ssim_score.cu`.
 *
 *  Three kernels — same algorithm as the CUDA twin, adapted for HIP:
 *
 *    1. ms_ssim_decimate — 9-tap 9/7 biorthogonal LPF + 2× downsample,
 *       period-2n mirror boundary. One thread per output pixel. Reads
 *       input float buffer, writes downsampled float buffer.
 *
 *    2. ms_ssim_horiz — horizontal 11-tap separable Gaussian over
 *       ref / cmp / ref² / cmp² / ref·cmp; operates on already-
 *       normalised float input (pyramid levels are float). Output:
 *       five intermediate float buffers, each (W-10) × H.
 *
 *    3. ms_ssim_vert_lcs — vertical 11-tap + per-pixel l/c/s formulas
 *       + per-block float partial sums (l, c, s). Grid sized over
 *       (W-10) × (H-10). One triple (l, c, s) per block.
 *
 *  HIP adaptation from CUDA:
 *  - Warp size varies: 64 on GCN/RDNA1 (wave64), 32 on RDNA2+ (wave32).
 *    `warpSize` is read at runtime; shared-memory is sized for the minimum
 *    warp size (32) so it fits the maximum warps per block on wave32:
 *    BLOCK_SIZE / MIN_WARP_SIZE = 128 / 32 = 4 slots.
 *  - Raw pointer parameters instead of VmafCudaBuffer wrappers.
 *  - `__shfl_down` without mask (HIP convention).
 *  - No `reinterpret_cast` on VmafCudaBuffer — raw float* args.
 *
 *  Host accumulates partials in double per scale, applies the Wang
 *  weights for the final product combine. See integer_ms_ssim_hip.c.
 */

#include <hip/hip_runtime.h>

#define BLOCK_X 16
#define BLOCK_Y 8
#define BLOCK_SIZE (BLOCK_X * BLOCK_Y)
#define K 11
#define LPF_LEN 9
#define LPF_HALF 4

/* Minimum HIP warp size across supported targets (32 on RDNA2+, 64 on GCN/RDNA1).
 * Shared memory is sized for the worst case (wave32 = most warps per block).
 * The runtime `warpSize` device variable is used in reduction loops and lane
 * detection so the same binary runs correctly on both wave32 and wave64. */
#define MIN_WARP_SIZE    32
#define WARPS_PER_BLOCK  (BLOCK_SIZE / MIN_WARP_SIZE)

__device__ static const float G[K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

__device__ static const float LPF[LPF_LEN] = {
    0.026727f, -0.016828f, -0.078201f, 0.266846f, 0.602914f,
    0.266846f, -0.078201f, -0.016828f, 0.026727f,
};

/* Period-2n mirror — matches ms_ssim_decimate_mirror() in ms_ssim_decimate.c
 * and the CUDA twin's mirror_idx(). */
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

/* ms_ssim_warp_reduce is no longer called after the ADR-0990 double
 * promotion; the inline reduction loop in ms_ssim_vert_lcs operates
 * directly on double wl/wc/ws.  Kept as a dead helper to avoid a diff
 * hunk that would conflict during a Netflix upstream sync.
 * Updated to use runtime warpSize for wave32/wave64 portability. */
__device__ __forceinline__ float ms_ssim_warp_reduce(float v)
{
    for (int off = (int)warpSize / 2; off > 0; off >>= 1)
        v += __shfl_down(v, off);
    return v;
}

extern "C" {

/**
 * Decimate: 9-tap 9/7 biorthogonal separable LPF + 2× downsample.
 * One thread per output pixel. Period-2n mirror boundary.
 * src: float input (w × h), dst: float output (w_out × h_out).
 */
__global__ void ms_ssim_decimate(const float *__restrict__ src, float *__restrict__ dst,
                                 unsigned w, unsigned h,
                                 unsigned w_out, unsigned h_out)
{
    const unsigned x_out = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y_out = blockIdx.y * blockDim.y + threadIdx.y;
    if (x_out >= w_out || y_out >= h_out)
        return;

    int x_src = (int)x_out * 2;
    int y_src = (int)y_out * 2;
    float acc = 0.0f;
    for (int kv = 0; kv < LPF_LEN; ++kv) {
        int yi = mirror_idx(y_src + kv - LPF_HALF, (int)h);
        float row_acc = 0.0f;
        for (int ku = 0; ku < LPF_LEN; ++ku) {
            int xi = mirror_idx(x_src + ku - LPF_HALF, (int)w);
            row_acc += src[yi * (int)w + xi] * LPF[ku];
        }
        acc += row_acc * LPF[kv];
    }
    dst[y_out * w_out + x_out] = acc;
}

/**
 * SSIM horizontal pass: read float ref/cmp at (W × H), write
 * 5 horizontal-pass values at ((W-10) × H). Operates on already-
 * normalised float input (pyramid levels produced by ms_ssim_decimate).
 */
__global__ void ms_ssim_horiz(const float *__restrict__ ref_in,
                               const float *__restrict__ cmp_in,
                               float *__restrict__ h_ref_mu,
                               float *__restrict__ h_cmp_mu,
                               float *__restrict__ h_ref_sq,
                               float *__restrict__ h_cmp_sq,
                               float *__restrict__ h_refcmp,
                               unsigned width,
                               unsigned w_horiz,
                               unsigned h_horiz)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w_horiz || y >= h_horiz)
        return;

    float ref_mu_h = 0.0f, cmp_mu_h = 0.0f;
    float ref_sq_h = 0.0f, cmp_sq_h = 0.0f, refcmp_h = 0.0f;
    for (int u = 0; u < K; u++) {
        const unsigned src_idx = y * width + (x + (unsigned)u);
        const float r = ref_in[src_idx];
        const float c = cmp_in[src_idx];
        const float w = G[u];
        ref_mu_h += w * r;
        cmp_mu_h += w * c;
        ref_sq_h += w * (r * r);
        cmp_sq_h += w * (c * c);
        refcmp_h += w * (r * c);
    }
    const unsigned dst_idx = y * w_horiz + x;
    h_ref_mu[dst_idx] = ref_mu_h;
    h_cmp_mu[dst_idx] = cmp_mu_h;
    h_ref_sq[dst_idx] = ref_sq_h;
    h_cmp_sq[dst_idx] = cmp_sq_h;
    h_refcmp[dst_idx] = refcmp_h;
}

/**
 * Vertical pass + per-pixel l/c/s + per-block 3-output partial sums.
 * Mirrors ms_ssim_vert_lcs in the CUDA twin including σ² ≥ 0 clamp.
 * Grid sized over (W-10) × (H-10) = w_final × h_final.
 * Each block writes one (l, c, s) triple into l_partials / c_partials /
 * s_partials at [blockIdx.y * gridDim.x + blockIdx.x].
 *
 * ADR-0139 / ADR-0990 double-precision fix: L/C/S are computed in
 * double to match the CPU scalar reference in ssim_tools.c which uses
 * `2.0 * ref_mu * cmp_mu` (double literal promoting the expression).
 * The warp and block reductions also run in double so accumulated
 * rounding over 33k pixels at scale 0 stays within the places=4 gate.
 * GCN/RDNA supports native fp64 __shfl_down; WARPS_PER_BLOCK=4 (wave32
 * worst case — over-allocated on wave64 where only 2 slots are used).
 */
__global__ void ms_ssim_vert_lcs(const float *__restrict__ h_ref_mu_buf,
                                  const float *__restrict__ h_cmp_mu_buf,
                                  const float *__restrict__ h_ref_sq_buf,
                                  const float *__restrict__ h_cmp_sq_buf,
                                  const float *__restrict__ h_refcmp_buf,
                                  double *__restrict__ l_partials,
                                  double *__restrict__ c_partials,
                                  double *__restrict__ s_partials,
                                  unsigned w_horiz,
                                  unsigned w_final,
                                  unsigned h_final,
                                  double c1,
                                  double c2,
                                  double c3)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    /* L/C/S in double — matches ssim_tools.c scalar reference which
     * uses `2.0 *` (double literal) for the numerators. */
    double my_l = 0.0, my_c = 0.0, my_s = 0.0;
    if (x < w_final && y < h_final) {
        float ref_mu = 0.0f, cmp_mu = 0.0f;
        float ref_sq = 0.0f, cmp_sq = 0.0f, refcmp = 0.0f;
        for (int v = 0; v < K; v++) {
            const unsigned src_idx = (y + (unsigned)v) * w_horiz + x;
            const float w = G[v];
            ref_mu += w * h_ref_mu_buf[src_idx];
            cmp_mu += w * h_cmp_mu_buf[src_idx];
            ref_sq += w * h_ref_sq_buf[src_idx];
            cmp_sq += w * h_cmp_sq_buf[src_idx];
            refcmp += w * h_refcmp_buf[src_idx];
        }
        /* Clamp σ² ≥ 0 before sqrt — matches MAX(0, ...) in
         * iqa/ssim_tools.c::ssim_variance_scalar. */
        const float ref_var = ref_sq - ref_mu * ref_mu < 0.0f
                                  ? 0.0f : ref_sq - ref_mu * ref_mu;
        const float cmp_var = cmp_sq - cmp_mu * cmp_mu < 0.0f
                                  ? 0.0f : cmp_sq - cmp_mu * cmp_mu;
        const float covar = refcmp - ref_mu * cmp_mu;
        const float sigma_xy_geom = sqrtf(ref_var * cmp_var);
        const float clamped_covar =
            (covar < 0.0f && sigma_xy_geom <= 0.0f) ? 0.0f : covar;

        /* Double literals match the CPU scalar: `2.0 *` promotes the
         * float operands to double before the multiply (ADR-0139). */
        my_l = (2.0 * (double)ref_mu * (double)cmp_mu + c1) /
               ((double)ref_mu * (double)ref_mu + (double)cmp_mu * (double)cmp_mu + c1);
        my_c = (2.0 * (double)sigma_xy_geom + c2) / ((double)ref_var + (double)cmp_var + c2);
        my_s = ((double)clamped_covar + c3) / ((double)sigma_xy_geom + c3);
    }

    /* 3-output per-block tree reduction using shared warp partials.
     * Warp size = 64 on GCN/RDNA; BLOCK_SIZE=128 → WARPS_PER_BLOCK=2.
     * Shared arrays hold double so warp-reduction precision is preserved. */
    __shared__ double s_l_warp[WARPS_PER_BLOCK];
    __shared__ double s_c_warp[WARPS_PER_BLOCK];
    __shared__ double s_s_warp[WARPS_PER_BLOCK];

    double wl = my_l, wc = my_c, ws = my_s;
    for (int off = (int)warpSize / 2; off > 0; off >>= 1) {
        wl += __shfl_down(wl, off);
        wc += __shfl_down(wc, off);
        ws += __shfl_down(ws, off);
    }

    const unsigned lane    = lid % (unsigned)warpSize;
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
