/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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

#ifndef ARM64_NEON_CAMBI_H_
#define ARM64_NEON_CAMBI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct VmafPicture;
void decimate_neon(struct VmafPicture *image, unsigned width, unsigned height);
void anti_dithering_filter_neon(struct VmafPicture *pic, unsigned width, unsigned height);
void filter_mode_neon(const struct VmafPicture *image, int width, int height, uint16_t *buffer);

void get_derivative_data_for_row_neon(const uint16_t *image_data, uint16_t *derivative_buffer,
                                      int width, int height, int row, int stride);

void calculate_c_values_row_neon(float *c_values, const uint16_t *histograms, const uint16_t *image,
                                 const uint16_t *mask, int row, int width, ptrdiff_t stride,
                                 const uint16_t num_diffs, const uint16_t *tvi_thresholds,
                                 uint16_t vlt_luma, const int *diff_weights, const int *all_diffs,
                                 const float *reciprocal_lut);

void calculate_c_values_neon(struct VmafPicture *pic, const struct VmafPicture *mask_pic,
                             float *c_values, uint16_t *histograms, uint16_t window_size,
                             const uint16_t num_diffs, const uint16_t *tvi_for_diff,
                             uint16_t vlt_luma, const int *diff_weights, const int *all_diffs,
                             int width, int height);

void compute_dp_row_neon(uint32_t *dp_curr, const uint32_t *dp_prev, const uint16_t *deriv,
                         int width, int pad_size, bool deriv_valid);

void compute_mask_row_neon(uint16_t *mask_row, const uint32_t *dp_bottom, const uint32_t *dp_top,
                           int width, int pad_size, uint32_t mask_index);

#endif /* ARM64_NEON_CAMBI_H_ */
