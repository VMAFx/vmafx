/**
 *
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 */

#ifndef LIBVMAF_FEATURE_X86_CONVOLVE_AVX512_H
#define LIBVMAF_FEATURE_X86_CONVOLVE_AVX512_H

/*
 * AVX-512 bit-exact fast path for `iqa_convolve` — 1-D separable,
 * 11-tap Gaussian or 8-tap box kernel, normalised, no border
 * reflection.
 *
 * Same primitive-args signature as `iqa_convolve_avx2`; the only
 * difference is the lane width (`__m512d` 8-lane vs `__m256d` 4-lane).
 * Bit-identical to the scalar reference by the same construction:
 * separate `_mm512_mul_pd` + `_mm512_add_pd` (no FMA), float→double
 * widen at load, double→float round at store.
 *
 * Requires AVX-512F (`_mm512_cvtps_pd`, `_mm512_cvtpd_ps`). No
 * additional ISA beyond what `ssim_avx512.c` already mandates for
 * the ssim_*_avx512 dispatch targets in the same static lib.
 *
 * See docs/adr/0138-iqa-convolve-avx2-bitexact-double.md §Follow-up.
 */
void iqa_convolve_avx512(float *img, int w, int h, const float *kernel_h, const float *kernel_v,
                         int kw, int kh, int normalized, float *workspace, float *result, int *rw,
                         int *rh);

#endif /* LIBVMAF_FEATURE_X86_CONVOLVE_AVX512_H */
