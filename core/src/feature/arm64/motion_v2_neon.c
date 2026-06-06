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

/*
 * motion_v2 NEON port — pairs with the AVX2 path in
 * `x86/motion_v2_avx2.c`. Bit-exact vs scalar `motion_score_pipeline_{8,16}`
 * in `integer_motion_v2.c`.
 *
 * The pipeline is:
 *   Phase 1: 5-row vertical convolve on (prev - cur), produces int32 `y_row`.
 *   Phase 2: 5-tap horizontal convolve on y_row, abs + sum → row SAD.
 *
 * Mirror reflection at both the outer row loop and the inner column loop;
 * NEON uses a scalar `mirror()` helper identical to AVX2/scalar.
 *
 * Float-free integer math. The bpc-dependent right shift uses NEON's
 * variable shift via `vshlq_s64(v, vneg(bpc_vec))` — arithmetic on
 * signed int64 to match scalar C's `>>` on `int64_t` (AVX2 uses
 * `_mm256_srlv_epi64` which is *logical*; documented as a small divergence
 * between AVX2 and scalar, see ADR-0145 §Alternatives considered).
 */

#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "feature/arm64/motion_v2_neon.h"
#include "feature/integer_motion.h"

static inline int mirror(int idx, int size)
{
    if (idx < 0)
        return -idx;
    if (idx >= size)
        return 2 * size - idx - 2;
    return idx;
}

/* Horizontal sum of 4×uint32 → scalar uint32. */
static inline uint32_t neon_hadd_u32(uint32x4_t v)
{
    return vaddvq_u32(v);
}

/* Non-zero test across 4×int32 lanes: returns non-zero iff any bit is set
 * in any lane.  Uses bitwise OR-fold rather than arithmetic sum so that
 * positive and negative signed values cannot cancel each other to give a
 * false zero (e.g. checkerboard frames trigger this on the signed-sum path).
 */
static inline uint32_t neon_any_nonzero_s32(int32x4_t v)
{
    const uint64x2_t u64 = vreinterpretq_u64_s32(v);
    return (uint32_t)(vgetq_lane_u64(u64, 0) | vgetq_lane_u64(u64, 1));
}

/* Compute abs(x_conv) for 4 int32 lanes starting at y_row[j] using
 * centre-tap offsets {-2,-1,0,1,2} and the 5-tap Gaussian `filter`. */
static inline int32x4_t x_conv_block4_neon(const int32_t *y_row, unsigned j, int32x4_t g0,
                                           int32x4_t g1, int32x4_t g2, int64x2_t round64)
{
    const int32x4_t y0 = vld1q_s32(y_row + j - 2);
    const int32x4_t y1 = vld1q_s32(y_row + j - 1);
    const int32x4_t y2 = vld1q_s32(y_row + j + 0);
    const int32x4_t y3 = vld1q_s32(y_row + j + 1);
    const int32x4_t y4 = vld1q_s32(y_row + j + 2);

    int64x2_t sum_lo = vmull_s32(vget_low_s32(y0), vget_low_s32(g0));
    sum_lo = vaddq_s64(sum_lo, vmull_s32(vget_low_s32(y1), vget_low_s32(g1)));
    sum_lo = vaddq_s64(sum_lo, vmull_s32(vget_low_s32(y2), vget_low_s32(g2)));
    sum_lo = vaddq_s64(sum_lo, vmull_s32(vget_low_s32(y3), vget_low_s32(g1)));
    sum_lo = vaddq_s64(sum_lo, vmull_s32(vget_low_s32(y4), vget_low_s32(g0)));

    int64x2_t sum_hi = vmull_high_s32(y0, g0);
    sum_hi = vaddq_s64(sum_hi, vmull_high_s32(y1, g1));
    sum_hi = vaddq_s64(sum_hi, vmull_high_s32(y2, g2));
    sum_hi = vaddq_s64(sum_hi, vmull_high_s32(y3, g1));
    sum_hi = vaddq_s64(sum_hi, vmull_high_s32(y4, g0));

    sum_lo = vshrq_n_s64(vaddq_s64(sum_lo, round64), 16);
    sum_hi = vshrq_n_s64(vaddq_s64(sum_hi, round64), 16);

    return vabsq_s32(vcombine_s32(vmovn_s64(sum_lo), vmovn_s64(sum_hi)));
}

