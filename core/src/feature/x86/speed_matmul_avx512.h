/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef X86_SPEED_MATMUL_AVX512_H_
#define X86_SPEED_MATMUL_AVX512_H_

/* Bit-exact AVX-512 twin of speed_matmul_scalar(); see feature/speed_matmul.h
 * for the accumulation-order contract this kernel is required to honour. */
void speed_matmul_avx512(float *dst, int dst_stride, const float *x, int x_stride, const float *y,
                         int y_stride, int rows, int inner, int cols);

#endif /* X86_SPEED_MATMUL_AVX512_H_ */
