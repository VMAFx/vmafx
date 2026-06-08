/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Internal API for the SpEED feature engine (speed.c), consumed by
 *  GPU-backend twins (ADR-0567).
 *
 *  Only symbols that GPU backends need are exposed here.  The full SpEED
 *  CPU pipeline (matrix math, eigensolver, QR decomp, score computation)
 *  also lives here as extern declarations; GPU backends call the CPU
 *  helpers for the fixed-size 25×25 matrix operations and provide their
 *  own GPU kernels for the tile-parallel pixel work.
 *
 *  GPU backend split:
 *    (CPU path) speed_filter_and_downscale   — Gaussian anti-alias filter
 *               speed_compute_eigenvalues    — 25×25 QR-iteration
 *               speed_qr_factorize           — 25×25 QR decomposition
 *               speed_qtb_multiply           — Q^T B multiply
 *    (GPU path) means, covariance, indterm,  — all tile-parallel work
 *               backward_subst, score
 */

#ifndef VMAF_SRC_FEATURE_SPEED_INTERNAL_H_
#define VMAF_SRC_FEATURE_SPEED_INTERNAL_H_

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Re-exported types (copies of speed.c private types)                 */
/* ------------------------------------------------------------------ */

#define SPEED_INTERNAL_BLOCK_SIZE (5)
#define SPEED_INTERNAL_ELEMENTS (SPEED_INTERNAL_BLOCK_SIZE * SPEED_INTERNAL_BLOCK_SIZE)
#define SPEED_INTERNAL_NUM_SCALES (4)
#define SPEED_INTERNAL_EIGENVALUE_MAX_ITERS (500)

typedef struct SpeedInternalDimensions {
    size_t original_height;
    size_t original_width;
    size_t scaled_height;
    size_t scaled_width;
    size_t alloc_height;
    size_t alloc_width;
    size_t operating_height;
    size_t operating_width;
    size_t block_size;
    size_t truncated_width;
    size_t truncated_height;
    size_t num_blocks_horizontal;
    size_t num_blocks_vertical;
    size_t num_blocks;
    size_t elements_in_block;
    size_t submatrix_width;
    size_t submatrix_height;
} SpeedInternalDimensions;

typedef struct SpeedInternalOptions {
    double speed_kernelscale;
    double speed_prescale;
    char *speed_prescale_method;
    double speed_sigma_nn;
    double speed_nn_floor;
    int speed_weight_var_mode;
} SpeedInternalOptions;

/* ------------------------------------------------------------------ */
/* CPU-side helpers exposed to GPU backends                            */
/* ------------------------------------------------------------------ */

/**
 * Compute SpEED dimensions from luma dimensions and prescale factor.
 * Fills all fields of *dim.
 *
 * @param dim     Output dimensions descriptor.
 * @param w       Frame width (luma or chroma depending on extractor).
 * @param h       Frame height.
 * @param prescale Prescale factor from SpeedInternalOptions.
 * @return 0 on success, -EINVAL if frame is too small.
 */
int speed_internal_init_dimensions(SpeedInternalDimensions *dim, int w, int h, double prescale);

/**
 * Compute the float_stride (bytes, ALIGN_CEIL-aligned) for alloc_width.
 *
 * @param alloc_width  alloc_width from SpeedInternalDimensions.
 * @return float_stride in bytes.
 */
size_t speed_internal_float_stride(size_t alloc_width);

/**
 * Apply anti-alias Gaussian filter and 2^NUM_SCALES downsampling, then
 * local mean subtraction, to the in-place float plane.  After this call,
 * frame_buffer[0 .. truncated_height * float_stride) holds the operating-
 * resolution mean-subtracted image.
 *
 * The tmp_buffer must be at least
 *   2 * alloc_height * (float_stride / sizeof(float)) * sizeof(float)
 * bytes.
 *
 * CPU reference: static filter_and_downscale() in speed.c.
 *
 * @param dim          SpeedInternalDimensions (must be pre-filled).
 * @param opt          SpeedInternalOptions (prescale_method, kernelscale).
 * @param frame_buffer Float plane (in/out, size = alloc_height * stride_px).
 * @param tmp_buffer   Scratch buffer (see size requirement above).
 * @param float_stride Float stride in bytes.
 */
