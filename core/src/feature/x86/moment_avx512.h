/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  AVX-512 dispatch header for compute_1st_moment / compute_2nd_moment
 *  (float_moment feature extractor).  Widens the AVX2 8-lane path to
 *  16-lane ZMM; see ADR-0987.
 */

#ifndef LIBVMAF_FEATURE_X86_MOMENT_AVX512_H_
#define LIBVMAF_FEATURE_X86_MOMENT_AVX512_H_

int compute_1st_moment_avx512(const float *pic, int w, int h, int stride, double *score);
int compute_2nd_moment_avx512(const float *pic, int w, int h, int stride, double *score);

#endif /* LIBVMAF_FEATURE_X86_MOMENT_AVX512_H_ */
