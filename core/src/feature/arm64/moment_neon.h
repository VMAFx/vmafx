/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  NEON dispatch for `float_moment`.  See ADR-0179 / T7-19.
 */

#ifndef LIBVMAF_FEATURE_ARM64_MOMENT_NEON_H_
#define LIBVMAF_FEATURE_ARM64_MOMENT_NEON_H_

int compute_1st_moment_neon(const float *pic, int w, int h, int stride, double *score);
int compute_2nd_moment_neon(const float *pic, int w, int h, int stride, double *score);

#endif /* LIBVMAF_FEATURE_ARM64_MOMENT_NEON_H_ */
