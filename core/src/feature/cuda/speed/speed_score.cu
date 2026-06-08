/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernels for the SpEED feature extractor (ADR-0567).
 *
 *  These kernels implement the tile-parallel layer of the SpEED algorithm.
 *  The CPU reference lives in libvmaf/src/feature/speed.c.
 *
 *  Kernel map (matches CPU est_params steps):
 *
 *  1. speed_means_kernel  — compute per-position mean across the tile.
 *     One thread per (element_row, element_col, tile_idx) triple.
 *     Block size: SPEED_ELEMENTS×SPEED_BLOCK_TILES, stride over tiles.
 *
 *  2. speed_cov_kernel — accumulate one entry of the 25×25 covariance
 *     matrix.  One CUDA block per (row, col) pair (625 blocks), each block
 *     sums the contribution from all num_blocks tiles using a shared-memory
 *     warp reduction. Output: cov_mat[625] (symmetric, row-major).
 *
 *  3. speed_indterm_kernel — assemble the 25×num_blocks independent-term
 *     matrix.  One thread per (element, tile) pair; scatter access.
 *
 *  4. speed_solve_kernel — backward substitution for each tile column.
 *     Given the QR factorisation R[25×25] from the host CPU (25×25 is too
 *     small to benefit from GPU QR), solve RX[:,j] = QtB[:,j] for all
 *     j in [0, num_blocks) in parallel.  One warp per column (25 < 32).
 *
 *  5. speed_score_kernel — per-tile entropy update and score computation.
 *     One thread per tile.  Reads eigenvalues (25 floats), the accumulated
 *     linear_system_sol row 0, and the independent_term to compute variances
 *     and entropies for ref and dis, then writes the per-tile score.
 *
 *  Numerical contract (ADR-0567):
 *    - Covariance matrix: double-precision accumulation in shared memory
 *      then cast to float to match CPU reference (which uses double in
 *      compute_covariance).
 *    - Backward substitution: float arithmetic, matches CPU.
 *    - Score: float arithmetic, matches CPU.
 *  places=4 vs CPU reference (ADR-0214 per-backend gate).
 */

#include "cuda_helper.cuh"
#include "common.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define SP_BLOCK_SIZE (5)
#define SP_ELEMENTS (SP_BLOCK_SIZE * SP_BLOCK_SIZE) /* 25 */

/* ------------------------------------------------------------------ */
/* Kernel 1: means                                                     */
/* ------------------------------------------------------------------ */

/* Compute the mean for each of the 25 pixel positions within the 5×5
 * block template, averaging over the num_blocks spatial tiles.
 *
 * CPU reference: compute_mean / outer loop in compute_covariance_matrix.
 *
 * Layout: gridDim.x = ceil(num_blocks / MEANS_TX), blockDim.x = MEANS_TX.
 *   Each thread handles one tile.  We loop over 25 elements inside the
 *   thread block using shared memory so every element sees the same tile
 *   pixel data once.
 *
 * Output:  means[elem * num_blocks + tile_idx]  (float)
 */
#define MEANS_TX (256)

