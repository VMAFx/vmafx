/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#ifndef CONVOLUTION_H_
#define CONVOLUTION_H_

/*
Filter widths above this one will not use the AVX path for convolutions.
*/
#define MAX_FWIDTH_AVX_CONV 17

/*
 * All functions listed here expect a SYMMETRICAL filter.
 * All array arguments must be at least 32-byte aligned. AVX-512 paths use
 * unaligned memory ops for source/tmp rows because the project allocator
 * contract is MAX_ALIGN == 32, not 64.
 *
 * filter - convolution kernel
 * filter_width - convolution width (including symmetric side)
 * src - input image
 * dst - output image
 * tmp - temporary array (at least same size as src, even for dec2 versions)
 * width - width of image
 * height - height of image
 * src_stride - distance between lines in src image (pixels, not bytes)
 * dst_stride - distance between lines in dst image (pixels, not bytes)
 */
void convolution_f32_c_s(const float *filter, int filter_width, const float *src, float *dst,
                         float *tmp, int width, int height, int src_stride, int dst_stride);

/* AVX2 paths (256-bit, 8 floats per FMA). */
void convolution_f32_avx_s(const float *filter, int filter_width, const float *src, float *dst,
                           float *tmp, int width, int height, int src_stride, int dst_stride);

void convolution_f32_avx_sq_s(const float *filter, int filter_width, const float *src, float *dst,
                              float *tmp, int width, int height, int src_stride, int dst_stride);

void convolution_f32_avx_xy_s(const float *filter, int filter_width, const float *src1,
                              const float *src2, float *dst, float *tmp, int width, int height,
                              int src1_stride, int src2_stride, int dst_stride);

/* AVX-512F paths (512-bit, 16 floats per FMA). ADR-0504.
 * Results are NOT bit-identical to the AVX2 paths (wider FMA tree changes
 * rounding at the ULP level) but are numerically equivalent within the
 * tolerance of the float VIF path (per ADR-0214 precedent). */
void convolution_f32_avx512_s(const float *filter, int filter_width, const float *src, float *dst,
                              float *tmp, int width, int height, int src_stride, int dst_stride);

void convolution_f32_avx512_sq_s(const float *filter, int filter_width, const float *src,
                                 float *dst, float *tmp, int width, int height, int src_stride,
                                 int dst_stride);

void convolution_f32_avx512_xy_s(const float *filter, int filter_width, const float *src1,
                                 const float *src2, float *dst, float *tmp, int width, int height,
                                 int src1_stride, int src2_stride, int dst_stride);

#endif // CONVOLUTION_H_