/* Scalar x_conv + abs + SAD for a single column `j` with mirror-boundary
 * tap addressing. Shared by the left/right-edge tails of the NEON
 * `x_conv_row_sad_neon`. */
static inline uint32_t x_conv_edge_one_col(const int32_t *y_row, unsigned j, unsigned w)
{
    int64_t accum = 0;
    for (int k = 0; k < 5; k++) {
        const int col = mirror((int)j - 2 + k, (int)w);
        accum += (int64_t)filter[k] * y_row[col];
    }
    const int32_t val = (int32_t)((accum + (1 << 15)) >> 16);
    return (uint32_t)abs(val);
}

/* Phase 2: x_conv + abs + SAD for one row of int32 `y_row`. Bit-exact to
 * the scalar x_conv in `integer_motion_v2.c`. */
static inline uint32_t x_conv_row_sad_neon(const int32_t *y_row, unsigned w)
{
    const int32x4_t g0 = vdupq_n_s32(3571);
    const int32x4_t g1 = vdupq_n_s32(16004);
    const int32x4_t g2 = vdupq_n_s32(26386);
    const int64x2_t round64 = vdupq_n_s64(1 << 15);

    uint32_t row_sad = 0;

    unsigned j;
    for (j = 0; j < 2 && j < w; j++)
        row_sad += x_conv_edge_one_col(y_row, j, w);

    /* SIMD middle: need y_row[j-2]..y_row[j+5], so j+6 <= w at 4-lane stride. */
    uint32x4_t sad_acc = vdupq_n_u32(0);
    for (; j + 6 <= w; j += 4) {
        const int32x4_t abs_row = x_conv_block4_neon(y_row, j, g0, g1, g2, round64);
        sad_acc = vaddq_u32(sad_acc, vreinterpretq_u32_s32(abs_row));
    }
    row_sad += neon_hadd_u32(sad_acc);

    for (; j < w; j++)
        row_sad += x_conv_edge_one_col(y_row, j, w);

    return row_sad;
}

/* One 4-lane SIMD step of phase-1 for the 16-bit pipeline: load 4 uint16s
 * from each of the 5 rows, diff, 5-tap int32×int32 → int64 convolve,
 * round, shift by `bpc`, narrow to int32, store into `y_row + j`.
 * Returns the int32x4 vector so the caller can OR-accumulate it for
 * the all-zero fast-path check. */
