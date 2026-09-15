/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef ARM64_NEON_CONVOLVE_H_
#define ARM64_NEON_CONVOLVE_H_

/*
 * NEON bit-exact fast path for `iqa_convolve` — 1-D separable,
 * interior-only (no boundary reflection).
 *
 * Signature matches x86 convolve_avx2.h / convolve_avx512.h so the
 * SSIM dispatch in ssim_tools.c can install either via
 * `iqa_convolve_set_dispatch`. See ADR-0138 for the scalar-matching
 * invariant and ADR-0140 for the simd_dx.h macros used inside.
 */
void iqa_convolve_neon(float *img, int w, int h, const float *kernel_h, const float *kernel_v,
                       int kw, int kh, int normalized, float *workspace, float *result, int *rw,
                       int *rh);

#endif /* ARM64_NEON_CONVOLVE_H_ */
