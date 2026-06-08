/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#ifndef X86_AVX512_MOTION_H_
#define X86_AVX512_MOTION_H_

#include <stddef.h>
#include <stdint.h>

#include "libvmaf/picture.h"

uint64_t motion_score_pipeline_8_avx512(const uint8_t *prev, ptrdiff_t prev_stride,
                                        const uint8_t *cur, ptrdiff_t cur_stride, int32_t *y_row,
                                        unsigned w, unsigned h, unsigned bpc);

uint64_t motion_score_pipeline_16_avx512(const uint8_t *prev, ptrdiff_t prev_stride,
                                         const uint8_t *cur, ptrdiff_t cur_stride, int32_t *y_row,
                                         unsigned w, unsigned h, unsigned bpc);

/* Sub-kernel functions used by unit tests (test_motion_avx512_parity.c). */

void sad_avx512(VmafPicture *pic_a, VmafPicture *pic_b, uint64_t *sad);

void y_convolution_8_avx512(const void *src, uint16_t *dst, unsigned width, unsigned height,
                            ptrdiff_t src_stride, ptrdiff_t dst_stride, unsigned inp_size_bits);

void y_convolution_16_avx512(const void *src, uint16_t *dst, unsigned width, unsigned height,
                             ptrdiff_t src_stride, ptrdiff_t dst_stride, unsigned inp_size_bits);

void x_convolution_16_avx512(const uint16_t *src, uint16_t *dst, unsigned width, unsigned height,
                             ptrdiff_t src_stride, ptrdiff_t dst_stride);

#endif /* X86_AVX512_MOTION_H_ */