extern "C" {

__global__ void speed_means_kernel(const float *__restrict__ plane, /* downscaled plane */
                                   float *__restrict__ means,       /* [25 × num_blocks] */
                                   uint32_t op_width,               /* truncated_width    */
                                   uint32_t stride_px,              /* float_stride/4     */
                                   uint32_t num_blocks_h,           /* tiles in X         */
                                   uint32_t num_blocks,             /* total tiles         */
                                   uint32_t submatrix_w,            /* submatrix_width     */
                                   uint32_t submatrix_h)            /* submatrix_height    */
{
    const uint32_t tile_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (tile_idx >= num_blocks)
        return;

    const uint32_t tile_x = tile_idx % num_blocks_h; /* tile column */
    const uint32_t tile_y = tile_idx / num_blocks_h; /* tile row    */

    /* For each of the 25 positions (r, c) within the 5×5 block: */
    for (uint32_t elem = 0; elem < SP_ELEMENTS; ++elem) {
        const uint32_t er = elem / SP_BLOCK_SIZE;
        const uint32_t ec = elem % SP_BLOCK_SIZE;
        /* starting pixel of position (er, ec) within this tile */
        const uint32_t start_row = tile_y * SP_BLOCK_SIZE + er;
        const uint32_t start_col = tile_x * SP_BLOCK_SIZE + ec;

        /* Accumulate mean over the submatrix window. */
        double acc = 0.0;
        for (uint32_t i = 0; i < submatrix_h; ++i) {
            for (uint32_t j = 0; j < submatrix_w; ++j) {
                acc += (double)plane[(start_row + i) * stride_px + (start_col + j)];
            }
        }
        means[elem * num_blocks + tile_idx] = (float)(acc / (double)(submatrix_w * submatrix_h));
    }
}

/* ------------------------------------------------------------------ */
/* Kernel 2: covariance matrix accumulation                            */
/* ------------------------------------------------------------------ */

/* Compute one entry cov_mat[x_index][y_index] of the 25×25 symmetric
 * covariance matrix by summing over all tiles.
 *
 * Launch: gridDim.x = SP_ELEMENTS (25), gridDim.y = SP_ELEMENTS (25),
 *         blockDim.x = COV_BLOCK (up to 256 threads per block).
 * Only the lower triangle (y_index <= x_index) needs unique computation;
 * the upper triangle is filled by symmetry on the host after readback.
 *
 * CPU reference: compute_covariance() called in a double loop.
 */
#define COV_BLOCK (256)

__global__ void speed_cov_kernel(const float *__restrict__ plane,
                                 const float *__restrict__ means, /* [25 × num_blocks] */
                                 float *__restrict__ cov_mat,     /* [25 × 25], row-major */
                                 uint32_t stride_px, uint32_t num_blocks_h, uint32_t num_blocks,
                                 uint32_t submatrix_w, uint32_t submatrix_h)
{
    /* Each block handles one (x_index, y_index) pair. */
    const uint32_t x_index = blockIdx.x; /* row    in [0, 25) */
    const uint32_t y_index = blockIdx.y; /* column in [0, 25) */

    if (x_index >= SP_ELEMENTS || y_index >= SP_ELEMENTS)
        return;

    const uint32_t tid = threadIdx.x;

    __shared__ double s_partial[COV_BLOCK];
    s_partial[tid] = 0.0;
    __syncthreads();

    /* Pixel coordinates within the 5×5 block for x_index and y_index */
    const uint32_t xr = x_index / SP_BLOCK_SIZE;
    const uint32_t xc = x_index % SP_BLOCK_SIZE;
    const uint32_t yr = y_index / SP_BLOCK_SIZE;
    const uint32_t yc = y_index % SP_BLOCK_SIZE;

    /* Each thread covers a strided subset of tiles. */
    double local_sum = 0.0;
    for (uint32_t tile_idx = tid; tile_idx < num_blocks; tile_idx += COV_BLOCK) {
        const uint32_t tile_x = tile_idx % num_blocks_h;
        const uint32_t tile_y = tile_idx / num_blocks_h;

        const double mean_x = (double)means[x_index * num_blocks + tile_idx];
        const double mean_y = (double)means[y_index * num_blocks + tile_idx];

        const uint32_t start_row_x = tile_y * SP_BLOCK_SIZE + xr;
        const uint32_t start_col_x = tile_x * SP_BLOCK_SIZE + xc;
        const uint32_t start_row_y = tile_y * SP_BLOCK_SIZE + yr;
        const uint32_t start_col_y = tile_x * SP_BLOCK_SIZE + yc;

        double cov = 0.0;
        for (uint32_t i = 0; i < submatrix_h; ++i) {
            for (uint32_t j = 0; j < submatrix_w; ++j) {
                double vx = (double)plane[(start_row_x + i) * stride_px + (start_col_x + j)];
                double vy = (double)plane[(start_row_y + i) * stride_px + (start_col_y + j)];
                cov += (vx - mean_x) * (vy - mean_y);
            }
        }
        local_sum += cov / (double)(submatrix_w * submatrix_h);
    }
    s_partial[tid] = local_sum;
    __syncthreads();

    /* Tree reduction */
    for (uint32_t s = COV_BLOCK / 2; s > 0; s >>= 1) {
        if (tid < s)
            s_partial[tid] += s_partial[tid + s];
        __syncthreads();
    }

    if (tid == 0)
        cov_mat[x_index * SP_ELEMENTS + y_index] = (float)s_partial[0];
}

/* ------------------------------------------------------------------ */
/* Kernel 3: independent term assembly                                 */
/* ------------------------------------------------------------------ */

/* Assemble the 25×num_blocks independent_term matrix.
 *
 * CPU reference: compute_independent_term_for_offset / compute_independent_term.
 *
 * Layout: gridDim.x = ceil(num_blocks * SP_ELEMENTS / INDTERM_BLOCK)
 *         blockDim.x = INDTERM_BLOCK.
 *
 * For each (element, tile) pair, write
 *   indterm[element * num_blocks + tile] = plane[tile_row_start * stride + tile_col_start]
 * where tile_row_start = (tile_y * block_size) + element_row,
 *       tile_col_start = (tile_x * block_size) + element_col.
 * This corresponds to exactly one pixel per (element, tile) pair, which is
 * the top-left corner of the submatrix for that position.
 */
#define INDTERM_BLOCK (256)

__global__ void speed_indterm_kernel(const float *__restrict__ plane,
                                     float *__restrict__ indterm, /* [25 × num_blocks] */
                                     uint32_t stride_px, uint32_t num_blocks_h, uint32_t num_blocks)
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

    const uint32_t pixel_row = tile_y * SP_BLOCK_SIZE + er;
    const uint32_t pixel_col = tile_x * SP_BLOCK_SIZE + ec;

    indterm[elem * num_blocks + tile_idx] = plane[pixel_row * stride_px + pixel_col];
}

