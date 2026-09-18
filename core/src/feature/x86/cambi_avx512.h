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

#ifndef X86_AVX512_CAMBI_H_
#define X86_AVX512_CAMBI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cambi_increment_range_avx512(uint16_t *arr, int left, int right);

void cambi_decrement_range_avx512(uint16_t *arr, int left, int right);

void get_derivative_data_for_row_avx512(const uint16_t *image_data, uint16_t *derivative_buffer,
                                        int width, int height, int row, int stride);

void calculate_c_values_row_avx512(float *c_values, const uint16_t *histograms,
                                   const uint16_t *image, const uint16_t *mask, int row, int width,
                                   ptrdiff_t stride, const uint16_t num_diffs,
                                   const uint16_t *tvi_thresholds, uint16_t vlt_luma,
                                   const int *diff_weights, const int *all_diffs,
                                   const float *reciprocal_lut);

void compute_dp_row_avx512(uint32_t *dp_curr, const uint32_t *dp_prev, const uint16_t *deriv,
                           int width, int pad_size, bool deriv_valid);

void compute_mask_row_avx512(uint16_t *mask_row, const uint32_t *dp_bottom, const uint32_t *dp_top,
                             int width, int pad_size, uint32_t mask_index);

#endif /* X86_AVX512_CAMBI_H_ */