void speed_internal_filter_and_downscale(SpeedInternalDimensions dim, SpeedInternalOptions *opt,
                                         float *frame_buffer, float *tmp_buffer,
                                         size_t float_stride);

/**
 * Compute the 25×25 covariance matrix from the operating-resolution
 * mean-subtracted float plane on the CPU.
 *
 * Used by GPU backends as a fallback / validation path; production GPU
 * backends launch GPU kernels instead.
 *
 * Output: cov_mat[elements_in_block × elements_in_block], symmetric.
 * tmp_means must be ≥ elements_in_block floats.
 *
 * @param dim          Dimensions.
 * @param data         Mean-subtracted operating-resolution plane.
 * @param cov_mat      Output covariance matrix [25×25].
 * @param tmp_means    Scratch buffer [25] for means.
 * @param stride_px    float_stride / sizeof(float).
 */
void speed_internal_compute_cov_matrix(SpeedInternalDimensions dim, const float *data,
                                       float *cov_mat, float *tmp_means, size_t stride_px);

/**
 * Compute eigenvalues of a symmetric matrix via Householder
 * tridiagonalisation followed by implicit-shift QR iteration.
 *
 * CPU reference: compute_eigenvalues() in speed.c.
 *
 * @param A_immutable  Input symmetric matrix [size × size] (read-only).
 * @param eigenvalues  Output eigenvalues [size].
 * @param size         Matrix side length (25 for SpEED).
 * @param buffer       Scratch: (size²+3×size) floats.
 */
void speed_internal_compute_eigenvalues(const float *A_immutable, float *eigenvalues, int size,
                                        float *buffer);

/**
 * QR factorisation of a square matrix A (in-place, Householder reflections).
 * Produces Q (orthogonal) and R (upper triangular) such that A = QR.
 * All matrices are [size × size] in row-major order.
 *
 * CPU reference: matrix_qr_decomposition() + solve_linear_system() in speed.c.
 *
 * @param A_data     Input matrix [size×size] (overwritten as scratch).
 * @param size       Matrix side length.
 * @param Q_out      Output Q [size×size].
 * @param R_out      Output R [size×size] (upper triangular).
 * @param tmp        Scratch: 3×size² floats.
 * @return 0 on success.
 */
int speed_internal_qr_factorize(float *A_data, int size, float *Q_out, float *R_out, float *tmp);

/**
 * Multiply Q^T (transposed orthogonal factor from QR) by B:
 *   Result = Q^T × B
 * where Q is [size×size] and B is [size×num_cols].
 * Result is written into B in-place (overwriting input).
 *
 * @param Q          [size×size] orthogonal matrix.
 * @param B          [size×num_cols] in/out (overwritten).
 * @param size       Matrix side length.
 * @param num_cols   Number of RHS columns.
 * @param tmp        Scratch: size×num_cols floats.
 */
void speed_internal_qt_multiply(const float *Q, float *B, int size, int num_cols, float *tmp);

/**
 * Backward substitution: solve R × X = B for all columns simultaneously.
 * B is overwritten with X.  R must be upper triangular with non-zero diagonal.
 *
 * CPU reference: solve_triangular_system() in speed.c.
 *
 * @param R        [size×size] upper triangular.
 * @param B        [size×num_cols] RHS on input, solution X on output.
 * @param size     Matrix side length.
 * @param num_cols Number of RHS columns.
 * @return 0 on success, -EINVAL if R diagonal has a near-zero entry.
 */
int speed_internal_backward_substitution(const float *R, float *B, int size, int num_cols);

/**
 * Check whether all eigenvalues exceed EIGENVALUE_EPS.
 * Returns true if the covariance matrix is regular (can be inverted).
 *
 * @param eigenvalues  [num_elements] float array.
 * @param num_elements Number of eigenvalues.
 * @return true if all eigenvalues > EIGENVALUE_EPS.
 */
bool speed_internal_is_matrix_regular(const float *eigenvalues, size_t num_elements);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_FEATURE_SPEED_INTERNAL_H_ */
