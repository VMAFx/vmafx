/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#ifndef ARM_64_ADM_H_
#define ARM_64_ADM_H_

#include <stdbool.h>
#include <stdint.h>

#include "feature/integer_adm.h"

void adm_dwt2_8_neon(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w, int h,
                     int src_stride, int dst_stride);

/* Scale-zero decouple, bit-identical to the scalar adm_decouple_cols() at every
 * enhancement gain limit (Netflix/vmaf 9e48141b). */
void adm_decouple_neon(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                       int32_t *adm_div_lookup);

/* Contrast masking of scale 0 and of scales 1 to 3, bit-identical to the
 * scalar adm_cm() / i4_adm_cm() of integer_adm.c (Netflix/vmaf 8bc5a5c6a,
 * b41d2340a; the fork's int32 centre tap and int64 excess, ADR-1402). */
float adm_cm_neon(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride,
                  double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                  double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                  double adm_p_norm, bool measure_aim);

float i4_adm_cm_neon(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride, int scale,
                     double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                     double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                     double adm_p_norm, bool measure_aim);

/* Daubechies-2 DWT of scales 1 to 3 for both pictures, bit-identical to the
 * scalar adm_dwt2_s123_combined() (Netflix/vmaf b41d2340a). */
void adm_dwt2_s123_combined_neon(const int32_t *i4_ref_scale, const int32_t *i4_curr_dis,
                                 AdmBuffer *buf, int w, int h, int ref_stride, int dis_stride,
                                 int dst_stride, int scale);

/* Decouple of scales 1 to 3, bit-identical to the scalar adm_decouple_s123()
 * at every enhancement gain limit; the vector path serves a limit of 1
 * (Netflix/vmaf b41d2340a). */
void adm_decouple_s123_neon(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                            int32_t *adm_div_lookup);

#endif /* ARM64_ADM_H_ */
