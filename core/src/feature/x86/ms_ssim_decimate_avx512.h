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