/* ------------------------------------------------------------------ */
/* Kernel 4: backward substitution (solve R X = Q^T B columns)        */
/* ------------------------------------------------------------------ */

/* Solve RX = RHS for each of num_blocks columns in parallel.
 * R is upper-triangular 25×25 (from QR factorisation on CPU).
 * RHS is the result of Q^T B computed on CPU (25×num_blocks).
 * Each warp (≤32 threads, 25 used) handles one column via sequential
 * backward substitution (i decreasing from 24..0), with all warps
 * running concurrently across the grid.
 *
 * CPU reference: solve_triangular_system in solve_linear_system.
 *
 * Output: X[25 × num_blocks] in-place into rhs.
 */
#define SOLVE_WARP (32)

__global__ void speed_solve_kernel(const float *__restrict__ R, /* [25×25] upper tri */
                                   float *__restrict__ rhs,     /* [25×num_blocks], in/out */
                                   uint32_t num_blocks)
{
    /* Each warp handles one column. */
    const uint32_t warp_id = (blockIdx.x * blockDim.x + threadIdx.x) / SOLVE_WARP;
    const uint32_t lane = threadIdx.x % SOLVE_WARP;
    if (warp_id >= num_blocks || lane >= SP_ELEMENTS)
        return;

    const uint32_t col = warp_id; /* column index into rhs */

    /* Backward substitution: row i from 24 downto 0.
     * Only lane 0..24 participate; lane 25..31 idle. */
    for (int32_t i = (int32_t)(SP_ELEMENTS - 1); i >= 0; --i) {
        /* Only the thread responsible for row i performs the update. */
        if ((int32_t)lane == i) {
            float val = rhs[(uint32_t)i * num_blocks + col];
            float denom = R[(uint32_t)i * SP_ELEMENTS + (uint32_t)i];
            /* Subtract contributions from already-solved rows i+1..24. */
            for (uint32_t k = (uint32_t)(i + 1); k < SP_ELEMENTS; ++k)
                val -= rhs[k * num_blocks + col] * R[(uint32_t)i * SP_ELEMENTS + k];
            /* Store — all other lanes read this via __shfl in future iters,
             * but since we do serial rows here the simpler store is correct. */
            if (fabsf(denom) > 1e-8f)
                rhs[(uint32_t)i * num_blocks + col] = val / denom;
            else
                rhs[(uint32_t)i * num_blocks + col] = 0.0f;
        }
        /* Barrier within the warp to ensure row-i result is visible to
         * lower rows before they continue. Since only one lane writes,
         * __syncwarp is sufficient. */
        __syncwarp(0xFFFFFFFFu);
    }
}

