/*
 * Copyright 2026 Lusoris
 *
 * Licensed under the BSD+Patent License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSDplusPatent
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * AVX2 accelerated helpers for the integer SSIM feature extractor.
 *
 * Two functions are exported:
 *   integer_ssim_accumulate_row_avx2  — horizontally weighted moment
 *       accumulation for 8-bit luma planes, replaces the inner-pixel
 *       k-loop in ssim_accumulate_row().  Bit-exact with the scalar
 *       reference (ADR-0784): accumulation uses int64 accumulators
 *       matching the scalar `m.mux` etc. fields exactly.
 *
 *   integer_ssim_accumulate_row_16_avx2 — same for 16-bit (>8bpc)
 *       planes.
 *
 * See docs/adr/0784-integer-ssim-avx2.md and
 * docs/backends/simd/integer-ssim-avx2.md.
 */

#ifndef X86_AVX2_INTEGER_SSIM_H_
#define X86_AVX2_INTEGER_SSIM_H_

/* integer_ssim_moments_t is defined in the shared header so non-x86 builds
 * (macOS arm64, Windows arm64) can compile integer_ssim.c without pulling in
 * x86 intrinsics.  Include it here so this header remains self-contained for
 * callers that only include integer_ssim_avx2.h directly. */
#include "../integer_ssim.h"

/*
 * integer_ssim_accumulate_row_avx2 — 8bpc horizontal moment pass.
 *
 * Processes `w` output pixels using the supplied `hkernel[0..hkernel_sz)`
 * and `hkernel_offs` (= hkernel_sz / 2).  Writes the ssim_moments struct
 * for each output pixel into `buf[0..w)`.
 *
 * The ssim_moments layout is:
 *   { int64_t mux, muy, x2, xy, y2, w }   (6 × int64, 48 bytes)
 *
 * Both 8-bit (this function) and 16-bit (integer_ssim_accumulate_row_16_avx2)
 * variants are provided.  `src` and `dst` must point to the beginning of the
 * row; the function handles boundary clamping identically to the scalar.
 */

void integer_ssim_accumulate_row_avx2(const uint8_t *src, const uint8_t *dst, int width,
                                      const unsigned *hkernel, int hkernel_sz, int hkernel_offs,
                                      integer_ssim_moments_t *buf);

void integer_ssim_accumulate_row_16_avx2(const uint16_t *src, const uint16_t *dst, int width,
                                         const unsigned *hkernel, int hkernel_sz, int hkernel_offs,
                                         integer_ssim_moments_t *buf);

#endif /* X86_AVX2_INTEGER_SSIM_H_ */
