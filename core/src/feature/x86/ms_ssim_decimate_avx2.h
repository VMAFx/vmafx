/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#ifndef __VMAF_MS_SSIM_DECIMATE_AVX2_H__
#define __VMAF_MS_SSIM_DECIMATE_AVX2_H__

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

#endif /* __VMAF_MS_SSIM_DECIMATE_AVX2_H__ */
