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

#ifndef ARM64_SVE2_SSIMULACRA2_H_
#define ARM64_SVE2_SSIMULACRA2_H_

#include <stddef.h>
#include <stdint.h>

#include "feature/ssimulacra2_simd_common.h"

/*
 * aarch64 SVE2 variants of the SSIMULACRA 2 SIMD kernels (T7-38).
 * Same byte-exact contract as the NEON / AVX2 / AVX-512 TUs (ADR-0161,
 * ADR-0162, ADR-0163) and identical numerical output to the NEON
 * sibling. Predicated lanes via `svwhilelt_b32` for tails; per-lane
 * scalar reductions preserved across reductions and libm (cbrtf /
 * srgb-EOTF) call sites to satisfy ADR-0138 / ADR-0139 / ADR-0140
 * bit-exactness invariants.
 *
 * Runtime gate: getauxval(AT_HWCAP2) & HWCAP2_SVE2 — checked by
 * `init_simd_dispatch` in ssimulacra2.c. NEON remains the fallback;
 * SVE2 is purely additive.
 */

void ssimulacra2_multiply_3plane_sve2(const float *a, const float *b, float *mul, unsigned w,
                                      unsigned h);
void ssimulacra2_linear_rgb_to_xyb_sve2(const float *lin, float *xyb, unsigned w, unsigned h);
void ssimulacra2_downsample_2x2_sve2(const float *in, unsigned iw, unsigned ih, float *out,
                                     unsigned *ow_out, unsigned *oh_out);
void ssimulacra2_ssim_map_sve2(const float *m1, const float *m2, const float *s11, const float *s22,
                               const float *s12, unsigned w, unsigned h, double plane_averages[6]);
void ssimulacra2_edge_diff_map_sve2(const float *img1, const float *mu1, const float *img2,
                                    const float *mu2, unsigned w, unsigned h,
                                    double plane_averages[12]);

void ssimulacra2_picture_to_linear_rgb_sve2(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                                            const simd_plane_t planes[3], float *out);

void ssimulacra2_blur_plane_sve2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                 float *col_state, const float *in, float *out, float *scratch,
                                 unsigned w, unsigned h);

#endif /* ARM64_SVE2_SSIMULACRA2_H_ */
