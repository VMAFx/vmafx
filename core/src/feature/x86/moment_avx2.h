/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  AVX2 dispatch for `float_moment`.
 *  See ADR-0179 — closes the only remaining fully-scalar SIMD-matrix
 *  row identified in `.workingdir2/analysis/metrics-backends-matrix.md`.
 */

#ifndef LIBVMAF_FEATURE_X86_MOMENT_AVX2_H_
#define LIBVMAF_FEATURE_X86_MOMENT_AVX2_H_

int compute_1st_moment_avx2(const float *pic, int w, int h, int stride, double *score);
int compute_2nd_moment_avx2(const float *pic, int w, int h, int stride, double *score);

#endif /* LIBVMAF_FEATURE_X86_MOMENT_AVX2_H_ */
