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

#ifndef X86_AVX512_SSIMULACRA2_H_
#define X86_AVX512_SSIMULACRA2_H_

#include <stddef.h>
#include <stdint.h>

#include "feature/ssimulacra2_simd_common.h"

/*
 * AVX-512 variants of the SSIMULACRA 2 SIMD kernels. Same bit-exact
 * contract as the AVX2 TU (ADR-0161) — 16-wide float lanes instead of
 * 8. Transcendental (`cbrtf`) calls remain per-lane scalar libm.
 */

void ssimulacra2_multiply_3plane_avx512(const float *a, const float *b, float *mul, unsigned w,
                                        unsigned h);
void ssimulacra2_linear_rgb_to_xyb_avx512(const float *lin, float *xyb, unsigned w, unsigned h);
void ssimulacra2_downsample_2x2_avx512(const float *in, unsigned iw, unsigned ih, float *out,
                                       unsigned *ow_out, unsigned *oh_out);
void ssimulacra2_ssim_map_avx512(const float *m1, const float *m2, const float *s11,
                                 const float *s22, const float *s12, unsigned w, unsigned h,
                                 double plane_averages[6]);
void ssimulacra2_edge_diff_map_avx512(const float *img1, const float *mu1, const float *img2,
                                      const float *mu2, unsigned w, unsigned h,
                                      double plane_averages[12]);

/* YUV → linear RGB (ADR-0163). 16-wide AVX-512 port of
 * `ssimulacra2_picture_to_linear_rgb_avx2`. See the AVX2 header for the
 * full contract. */
void ssimulacra2_picture_to_linear_rgb_avx512(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                                              const simd_plane_t planes[3], float *out);

/* Two-pass FastGaussian IIR blur. See ADR-0162 and ssimulacra2_avx2.h for
 * the full contract. 16-wide AVX-512 port. */
void ssimulacra2_blur_plane_avx512(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                   float *col_state, const float *in, float *out, float *scratch,
                                   unsigned w, unsigned h);

#endif /* X86_AVX512_SSIMULACRA2_H_ */
