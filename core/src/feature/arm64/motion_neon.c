/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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

#include <arm_neon.h>
#include <stdbool.h>
#include <stddef.h>

#include "feature/integer_motion.h"
#include "feature/arm64/motion_neon.h"
#include "feature/common/alignment.h"

/* filter[] applied to four columns: widening multiply-accumulate of the five
 * taps, then round and shift right by 16. */
static inline uint32x4_t filter5_u16x4_neon(uint16x4_t s0, uint16x4_t s1, uint16x4_t s2,
                                            uint16x4_t s3, uint16x4_t s4)
{
    const uint16x4_t k0 = vdup_n_u16(3571);
    const uint16x4_t k1 = vdup_n_u16(16004);
    const uint16x4_t k2 = vdup_n_u16(26386);
    const uint32x4_t addnum = vdupq_n_u32(32768);

    uint32x4_t acc = vmull_u16(s0, k0);
    acc = vmlal_u16(acc, s1, k1);
    acc = vmlal_u16(acc, s2, k2);
    acc = vmlal_u16(acc, s3, k1);
    acc = vmlal_u16(acc, s4, k0);
    acc = vaddq_u32(acc, addnum);
    return vshrq_n_u32(acc, 16);
}

/* The horizontal filter for the 8 columns whose first tap is sp[0]; reads
 * sp[0] .. sp[11]. */
static inline uint16x8_t x_conv8_neon(const uint16_t *sp)
{
    /* Load 5 overlapping vectors of 8 uint16 for the 5-tap filter */
    uint16x8_t s0 = vld1q_u16(sp);
    uint16x8_t s1 = vld1q_u16(sp + 1);
    uint16x8_t s2 = vld1q_u16(sp + 2);
    uint16x8_t s3 = vld1q_u16(sp + 3);
    uint16x8_t s4 = vld1q_u16(sp + 4);

    uint32x4_t acc_lo = filter5_u16x4_neon(vget_low_u16(s0), vget_low_u16(s1), vget_low_u16(s2),
                                           vget_low_u16(s3), vget_low_u16(s4));
    uint32x4_t acc_hi = filter5_u16x4_neon(vget_high_u16(s0), vget_high_u16(s1), vget_high_u16(s2),
                                           vget_high_u16(s3), vget_high_u16(s4));

    /* Narrow back to uint16 */
    return vcombine_u16(vmovn_u32(acc_lo), vmovn_u32(acc_hi));
}

/* Mirror-boundary columns [first, last) of row i. */
static void x_conv_edge_cols_neon(const uint16_t *src, uint16_t *dst_row, unsigned width,
                                  unsigned height, ptrdiff_t src_stride, unsigned i, unsigned first,
                                  unsigned last)
{
    const unsigned shift_add_round = 32768;
    for (unsigned j = first; j < last; j++) {
        dst_row[j] =
            (edge_16(true, src, width, height, (int)src_stride, i, j) + shift_add_round) >> 16;
    }
}

/* Interior columns [left_edge, right_edge) of one row: 8 uint16 per iteration,
 * then a scalar tail. src_row points at the first tap of column left_edge. */
static void x_conv_row_interior_neon(const uint16_t *src_row, uint16_t *dst_row, unsigned left_edge,
                                     unsigned right_edge)
{
    const unsigned shift_add_round = 32768;
    unsigned j = left_edge;

    for (; j + 8 <= right_edge; j += 8) {
        vst1q_u16(dst_row + j, x_conv8_neon(src_row));
        src_row += 8;
    }

    /* Scalar tail for remaining interior pixels */
    for (; j < right_edge; j++) {
        uint32_t accum = 0;
        const uint16_t *sp = src_row;
        for (int k = 0; k < filter_width; ++k) {
            accum += filter[k] * sp[k];
        }
        dst_row[j] = (accum + shift_add_round) >> 16;
        src_row++;
    }
}

void x_convolution_16_neon(const uint16_t *src, uint16_t *dst, unsigned width, unsigned height,
                           ptrdiff_t src_stride, ptrdiff_t dst_stride)
{
    const unsigned radius = filter_width / 2;
    const unsigned left_edge = vmaf_ceiln(radius, 1);
    const unsigned right_edge = vmaf_floorn(width - (filter_width - radius), 1);

    /* Edge pixels: left */
    for (unsigned i = 0; i < height; ++i) {
        x_conv_edge_cols_neon(src, dst + i * dst_stride, width, height, src_stride, i, 0,
                              left_edge);
    }

    /* Interior pixels: NEON vectorized */
    for (unsigned i = 0; i < height; ++i) {
        x_conv_row_interior_neon(src + i * src_stride + (left_edge - radius), dst + i * dst_stride,
                                 left_edge, right_edge);
    }

    /* Edge pixels: right */
    for (unsigned i = 0; i < height; ++i) {
        x_conv_edge_cols_neon(src, dst + i * dst_stride, width, height, src_stride, i, right_edge,
                              width);
    }
}
