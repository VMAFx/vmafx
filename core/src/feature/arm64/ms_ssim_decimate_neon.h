/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef __VMAF_MS_SSIM_DECIMATE_NEON_H__
#define __VMAF_MS_SSIM_DECIMATE_NEON_H__

/*
 * NEON specialisation of ms_ssim_decimate_scalar.
 *
 * Bit-identical output contract: byte-for-byte equal to
 * `ms_ssim_decimate_scalar` and the x86 AVX2/AVX-512 variants.
 * Per-lane evaluation uses `vfmaq_n_f32` (ARMv8-A FMA, single
 * rounding) with broadcast coefficients, so every lane's k-loop FMA
 * chain matches the scalar `fmaf` chain. Border columns / rows use
 * the same scalar fallback as the x86 variants.
 *
 * Stride-2 horizontal deinterleave uses `vld2q_f32`, which splits 8
 * contiguous source floats into two float32x4_t vectors: .val[0] =
 * even lanes [p0, p2, p4, p6], .val[1] = odd lanes.
 *
 * Invariants (rebase-sensitive; see libvmaf/src/feature/AGENTS.md):
 *   - Coefficients in this TU MUST equal `ms_ssim_lpf_{h,v}` in
 *     libvmaf/src/feature/ms_ssim_decimate.c and the x86 variants.
 *   - Mirror semantics MUST equal the scalar reference's
 *     `ms_ssim_decimate_mirror`.
 */

int ms_ssim_decimate_neon(const float *src, int w, int h, float *dst, int *rw, int *rh);

#endif /* __VMAF_MS_SSIM_DECIMATE_NEON_H__ */
