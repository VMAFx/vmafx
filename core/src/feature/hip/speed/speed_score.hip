/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP compute kernels for speed_chroma and speed_temporal feature
 *  extractors (ADR-0567).  Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/speed/speed_score.cu`.
 *
 *  Algorithm: SpEED (Spatial Efficient Entropy Descriptor) tile-parallel
 *  work — means, covariance accumulation, independent-term scatter,
 *  backward substitution, and per-tile entropy/score.  The 25×25
 *  eigendecomposition and QR factorisation happen on the host (CPU)
 *  between kernel batches K2/K3 and K4; that is not forwarding, it is
 *  a mathematical constraint (fixed serial 25×25 op vs tile-parallel GPU
 *  work at 28–312 tiles per frame).
 *
 *  HIP adaptation from CUDA twin:
 *  - Warp size varies: 64 on GCN/RDNA1 (wave64), 32 on RDNA2+ (wave32).
 *    The `speed_solve_hip_kernel` is launched with blockDim.x = wavefront
 *    size (HIP_WARP_SIZE) by the host; on RDNA2+ the host must query
 *    wavefront size at runtime and pass 32 instead of 64.
 *  - `__shfl_down` used without mask (HIP convention).
 *  - `extern "C"` linkage preserved for Module API lookup.
 *  - `__syncwarp()` replaced by `__builtin_amdgcn_wave_barrier()` inside
 *    the solve kernel (one wavefront per column).
 *  - Entry-point names are `speed_*_hip_kernel` (vs `speed_*_kernel` in
 *    CUDA) so host-side hipModuleGetFunction calls resolve correctly.
 *
 *  Kernel names exported:
 *    speed_means_hip_kernel
 *    speed_cov_hip_kernel
 *    speed_indterm_hip_kernel
 *    speed_solve_hip_kernel
 *    speed_score_hip_kernel
 */

#include <hip/hip_runtime.h>
#include <stdint.h>
#include <math.h>

#define SP_BLOCK_SIZE 5u
#define SP_ELEMENTS   25u   /* SP_BLOCK_SIZE * SP_BLOCK_SIZE */
/* HIP_WARP_SIZE is the launch-time blockDim.x passed by the host to
 * speed_solve_hip_kernel.  It must equal the device's actual wavefront
 * size (64 on GCN/RDNA1, 32 on RDNA2+).  The host queries this via
 * hipDeviceGetAttribute(HIP_DEVICE_ATTRIBUTE_WARP_SIZE) before launch.
 * The kernel guard `lane >= SP_ELEMENTS` ensures correctness for both
 * sizes (25 active threads out of 32 or 64 per wavefront). */
#define HIP_WARP_SIZE 64u   /* default for GCN/RDNA1; host overrides on RDNA2+ */

/* ------------------------------------------------------------------ */
/* Kernel 1: global-window means[SP_ELEMENTS] (one thread per element) */
/* ------------------------------------------------------------------ */

extern "C" __global__ void speed_means_hip_kernel(
    const float *__restrict__ plane,
    float *__restrict__ means,
    uint32_t op_w,
    uint32_t stride_px,
    uint32_t num_blocks_h,
    uint32_t num_blocks,
    uint32_t submatrix_w,
    uint32_t submatrix_h)
{
    /* One thread per element position (er, ec) in the 5x5 block template.
     * CPU parity (compute_mean, called from compute_covariance_matrix): each of
     * the 25 means is over a single GLOBAL window across the whole truncated
     * plane at start (er, ec) — NOT per-tile. The historic per-tile origin
     * (tile_y*5+er) over-read the plane and produced 25*num_blocks block-local
     * means, giving a wrong covariance and ~7x-low SpEED scores. means[] is
     * over-allocated (25*num_blocks); only [0,25) are written/read now. */
    (void)op_w;
    (void)num_blocks_h;
    (void)num_blocks;
    const uint32_t elem = blockIdx.x * blockDim.x + threadIdx.x;
    if (elem >= SP_ELEMENTS)
        return;
    const uint32_t er = elem / SP_BLOCK_SIZE;
    const uint32_t ec = elem % SP_BLOCK_SIZE;

    double acc = 0.0;
    for (uint32_t i = 0; i < submatrix_h; ++i)
        for (uint32_t j = 0; j < submatrix_w; ++j)
            acc += (double)plane[(er + i) * stride_px + (ec + j)];
    means[elem] = (float)(acc / (double)(submatrix_w * submatrix_h));
}

/* ------------------------------------------------------------------ */
/* Kernel 2: covariance matrix (SP_ELEMENTS×SP_ELEMENTS work-groups)  */
/* ------------------------------------------------------------------ */

extern "C" __global__ void speed_cov_hip_kernel(
    const float *__restrict__ plane,
    const float *__restrict__ means,
    float *__restrict__ cov_mat,
    uint32_t stride_px,
    uint32_t num_blocks_h,
    uint32_t num_blocks,
    uint32_t submatrix_w,
    uint32_t submatrix_h)
{
    /* Each work-group handles one (x_index, y_index) pair. */
    const uint32_t x_index = blockIdx.x;
    const uint32_t y_index = blockIdx.y;
    const uint32_t tid = threadIdx.x;
    const uint32_t block_sz = blockDim.x;
    (void)num_blocks_h;
    (void)num_blocks;

    /* Shared memory for per-thread partial sums. */
    extern __shared__ double s_partial[];
    s_partial[tid] = 0.0;
    __syncthreads();

    const uint32_t xr = x_index / SP_BLOCK_SIZE;
    const uint32_t xc = x_index % SP_BLOCK_SIZE;
    const uint32_t yr = y_index / SP_BLOCK_SIZE;
    const uint32_t yc = y_index % SP_BLOCK_SIZE;
    const double mean_x = (double)means[x_index];
    const double mean_y = (double)means[y_index];

    /* CPU parity (compute_covariance): one GLOBAL submatrix sweep with the
     * scalar global means, divided by N once. The historic per-tile loop
     * (displaced origins ty*5+xr, per-tile means, per-tile /N) summed
     * num_blocks block-local covariances instead — wrong matrix, ~7x-low
     * scores. Threads stride over the submatrix_h × submatrix_w pixels. */
    const uint32_t total = submatrix_h * submatrix_w;
    double local_sum = 0.0;
    for (uint32_t p = tid; p < total; p += block_sz) {
        const uint32_t i = p / submatrix_w;
        const uint32_t j = p % submatrix_w;
        const double vx = (double)plane[(xr + i) * stride_px + (xc + j)];
        const double vy = (double)plane[(yr + i) * stride_px + (yc + j)];
        local_sum += (vx - mean_x) * (vy - mean_y);
    }
    s_partial[tid] = local_sum;
    __syncthreads();

    /* Tree reduction. */
    for (uint32_t s = block_sz / 2u; s > 0u; s >>= 1u) {
        if (tid < s)
            s_partial[tid] += s_partial[tid + s];
        __syncthreads();
    }
    if (tid == 0u)
        cov_mat[x_index * SP_ELEMENTS + y_index] = (float)(s_partial[0] / (double)total);
}

/* ------------------------------------------------------------------ */
/* Kernel 3: independent term (one pixel per element per tile)        */
/* ------------------------------------------------------------------ */

extern "C" __global__ void speed_indterm_hip_kernel(
    const float *__restrict__ plane,
    float *__restrict__ indterm,
    uint32_t stride_px,
    uint32_t num_blocks_h,
    uint32_t num_blocks)
{
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t total = SP_ELEMENTS * num_blocks;
    if (idx >= total)
        return;

    const uint32_t elem = idx / num_blocks;
    const uint32_t tile_idx = idx % num_blocks;
    const uint32_t tile_x = tile_idx % num_blocks_h;
    const uint32_t tile_y = tile_idx / num_blocks_h;
    const uint32_t er = elem / SP_BLOCK_SIZE;
    const uint32_t ec = elem % SP_BLOCK_SIZE;
    const uint32_t pr = tile_y * SP_BLOCK_SIZE + er;
    const uint32_t pc = tile_x * SP_BLOCK_SIZE + ec;
    indterm[elem * num_blocks + tile_idx] = plane[pr * stride_px + pc];
}

/* ------------------------------------------------------------------ */
/* Kernel 4: backward substitution (one wavefront per column)         */
/* ------------------------------------------------------------------ */
/*
 * HIP adaptation: wavefront size is 64 on GCN/RDNA1 or 32 on RDNA2+.
 * Threads >= SP_ELEMENTS are idle per wavefront (25 active out of 32/64).
 * Serial dependency between rows is enforced via
 * __builtin_amdgcn_wave_barrier() — the AMD equivalent of __syncwarp().
 * Layout: num_blocks columns distributed across wavefronts.
 * blockDim.x = device wavefront size (host queries and passes at launch),
 * gridDim.x = num_blocks.
 */
extern "C" __global__ void speed_solve_hip_kernel(
    const float *__restrict__ R,
    float *__restrict__ rhs,
    uint32_t num_blocks)
{
    /* Each wavefront processes one column.  The grid is launched as
     * (num_blocks × 1 × 1) blocks of warpSize threads. */
    const uint32_t col = blockIdx.x;
    const uint32_t lane = threadIdx.x;

    if (col >= num_blocks || lane >= SP_ELEMENTS)
        return;

    /* Back-substitute row by row from row 24 down to 0. */
    for (int32_t i = (int32_t)(SP_ELEMENTS - 1u); i >= 0; --i) {
        if ((int32_t)lane == i) {
            float val = rhs[(uint32_t)i * num_blocks + col];
            const float denom = R[(uint32_t)i * SP_ELEMENTS + (uint32_t)i];
            for (uint32_t k = (uint32_t)(i + 1); k < SP_ELEMENTS; ++k)
                val -= rhs[k * num_blocks + col] * R[(uint32_t)i * SP_ELEMENTS + k];
            rhs[(uint32_t)i * num_blocks + col] =
                (fabsf(denom) > 1e-8f) ? val / denom : 0.0f;
        }
        /* All 64 lanes must wait for row-i result before proceeding. */
        __builtin_amdgcn_wave_barrier();
    }
}

/* ------------------------------------------------------------------ */
/* Kernel 5: per-tile entropy + score                                  */
/* ------------------------------------------------------------------ */

extern "C" __global__ void speed_score_hip_kernel(
    const float *__restrict__ ref_eigenvalues,
    const float *__restrict__ dis_eigenvalues,
    const float *__restrict__ ref_sol,
    const float *__restrict__ dis_sol,
    const float *__restrict__ ref_indterm,
    const float *__restrict__ dis_indterm,
    float *__restrict__ ref_entropies,
    float *__restrict__ ref_variances,
    float *__restrict__ dis_entropies,
    float *__restrict__ dis_variances,
    uint32_t num_blocks,
    float sigma_nn)
{
    const uint32_t tile = blockIdx.x * blockDim.x + threadIdx.x;
    if (tile >= num_blocks)
        return;

    /* Accumulate variance proxies (dot product of solution and indterm). */
    float rv = 0.0f;
    float dv = 0.0f;
    for (uint32_t elem = 0; elem < SP_ELEMENTS; ++elem) {
        const uint32_t idx = elem * num_blocks + tile;
        rv += ref_sol[idx] * ref_indterm[idx];
        dv += dis_sol[idx] * dis_indterm[idx];
    }
    rv /= (float)SP_ELEMENTS;
    dv /= (float)SP_ELEMENTS;
    ref_variances[tile] = rv;
    dis_variances[tile] = dv;

    /* Differential entropy (nats → bits via log2, Gaussian model). The CPU
     * reference (est_params in speed.c) eigendecomposes SEPARATE ref and dis
     * covariance matrices, so the ref entropy uses ref_eigenvalues and the dis
     * entropy uses dis_eigenvalues — they are NOT shared. */
    const float log2e_2pi = log2f(2.0f * 3.14159265358979323846f * 2.71828182845904523536f);
    float re = 0.0f;
    float de = 0.0f;
    for (uint32_t k = 0; k < SP_ELEMENTS; ++k) {
        float ref_lk = ref_eigenvalues[k] < 0.0f ? 0.0f : ref_eigenvalues[k];
        float dis_lk = dis_eigenvalues[k] < 0.0f ? 0.0f : dis_eigenvalues[k];
        re += log2f(ref_lk * rv + sigma_nn) + log2e_2pi;
        de += log2f(dis_lk * dv + sigma_nn) + log2e_2pi;
    }
    ref_entropies[tile] = re;
    dis_entropies[tile] = de;
}
