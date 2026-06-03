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

#ifndef VMAF_FEATURE_ARM64_MOTION_V2_NEON_H_
#define VMAF_FEATURE_ARM64_MOTION_V2_NEON_H_

#include <stddef.h>
#include <stdint.h>

/* motion_v2 NEON fast paths for 8-bit and 10/12-bit inputs. Signatures
 * mirror the AVX2 variants in [`../x86/motion_v2_avx2.h`](../x86/motion_v2_avx2.h).
 * Bit-exact vs the scalar references `motion_score_pipeline_{8,16}` in
 * `integer_motion_v2.c`. See ADR-0145. */
uint64_t motion_score_pipeline_8_neon(const uint8_t *prev, ptrdiff_t prev_stride,
                                      const uint8_t *cur, ptrdiff_t cur_stride, int32_t *y_row,
                                      unsigned w, unsigned h, unsigned bpc);

uint64_t motion_score_pipeline_16_neon(const uint8_t *prev_u8, ptrdiff_t prev_stride,
                                       const uint8_t *cur_u8, ptrdiff_t cur_stride, int32_t *y_row,
                                       unsigned w, unsigned h, unsigned bpc);

#endif /* VMAF_FEATURE_ARM64_MOTION_V2_NEON_H_ */
