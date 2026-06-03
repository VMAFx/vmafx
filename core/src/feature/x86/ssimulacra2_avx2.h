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

#ifndef X86_AVX2_SSIMULACRA2_H_
#define X86_AVX2_SSIMULACRA2_H_

#include <stddef.h>
#include <stdint.h>

#include "feature/ssimulacra2_simd_common.h"

/*
 * AVX2 variants of the SSIMULACRA 2 pointwise, reduction, and
 * separable-IIR kernels. Each function produces byte-for-byte
 * identical output to its scalar counterpart in
 * libvmaf/src/feature/ssimulacra2.c under FLT_EVAL_METHOD == 0
 * — transcendental calls (cbrtf / powf) are applied per-lane
 * via scalar libm to preserve bit-exactness with the scalar
 * reference.
 */

/* mul[i] = a[i] * b[i] for i in [0, 3 * w * h). Pure pointwise mul. */
void ssimulacra2_multiply_3plane_avx2(const float *a, const float *b, float *mul, unsigned w,
                                      unsigned h);

/* Linear RGB → XYB with MakePositiveXYB rescale folded in. Matmul part
 * is 8-wide AVX2; `cbrtf` is per-lane scalar. */
void ssimulacra2_linear_rgb_to_xyb_avx2(const float *lin, float *xyb, unsigned w, unsigned h);

/* 2x2 box downsample of 3 planes from (iw, ih) to (ow, oh) = ((iw+1)/2, (ih+1)/2). */
void ssimulacra2_downsample_2x2_avx2(const float *in, unsigned iw, unsigned ih, float *out,
                                     unsigned *ow_out, unsigned *oh_out);

/* SSIM map pooling — 6 L1/L4 averages (2 per plane). AVX2-accelerated
 * pointwise; pairwise double accumulation via per-lane scalar tail
 * (ADR-0139 pattern). */
void ssimulacra2_ssim_map_avx2(const float *m1, const float *m2, const float *s11, const float *s22,
                               const float *s12, unsigned w, unsigned h, double plane_averages[6]);

/* Edge-diff map pooling — 12 averages (4 per plane). AVX2-accelerated
 * pointwise; per-lane scalar double accumulation. */
void ssimulacra2_edge_diff_map_avx2(const float *img1, const float *mu1, const float *img2,
                                    const float *mu2, unsigned w, unsigned h,
                                    double plane_averages[12]);

/*
 * YUV → linear RGB conversion (ADR-0163). Combines BT.709/BT.601
 * matrix multiplication with the sRGB EOTF. Bit-exact to scalar
 * `picture_to_linear_rgb` under FLT_EVAL_METHOD == 0.
 *
 * `yuv_matrix` is the enum value (0=BT.709 limited, 1=BT.601 limited,
 * 2=BT.709 full, 3=BT.601 full). `bpc` is bits per component (8–16).
 * `planes[0..2]` describe the Y, U, V planes (any chroma subsampling);
 * per-lane scalar fallback handles non-1:1 / non-2:1 chroma ratios.
 * `out` is `3 * w * h` floats, R|G|B planar.
 */
void ssimulacra2_picture_to_linear_rgb_avx2(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                                            const simd_plane_t planes[3], float *out);

/*
 * Two-pass separable FastGaussian IIR blur (libjxl reference).
 * - `rg_n2[3]` / `rg_d1[3]` / `rg_radius` are the Charalampidis 3-pole
 *   coefficients; pre-computed in Ssimu2State during `init()`.
 * - `col_state` is a 6*w scratch for the vertical pass's per-column
 *   IIR state (prev1_{0,1,2}, prev2_{0,1,2}), zeroed inside the fn.
 * - `scratch` is a w*h temp for horizontal-pass output that the
 *   vertical pass consumes.
 * - Output writes to `out` (w*h floats).
 *
 * Byte-for-byte identical to the scalar `blur_plane` under
 * FLT_EVAL_METHOD == 0. The horizontal pass batches 8 rows in
 * parallel via `vpgatherdd` loads; the vertical pass SIMD-iterates
 * 8 columns at a time with contiguous state loads/stores. Scalar
 * tails handle leftover rows (horizontal) and leftover columns
 * (vertical).
 */
void ssimulacra2_blur_plane_avx2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                 float *col_state, const float *in, float *out, float *scratch,
                                 unsigned w, unsigned h);

#endif /* X86_AVX2_SSIMULACRA2_H_ */
