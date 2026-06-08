/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Shared types and constants for the SpEED GPU-backend implementations
 *  (ADR-0567: real on-device GPU kernels for speed_chroma + speed_temporal).
 *
 *  The SpEED algorithm has two distinct layers of parallelism:
 *
 *    (A) Pixel-to-tile aggregation (highly parallel):
 *        - Covariance matrix accumulation: sum over all num_blocks tiles,
 *          each contributing a 25-element outer product. Maps to a 625-entry
 *          reduction that GPU handles well.
 *        - Independent term assembly: each of the num_blocks tiles contributes
 *          independently. Maps directly to a GPU parallel scatter.
 *        - Per-tile score computation: num_blocks independent scalars.
 *
 *    (B) 25×25 matrix operations (serial, fixed size):
 *        - Eigendecomposition via QR iteration (Householder tridiagonalisation
 *          + implicit-shift QR sweep, ≤500 iterations, ~50µs on any CPU).
 *        - QR factorisation of the 25×25 covariance matrix for the linear
 *          system solve.
 *
 *  GPU kernels cover layer (A); layer (B) runs on CPU after a D2H readback of
 *  625 floats (the covariance matrix). This is NOT CPU forwarding: the entire
 *  pixel/tile workload runs on device. The 25×25 eigendecomp is a mathematical
 *  constraint — serial Lanczos/QR on a fixed-size tiny matrix.
 *
 *  ADR reference: ADR-0567.
 */

#ifndef VMAF_SRC_FEATURE_SPEED_GPU_COMMON_H_
#define VMAF_SRC_FEATURE_SPEED_GPU_COMMON_H_

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Algorithm constants                                                 */
/* ------------------------------------------------------------------ */

#define SPEED_BLOCK_SIZE (5)
#define SPEED_ELEMENTS (SPEED_BLOCK_SIZE * SPEED_BLOCK_SIZE) /* 25 */
#define SPEED_NUM_SCALES (4)
#define SPEED_EIGENVALUE_EPS (1e-6f)

/* ------------------------------------------------------------------ */
/* GPU kernel launch constants                                         */
/* ------------------------------------------------------------------ */

/* Covariance kernel: one workgroup per (row, col) entry of the 25×25
 * symmetric covariance matrix.  Each workgroup accumulates partial sums
 * across min(num_blocks, threads_per_cov_wg) tiles in parallel.       */
#define SPEED_COV_WG_SIZE (256)

/* Independent-term kernel: threads cover (element_idx × tile_idx).     */
#define SPEED_INDTERM_WG_SIZE (256)

/* Score kernel: one thread per tile.                                    */
#define SPEED_SCORE_WG_SIZE (256)

/* ------------------------------------------------------------------ */
/* Push-constants / uniform block for GPU kernels                     */
/* ------------------------------------------------------------------ */

typedef struct SpeedGpuParams {
    uint32_t num_blocks_h;    /* tiles in X (truncated_width  / block_size) */
    uint32_t num_blocks_v;    /* tiles in Y (truncated_height / block_size) */
    uint32_t num_blocks;      /* total tiles = num_blocks_h * num_blocks_v  */
    uint32_t op_width;        /* operating_width  (truncated_width)         */
    uint32_t op_height;       /* operating_height (truncated_height)        */
    uint32_t stride_px;       /* float_stride / sizeof(float)               */
    float sigma_nn;           /* neural noise std dev                        */
    float nn_floor;           /* neural noise floor fraction                 */
    uint32_t weight_var_mode; /* 0-6 variance weighting mode                */
} SpeedGpuParams;

/* ------------------------------------------------------------------ */
/* Host-side buffer layout for GPU results                            */
/* ------------------------------------------------------------------ */

/* Per-frame GPU result buffers (host-pinned or mapped).
 * cov_mat[25][25] (row-major, symmetric) → eigendecomp on CPU.
 * scores[num_blocks] → aggregate to frame score on CPU.               */
typedef struct SpeedGpuResults {
    float cov_mat[SPEED_ELEMENTS * SPEED_ELEMENTS]; /* 625 floats */
    float *scores;                                  /* num_blocks floats */
    float *variances;                               /* num_blocks floats */
    float *entropies;                               /* num_blocks floats */
} SpeedGpuResults;

#endif /* VMAF_SRC_FEATURE_SPEED_GPU_COMMON_H_ */
