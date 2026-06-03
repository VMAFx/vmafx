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
