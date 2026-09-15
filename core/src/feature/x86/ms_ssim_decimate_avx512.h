/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef __VMAF_MS_SSIM_DECIMATE_AVX512_H__
#define __VMAF_MS_SSIM_DECIMATE_AVX512_H__

/*
 * AVX-512 specialisation of ms_ssim_decimate_scalar.
 *
 * Bit-identical output contract: byte-for-byte equal to
 * `ms_ssim_decimate_scalar` and `ms_ssim_decimate_avx2`. Per-lane
 * evaluation uses `_mm512_fmadd_ps` with broadcast coefficients, so
 * every lane's k-loop FMA chain matches the scalar `fmaf` chain.
 * Border columns / rows use the same scalar fallback as the AVX2
 * variant.
 *
 * Invariants (rebase-sensitive; see libvmaf/src/feature/AGENTS.md):
 *   - Coefficients in this TU MUST equal `ms_ssim_lpf_{h,v}` in
 *     libvmaf/src/feature/ms_ssim_decimate.c and the AVX2 variant.
 *   - Mirror semantics MUST equal the scalar reference's
 *     `ms_ssim_decimate_mirror`.
 */

int ms_ssim_decimate_avx512(const float *src, int w, int h, float *dst, int *rw, int *rh);

#endif /* __VMAF_MS_SSIM_DECIMATE_AVX512_H__ */
