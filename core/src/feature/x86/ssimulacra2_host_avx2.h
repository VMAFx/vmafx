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

#ifndef X86_AVX2_SSIMULACRA2_HOST_H_
#define X86_AVX2_SSIMULACRA2_HOST_H_

#include <stddef.h>

/*
 * AVX2 variants of the ssimulacra2 Vulkan-host kernels (ADR-0242).
 *
 * These differ from the CPU-extractor SIMD kernels in ssimulacra2_avx2.h
 * in one way: they accept an explicit `plane_stride` parameter (measured
 * in floats) to support the Vulkan pyramid layout where each plane
 * occupies a fixed-size slot equal to the full-resolution frame size,
 * even at downsampled scales.
 *
 * Bit-exact contract: same as ADR-0161 — lane-commutative pointwise
 * arithmetic, per-lane scalar `cbrtf`, `#pragma STDC FP_CONTRACT OFF`,
 * `-ffp-contract=off` compile flag.
 */

/*
 * Linear RGB → XYB with MakePositiveXYB rescale, plane_stride form.
 * `lin`  : 3-plane buffer; plane p begins at `lin + p * plane_stride`.
 * `xyb`  : 3-plane output; same plane_stride layout.
 * `w`,`h`: actual pixel dimensions of this scale's data.
 * `plane_stride`: floats per plane slot (>= w*h).
 */
void ssimulacra2_host_linear_rgb_to_xyb_avx2(const float *lin, float *xyb, unsigned w, unsigned h,
                                             size_t plane_stride);

/*
 * 2×2 box downsample, plane_stride form.
 * `in`  : input buffer (iw × ih pixels per plane, plane_stride slot size).
 * `out` : output buffer (ow × oh pixels per plane, same plane_stride slot).
 * ow = (iw + 1) / 2, oh = (ih + 1) / 2 (must be pre-computed by caller).
 */
void ssimulacra2_host_downsample_2x2_avx2(const float *in, unsigned iw, unsigned ih, float *out,
                                          unsigned ow, unsigned oh, size_t plane_stride);

#endif /* X86_AVX2_SSIMULACRA2_HOST_H_ */
