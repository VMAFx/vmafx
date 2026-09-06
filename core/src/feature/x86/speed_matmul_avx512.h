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

#ifndef X86_SPEED_MATMUL_AVX512_H_
#define X86_SPEED_MATMUL_AVX512_H_

/* Bit-exact AVX-512 twin of speed_matmul_scalar(); see feature/speed_matmul.h
 * for the accumulation-order contract this kernel is required to honour. */
void speed_matmul_avx512(float *dst, int dst_stride, const float *x, int x_stride, const float *y,
                         int y_stride, int rows, int inner, int cols);

#endif /* X86_SPEED_MATMUL_AVX512_H_ */
