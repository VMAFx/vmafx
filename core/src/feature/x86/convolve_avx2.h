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

#ifndef LIBVMAF_FEATURE_X86_CONVOLVE_AVX2_H
#define LIBVMAF_FEATURE_X86_CONVOLVE_AVX2_H

/*
 * AVX2 bit-exact fast path for `iqa_convolve` — 1-D separable,
 * 11-tap Gaussian or 8-tap box kernel, normalised, no border
 * reflection.
 *
 * Primitive-argument signature decouples x86 SIMD sources from the
 * vendored iqa/convolve.h `struct iqa_kernel` (keeps the x86_avx2 static
 * library's include set narrow, matching the rest of libvmaf/src/feature/x86/).
 *
 * Bit-identical to the scalar reference by construction: `__m256d`
 * (4-lane double) accumulator with separate `_mm256_mul_pd` /
 * `_mm256_add_pd` to mirror the scalar's unfused `sum += a*b`.
 *
 * See docs/adr/0138-iqa-convolve-avx2-bitexact-double.md and
 * docs/research/0011-iqa-convolve-avx2.md.
 */
void iqa_convolve_avx2(float *img, int w, int h, const float *kernel_h, const float *kernel_v,
                       int kw, int kh, int normalized, float *workspace, float *result, int *rw,
                       int *rh);

#endif /* LIBVMAF_FEATURE_X86_CONVOLVE_AVX2_H */
