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

#ifndef X86_AVX2_PSNR_HVS_H_
#define X86_AVX2_PSNR_HVS_H_

#include <stdint.h>

/*
 * AVX2 variant of the 8x8 Daala/Xiph forward integer DCT butterfly
 * (`od_bin_fdct8x8`). Byte-for-byte identical output to the scalar
 * reference in libvmaf/src/feature/third_party/xiph/psnr_hvs.c under
 * FLT_EVAL_METHOD == 0. Operates on a single 8x8 block of int32
 * coefficients (row-major, `ystride == xstride == 8`).
 *
 * Only the integer DCT is vectorised. The outer `calc_psnrhvs`
 * per-block float accumulations (means / vars / mask / err) remain
 * scalar because their left-to-right summation order is
 * observable and matching it in SIMD would provide no speedup.
 */
void od_bin_fdct8x8_avx2(int32_t *y, int32_t ystride, const int32_t *x, int32_t xstride);

/*
 * AVX2 variant of calc_psnrhvs. Same signature and byte-identical
 * output (final double) as the scalar reference. Internally calls
 * `od_bin_fdct8x8_avx2` for each strided 8x8 block.
 */
double calc_psnrhvs_avx2(const unsigned char *src, int systride, const unsigned char *dst,
                         int dystride, double par, int depth, int w, int h, int step,
                         float csf[8][8]);

#endif /* X86_AVX2_PSNR_HVS_H_ */