static inline int32x4_t y_conv_row_step16_neon(const uint16_t *pp[5], const uint16_t *cp[5],
                                               unsigned j, int32x4_t g0, int32x4_t g1, int32x4_t g2,
                                               int64x2_t y_round, int64x2_t bpc_neg, int32_t *y_row)
{
    const int32x4_t d0 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vld1_u16(pp[0] + j))),
                                   vreinterpretq_s32_u32(vmovl_u16(vld1_u16(cp[0] + j))));
    const int32x4_t d1 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vld1_u16(pp[1] + j))),
                                   vreinterpretq_s32_u32(vmovl_u16(vld1_u16(cp[1] + j))));
    const int32x4_t d2 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vld1_u16(pp[2] + j))),
                                   vreinterpretq_s32_u32(vmovl_u16(vld1_u16(cp[2] + j))));
    const int32x4_t d3 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vld1_u16(pp[3] + j))),
                                   vreinterpretq_s32_u32(vmovl_u16(vld1_u16(cp[3] + j))));
    const int32x4_t d4 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vld1_u16(pp[4] + j))),
                                   vreinterpretq_s32_u32(vmovl_u16(vld1_u16(cp[4] + j))));

    int64x2_t acc_lo = vmull_s32(vget_low_s32(d0), vget_low_s32(g0));
    acc_lo = vaddq_s64(acc_lo, vmull_s32(vget_low_s32(d1), vget_low_s32(g1)));
    acc_lo = vaddq_s64(acc_lo, vmull_s32(vget_low_s32(d2), vget_low_s32(g2)));
    acc_lo = vaddq_s64(acc_lo, vmull_s32(vget_low_s32(d3), vget_low_s32(g1)));
    acc_lo = vaddq_s64(acc_lo, vmull_s32(vget_low_s32(d4), vget_low_s32(g0)));

    int64x2_t acc_hi = vmull_high_s32(d0, g0);
    acc_hi = vaddq_s64(acc_hi, vmull_high_s32(d1, g1));
    acc_hi = vaddq_s64(acc_hi, vmull_high_s32(d2, g2));
    acc_hi = vaddq_s64(acc_hi, vmull_high_s32(d3, g1));
    acc_hi = vaddq_s64(acc_hi, vmull_high_s32(d4, g0));

    acc_lo = vshlq_s64(vaddq_s64(acc_lo, y_round), bpc_neg);
    acc_hi = vshlq_s64(vaddq_s64(acc_hi, y_round), bpc_neg);

    const int32x4_t row = vcombine_s32(vmovn_s64(acc_lo), vmovn_s64(acc_hi));
    vst1q_s32(y_row + j, row);
    return row;
}

/* Scalar phase-1 step for column `j` of the 16-bit pipeline. Writes
 * `y_row[j]` and returns it for the all-zero fast-path check. */
static inline int32_t y_conv_col_scalar16(const uint16_t *pp[5], const uint16_t *cp[5], unsigned j,
                                          unsigned bpc, int32_t *y_row)
{
    int64_t accum = 0;
    for (int k = 0; k < 5; k++) {
        const int32_t diff = pp[k][j] - cp[k][j];
        accum += (int64_t)filter[k] * diff;
    }
    const int32_t v = (int32_t)((accum + ((int64_t)1 << (bpc - 1))) >> bpc);
    y_row[j] = v;
    return v;
}

uint64_t motion_score_pipeline_16_neon(const uint8_t *prev_u8, ptrdiff_t prev_stride,
                                       const uint8_t *cur_u8, ptrdiff_t cur_stride, int32_t *y_row,
                                       unsigned w, unsigned h, unsigned bpc)
{
    const uint16_t *prev = (const uint16_t *)prev_u8;
    const uint16_t *cur = (const uint16_t *)cur_u8;
    const ptrdiff_t p_stride = prev_stride / 2;
    const ptrdiff_t c_stride = cur_stride / 2;

    const int32x4_t g0 = vdupq_n_s32(3571);
    const int32x4_t g1 = vdupq_n_s32(16004);
    const int32x4_t g2 = vdupq_n_s32(26386);
    const int64x2_t y_round = vdupq_n_s64((int64_t)1 << (bpc - 1));
    /* Runtime variable right shift via `vshlq_s64(v, -bpc)`; arithmetic
     * on signed int64. */
    const int64x2_t bpc_neg = vdupq_n_s64(-(int64_t)bpc);

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        const uint16_t *pp[5];
        const uint16_t *cp[5];
        for (int k = 0; k < 5; k++) {
            const int r = mirror((int)i - 2 + k, (int)h);
            pp[k] = prev + (ptrdiff_t)r * p_stride;
            cp[k] = cur + (ptrdiff_t)r * c_stride;
        }

        unsigned j;
        int32x4_t nz_acc = vdupq_n_s32(0);
        for (j = 0; j + 4 <= w; j += 4) {
            nz_acc = vorrq_s32(
                nz_acc, y_conv_row_step16_neon(pp, cp, j, g0, g1, g2, y_round, bpc_neg, y_row));
        }

        int32_t nz_tail = 0;
        for (; j < w; j++) {
            nz_tail |= y_conv_col_scalar16(pp, cp, j, bpc, y_row);
        }

        if (!(neon_any_nonzero_s32(nz_acc) | (uint32_t)nz_tail))
            continue;

        sad += x_conv_row_sad_neon(y_row, w);
    }

    return sad;
}

