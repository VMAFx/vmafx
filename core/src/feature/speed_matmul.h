/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef FEATURE_SPEED_MATMUL_H_
#define FEATURE_SPEED_MATMUL_H_

/*
 * Dense single-precision matrix product used by SpEED's QR / linear-solve
 * path (`matrix_qr_decomposition`, `solve_linear_system` in speed.c).
 *
 * Contract — every implementation MUST produce bit-identical output:
 *
 *   dst[i][j] = ((((0 + x[i][0]*y[0][j]) + x[i][1]*y[1][j]) + ...)
 *                                        + x[i][inner-1]*y[inner-1][j])
 *
 * i.e. the accumulation order over `k` is fixed left-to-right and each
 * step is a separate IEEE-754 binary32 multiply followed by a separate
 * add.  Fusing the pair into an FMA changes the rounding and breaks the
 * contract, so the SIMD translation units are compiled in their own
 * `-ffp-contract=off` static libraries (see core/src/meson.build, same
 * carve-out pattern as x86_ssim_avx2 / x86_float_adm_avx2).  Vector
 * widening is safe because `j` is an output index, not a reduction axis:
 * widening changes how many lanes are processed per instruction, never
 * the order in which a single dst element accumulates.
 *
 * `dst` must not overlap `x` or `y` (the SIMD kernels keep the dst row in
 * registers across the whole `k` loop).  All SpEED call sites use
 * disjoint workspace buffers.
 *
 * Strides are element counts (floats), not bytes.
 */
typedef void (*speed_matmul_fn)(float *dst, int dst_stride, const float *x, int x_stride,
                                const float *y, int y_stride, int rows, int inner, int cols);

/* Portable reference implementation (speed.c). */
void speed_matmul_scalar(float *dst, int dst_stride, const float *x, int x_stride, const float *y,
                         int y_stride, int rows, int inner, int cols);

#endif /* FEATURE_SPEED_MATMUL_H_ */
