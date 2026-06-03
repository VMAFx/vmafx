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

#ifndef ARM64_NEON_SSIMULACRA2_HOST_H_
#define ARM64_NEON_SSIMULACRA2_HOST_H_

#include <stddef.h>

/*
 * aarch64 NEON variants of the ssimulacra2 Vulkan-host kernels (ADR-0242).
 * 4-wide float lanes. Same plane_stride convention as the AVX2 sibling:
 * channel p starts at `base + p * plane_stride`, where plane_stride >= w*h.
 *
 * Bit-exact contract: ADR-0161 / ADR-0242 — per-lane scalar cbrtf,
 * `#pragma STDC FP_CONTRACT OFF`, `-ffp-contract=off` compile flag.
 */

void ssimulacra2_host_linear_rgb_to_xyb_neon(const float *lin, float *xyb, unsigned w, unsigned h,
                                             size_t plane_stride);

void ssimulacra2_host_downsample_2x2_neon(const float *in, unsigned iw, unsigned ih, float *out,
                                          unsigned ow, unsigned oh, size_t plane_stride);

#endif /* ARM64_NEON_SSIMULACRA2_HOST_H_ */