/* ------------------------------------------------------------------ */
/* Kernel 5: per-tile score computation                                */
/* ------------------------------------------------------------------ */

/* Compute per-tile entropy and variance, then the absolute score diff.
 *
 * CPU reference: update_entropy, get_speed_score.
 *
 * Inputs:
 *   eigenvalues[25] — from CPU eigendecomp of the covariance matrix.
 *   ref_sol[25 × num_blocks] — linear system solution for reference.
 *   dis_sol[25 × num_blocks] — linear system solution for distorted.
 *   ref_indterm[25 × num_blocks] — independent terms for reference.
 *   dis_indterm[25 × num_blocks] — independent terms for distorted.
 *
 * Output:
 *   ref_entropies[num_blocks], dis_entropies[num_blocks]
 *   ref_variances[num_blocks], dis_variances[num_blocks]
 *   (host reduces these to a single score)
 */
#define SCORE_BLOCK (256)
#define SPEED_PI_F (3.14159265358979323846f)
#define SPEED_E_F (2.71828182845904523536f)

__global__ void speed_score_kernel(const float *__restrict__ eigenvalues, /* [25] */
                                   const float *__restrict__ ref_sol,     /* [25 × num_blocks] */
                                   const float *__restrict__ dis_sol,     /* [25 × num_blocks] */
                                   const float *__restrict__ ref_indterm, /* [25 × num_blocks] */
                                   const float *__restrict__ dis_indterm, /* [25 × num_blocks] */
                                   float *__restrict__ ref_entropies,     /* [num_blocks] */
                                   float *__restrict__ ref_variances,     /* [num_blocks] */
                                   float *__restrict__ dis_entropies,     /* [num_blocks] */
                                   float *__restrict__ dis_variances,     /* [num_blocks] */
                                   uint32_t num_blocks, float sigma_nn)
{
    const uint32_t tile = blockIdx.x * blockDim.x + threadIdx.x;
    if (tile >= num_blocks)
        return;

    const float log2e_2pi = log2f(2.0f * SPEED_PI_F * SPEED_E_F);

    /* Step 5+6 (CPU: compute_pointwise_product_and_division + sum_columns):
     * variance[tile] = sum over elem { sol[elem][tile] * indterm[elem][tile] } / 25 */
    float ref_var = 0.0f;
    float dis_var = 0.0f;
    for (uint32_t elem = 0; elem < SP_ELEMENTS; ++elem) {
        const uint32_t idx = elem * num_blocks + tile;
        ref_var += ref_sol[idx] * ref_indterm[idx];
        dis_var += dis_sol[idx] * dis_indterm[idx];
    }
    ref_var /= (float)SP_ELEMENTS;
    dis_var /= (float)SP_ELEMENTS;
    ref_variances[tile] = ref_var;
    dis_variances[tile] = dis_var;

    /* Step 9 (CPU: update_entropy for each eigenvalue k):
     * entropy = sum_k { log2(L_k * var + sigma_nn) + log2(2πe) }
     * where L_k = max(0, eigenvalues[k]). */
    float ref_ent = 0.0f;
    float dis_ent = 0.0f;
    for (uint32_t k = 0; k < SP_ELEMENTS; ++k) {
        float lk = eigenvalues[k] < 0.0f ? 0.0f : eigenvalues[k];
        ref_ent += log2f(lk * ref_var + sigma_nn) + log2e_2pi;
        dis_ent += log2f(lk * dis_var + sigma_nn) + log2e_2pi;
    }
    ref_entropies[tile] = ref_ent;
    dis_entropies[tile] = dis_ent;
}

} /* extern "C" */
