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

#ifndef ARM_64_ADM_H_
#define ARM_64_ADM_H_

#include "feature/integer_adm.h"

void adm_dwt2_8_neon(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w, int h,
                     int src_stride, int dst_stride);

/* Production-only compatibility entry point for the immutable Darwin AArch64
 * score contract. Direct SIMD parity tests must use adm_dwt2_8_neon(). */
void adm_dwt2_8_neon_apple_legacy(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf,
                                  int w, int h, int src_stride, int dst_stride);

#endif /* ARM64_ADM_H_ */
