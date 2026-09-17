/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 */

#ifndef VMAF_MS_SSIM_DECIMATE_AVX2_H_
#define VMAF_MS_SSIM_DECIMATE_AVX2_H_

/*
 * AVX2 specialisation of ms_ssim_decimate_scalar.
 *
 * Bit-identical output contract: for every (src, w, h) pair this
 * function produces byte-for-byte the same `dst` as
 * `ms_ssim_decimate_scalar`. Lane-parallel evaluation uses per-lane
 * sequential `_mm256_fmadd_ps` with broadcast coefficients, so each
 * lane's accumulation order exactly mirrors the scalar reference's
 * `fmaf` chain. Border columns / rows where the 9-tap kernel would
 * cross the image edge are handled by an inline scalar fallback using
 * the same `fmaf` + KBND_SYMMETRIC mirror as the scalar reference.
 *
 * Tested in libvmaf/test/test_ms_ssim_decimate.c — the byte-identity
 * assertion runs on synthetic + real-YUV inputs.
 *
 * Invariants (rebase-sensitive; see libvmaf/src/feature/AGENTS.md):
 *   - Coefficients in this TU MUST equal `ms_ssim_lpf_{h,v}` in
 *     libvmaf/src/feature/ms_ssim_decimate.c.
 *   - Mirror semantics MUST equal `ms_ssim_decimate_mirror` in the
 *     scalar reference.
 */

int ms_ssim_decimate_avx2(const float *src, int w, int h, float *dst, int *rw, int *rh);

#endif /* VMAF_MS_SSIM_DECIMATE_AVX2_H_ */
