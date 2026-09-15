/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAF_FEATURE_ARM64_MOTION_V2_NEON_H_
#define VMAF_FEATURE_ARM64_MOTION_V2_NEON_H_

#include <stddef.h>
#include <stdint.h>

/* motion_v2 NEON fast paths for 8-bit and 10/12-bit inputs. Signatures
 * mirror the AVX2 variants in [`../x86/motion_avx2.h`](../x86/motion_avx2.h).
 * Bit-exact vs the scalar references `motion_score_pipeline_{8,16}` in
 * `integer_motion_v2.c`. See ADR-0145. */
uint64_t motion_score_pipeline_8_neon(const uint8_t *prev, ptrdiff_t prev_stride,
                                      const uint8_t *cur, ptrdiff_t cur_stride, int32_t *y_row,
                                      unsigned w, unsigned h, unsigned bpc);

uint64_t motion_score_pipeline_16_neon(const uint8_t *prev_u8, ptrdiff_t prev_stride,
                                       const uint8_t *cur_u8, ptrdiff_t cur_stride, int32_t *y_row,
                                       unsigned w, unsigned h, unsigned bpc);

#endif /* VMAF_FEATURE_ARM64_MOTION_V2_NEON_H_ */