/* One 4-lane SIMD step of phase-1 for the 8-bit pipeline. 8-bit diff fits
 * in int16 (range [-255, 255]); filter × diff fits in int32; sum of 5 fits
 * in int32. Everything stays int32. */
static inline int32x4_t y_conv_row_step8_neon(const uint8_t *p[5], const uint8_t *c[5], unsigned j,
                                              int32x4_t g0, int32x4_t g1, int32x4_t g2,
                                              int32x4_t y_round_i32, int32_t *y_row)
{
    const int32x4_t d0 =
        vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(p[0] + j))))),
                  vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(c[0] + j))))));
    const int32x4_t d1 =
        vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(p[1] + j))))),
                  vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(c[1] + j))))));
    const int32x4_t d2 =
        vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(p[2] + j))))),
                  vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(c[2] + j))))));
    const int32x4_t d3 =
        vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(p[3] + j))))),
                  vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(c[3] + j))))));
    const int32x4_t d4 =
        vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(p[4] + j))))),
                  vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vld1_u8(c[4] + j))))));

    int32x4_t acc = vmulq_s32(d0, g0);
    acc = vaddq_s32(acc, vmulq_s32(d1, g1));
    acc = vaddq_s32(acc, vmulq_s32(d2, g2));
    acc = vaddq_s32(acc, vmulq_s32(d3, g1));
    acc = vaddq_s32(acc, vmulq_s32(d4, g0));

    /* Arithmetic right shift by 8, matching scalar `(accum + 128) >> 8`. */
    const int32x4_t row = vshrq_n_s32(vaddq_s32(acc, y_round_i32), 8);
    vst1q_s32(y_row + j, row);
    return row;
}

/* Scalar phase-1 step for column `j` of the 8-bit pipeline. */
static inline int32_t y_conv_col_scalar8(const uint8_t *p[5], const uint8_t *c[5], unsigned j,
                                         int32_t *y_row)
{
    int32_t accum = 0;
    for (int k = 0; k < 5; k++) {
        const int32_t diff = p[k][j] - c[k][j];
        accum += (int32_t)filter[k] * diff;
    }
    const int32_t v = (accum + (1 << 7)) >> 8;
    y_row[j] = v;
    return v;
}

uint64_t motion_score_pipeline_8_neon(const uint8_t *prev, ptrdiff_t prev_stride,
                                      const uint8_t *cur, ptrdiff_t cur_stride, int32_t *y_row,
                                      unsigned w, unsigned h, unsigned bpc)
{
    (void)bpc;

    const int32x4_t g0 = vdupq_n_s32(3571);
    const int32x4_t g1 = vdupq_n_s32(16004);
    const int32x4_t g2 = vdupq_n_s32(26386);
    const int32x4_t y_round_i32 = vdupq_n_s32(1 << 7);

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        const uint8_t *p[5];
        const uint8_t *c[5];
        for (int k = 0; k < 5; k++) {
            const int r = mirror((int)i - 2 + k, (int)h);
            p[k] = prev + (ptrdiff_t)r * prev_stride;
            c[k] = cur + (ptrdiff_t)r * cur_stride;
        }

        unsigned j;
        int32x4_t nz_acc = vdupq_n_s32(0);
        for (j = 0; j + 4 <= w; j += 4) {
            nz_acc =
                vorrq_s32(nz_acc, y_conv_row_step8_neon(p, c, j, g0, g1, g2, y_round_i32, y_row));
        }

        int32_t nz_tail = 0;
        for (; j < w; j++) {
            nz_tail |= y_conv_col_scalar8(p, c, j, y_row);
        }

        if (!(neon_any_nonzero_s32(nz_acc) | (uint32_t)nz_tail))
            continue;

        sad += x_conv_row_sad_neon(y_row, w);
    }

    return sad;
}
