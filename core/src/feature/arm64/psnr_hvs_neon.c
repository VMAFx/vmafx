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
 * NEON port of calc_psnrhvs / od_bin_fdct8x8.
 *
 * Sister TU to libvmaf/src/feature/x86/psnr_hvs_avx2.c (ADR-0159).
 * The AVX2 TU vectorises 8 columns per `__m256i` register;
 * aarch64 NEON's `int32x4_t` holds 4 lanes, so each 8-column row
 * is split into a `lo` register (cols 0-3) and a `hi` register
 * (cols 4-7). Every butterfly is therefore invoked twice per DCT
 * pass — once for the low half, once for the high half — and the
 * 8x8 transpose decomposes into four 4x4 transposes plus a
 * lo/hi block regroup across the row axis.
 *
 * Bit-exactness contract (ADR-0159 mirror):
 *
 *   Under FLT_EVAL_METHOD == 0 this TU produces byte-for-byte
 *   identical int32 DCT coefficients and byte-for-byte identical
 *   `double` return values as the scalar reference in
 *   libvmaf/src/feature/third_party/xiph/psnr_hvs.c — and
 *   therefore also as the AVX2 TU.
 *
 *   The integer forward-DCT butterfly network is pure signed
 *   int32 arithmetic (adds, subs, `mullo`, unbiased arithmetic
 *   right-shifts). Applying the same 30 ops to 4 independent
 *   columns packed in a single `int32x4_t` lane-wise yields, by
 *   the commutativity of SIMD lanes, the same result as running
 *   the scalar butterfly on each column individually. Running
 *   the same butterfly on the high 4 lanes immediately after
 *   produces the remaining 4 columns.
 *
 *   All float and double accumulations (means, variances, mask,
 *   error) remain scalar, so their left-to-right summation order
 *   and intermediate type promotions match the scalar reference
 *   trivially. This matches ADR-0159's float-accumulator rule.
 *   In particular, `accumulate_error()` threads the cross-block
 *   `ret` accumulator by pointer so each of the 64 per-coefficient
 *   contributions hits the outer `ret` directly, preserving the
 *   exact scalar summation tree (see rebase-notes.md §0052
 *   invariant #3).
 */

#include <arm_neon.h>
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "psnr_hvs_neon.h"

/* Disable compiler-emitted FMA contraction. The scalar reference is
 * compiled without FMA, so `a + b * c` always executes as two
 * IEEE-754-rounded ops. This TU is compiled with NEON enabled (aarch64
 * baseline); `calc_psnrhvs_neon`'s scalar float accumulators must stay
 * uncontracted to preserve byte-for-byte parity with scalar. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#pragma STDC FP_CONTRACT OFF
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

typedef int32_t od_coeff;

/* Mirror of scalar OD_UNBIASED_RSHIFT32 / OD_DCT_RSHIFT across 4 lanes.
 *
 *   Scalar: `(int32_t)(((uint32_t)a >> (32 - b)) + a) >> b` —
 *   unbiased (round-to-nearest-even-ish) arithmetic right shift.
 *   Bit-identical to scalar:
 *
 *     - `vreinterpretq_u32_s32` + `vshrq_n_u32` (logical shift
 *       treats lanes as uint32) matches the `(uint32_t)` cast.
 *     - `vaddq_s32` wraps on overflow (undefined in C for signed,
 *       but scalar also casts through uint32) — matches.
 *     - `vshlq_s32(_, vnegq_s32(vdupq_n_s32(b)))` is a signed
 *       arithmetic right shift by `b` — matches scalar's final
 *       `>> b` on int32. (NEON has no immediate arith right
 *       shift by a variable; this `vshlq_s32 + neg` is the
 *       canonical idiom.)
 */
static inline int32x4_t od_dct_rshift_neon(int32x4_t v, int b)
{
    const uint32x4_t vu = vreinterpretq_u32_s32(v);
    const int32x4_t bias = vreinterpretq_s32_u32(vshlq_u32(vu, vdupq_n_s32(-(32 - b))));
    return vshlq_s32(vaddq_s32(v, bias), vdupq_n_s32(-b));
}

/* Mirror of the scalar `(x * k + round) >> shift` pattern.
 *
 *   `vmulq_s32` returns the low 32 bits of the signed 32x32
 *   product — same truncation semantics as scalar multiplying
 *   two int32s whose product fits in int32 (which it does for
 *   every DCT butterfly multiplier with 12-bit input). `vaddq_s32`
 *   + `vshlq_s32(_, -shift)` match scalar's `+ round` then signed
 *   `>> shift`.
 */
static inline int32x4_t od_mulrshift_neon(int32x4_t x, int32_t k, int32_t round, int shift)
{
    const int32x4_t kv = vdupq_n_s32(k);
    const int32x4_t rv = vdupq_n_s32(round);
    const int32x4_t prod = vmulq_s32(x, kv);
    return vshlq_s32(vaddq_s32(prod, rv), vdupq_n_s32(-shift));
}

/*
 * Apply the scalar `od_bin_fdct8` butterfly network to 4 columns
 * in parallel. Signature mirrors the AVX2 variant (ADR-0159) with
 * `int32x4_t` substituted for `__m256i`. Per ADR-0141 the
 * 30-butterfly network is kept together — splitting it would
 * break the one-to-one scalar diff that the bit-exactness audit
 * depends on.
 */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — ADR-0141 §2 /
// ADR-0159 / ADR-0278: 30-butterfly network kept whole for the scalar bit-exact diff.
static inline void od_bin_fdct8_simd(int32x4_t in0, int32x4_t in1, int32x4_t in2, int32x4_t in3,
                                     int32x4_t in4, int32x4_t in5, int32x4_t in6, int32x4_t in7,
                                     int32x4_t *out0, int32x4_t *out1, int32x4_t *out2,
                                     int32x4_t *out3, int32x4_t *out4, int32x4_t *out5,
                                     int32x4_t *out6, int32x4_t *out7)
{
    /* Initial permutation: mirror scalar `od_bin_fdct8` reads. */
    int32x4_t t0 = in0;
    int32x4_t t4 = in1;
    int32x4_t t2 = in2;
    int32x4_t t6 = in3;
    int32x4_t t7 = in4;
    int32x4_t t3 = in5;
    int32x4_t t5 = in6;
    int32x4_t t1 = in7;
    int32x4_t t1h;
    int32x4_t t4h;
    int32x4_t t6h;

    /* +1/-1 butterflies. */
    t1 = vsubq_s32(t0, t1);
    t1h = od_dct_rshift_neon(t1, 1);
    t0 = vsubq_s32(t0, t1h);
    t4 = vaddq_s32(t4, t5);
    t4h = od_dct_rshift_neon(t4, 1);
    t5 = vsubq_s32(t5, t4h);
    t3 = vsubq_s32(t2, t3);
    t2 = vsubq_s32(t2, od_dct_rshift_neon(t3, 1));
    t6 = vaddq_s32(t6, t7);
    t6h = od_dct_rshift_neon(t6, 1);
    t7 = vsubq_s32(t6h, t7);

    /* Embedded 4-point type-II DCT. */
    t0 = vaddq_s32(t0, t6h);
    t6 = vsubq_s32(t0, t6);
    t2 = vsubq_s32(t4h, t2);
    t4 = vsubq_s32(t2, t4);

    /* Embedded 2-point type-II DCT. */
    /* 13573/32768 ~= sqrt(2) - 1 */
    t0 = vsubq_s32(t0, od_mulrshift_neon(t4, 13573, 16384, 15));
    /* 11585/16384 ~= sqrt(1/2) */
    t4 = vaddq_s32(t4, od_mulrshift_neon(t0, 11585, 8192, 14));
    /* 13573/32768 */
    t0 = vsubq_s32(t0, od_mulrshift_neon(t4, 13573, 16384, 15));

    /* Embedded 2-point type-IV DST. */
    /* 21895/32768 ~= (1 - cos(3pi/8))/sin(3pi/8) */
    t6 = vsubq_s32(t6, od_mulrshift_neon(t2, 21895, 16384, 15));
    /* 15137/16384 ~= sin(3pi/8) */
    t2 = vaddq_s32(t2, od_mulrshift_neon(t6, 15137, 8192, 14));
    /* 21895/32768 */
    t6 = vsubq_s32(t6, od_mulrshift_neon(t2, 21895, 16384, 15));

    /* Embedded 4-point type-IV DST. */
    /* 19195/32768 ~= 2 - sqrt(2) */
    t3 = vaddq_s32(t3, od_mulrshift_neon(t5, 19195, 16384, 15));
    /* 11585/16384 ~= sqrt(1/2) */
    t5 = vaddq_s32(t5, od_mulrshift_neon(t3, 11585, 8192, 14));
    /* 7489/8192 ~= sqrt(2) - 1/2 */
    t3 = vsubq_s32(t3, od_mulrshift_neon(t5, 7489, 4096, 13));
    t7 = vsubq_s32(od_dct_rshift_neon(t5, 1), t7);
    t5 = vsubq_s32(t5, t7);
    t3 = vsubq_s32(t1h, t3);
    t1 = vsubq_s32(t1, t3);
    /* 3227/32768 */
    t7 = vaddq_s32(t7, od_mulrshift_neon(t1, 3227, 16384, 15));
    /* 6393/32768 */
    t1 = vsubq_s32(t1, od_mulrshift_neon(t7, 6393, 16384, 15));
    /* 3227/32768 */
    t7 = vaddq_s32(t7, od_mulrshift_neon(t1, 3227, 16384, 15));
    /* 2485/8192 */
    t5 = vaddq_s32(t5, od_mulrshift_neon(t3, 2485, 4096, 13));
    /* 18205/32768 ~= sin(3pi/16) */
    t3 = vsubq_s32(t3, od_mulrshift_neon(t5, 18205, 16384, 15));
    /* 2485/8192 */
    t5 = vaddq_s32(t5, od_mulrshift_neon(t3, 2485, 4096, 13));

    *out0 = t0;
    *out1 = t1;
    *out2 = t2;
    *out3 = t3;
    *out4 = t4;
    *out5 = t5;
    *out6 = t6;
    *out7 = t7;
}

/* 4x4 int32 transpose using aarch64 NEON trn1/trn2 idiom.
 * (`vtrnq_s64` is armv7-only; aarch64 exposes separate
 * `vtrn1q_s64` / `vtrn2q_s64` intrinsics.) */
static inline void transpose4x4_s32(int32x4_t *a, int32x4_t *b, int32x4_t *c, int32x4_t *d)
{
    const int32x4_t ab0 = vtrn1q_s32(*a, *b);
    const int32x4_t ab1 = vtrn2q_s32(*a, *b);
    const int32x4_t cd0 = vtrn1q_s32(*c, *d);
    const int32x4_t cd1 = vtrn2q_s32(*c, *d);
    const int64x2_t ab0_64 = vreinterpretq_s64_s32(ab0);
    const int64x2_t ab1_64 = vreinterpretq_s64_s32(ab1);
    const int64x2_t cd0_64 = vreinterpretq_s64_s32(cd0);
    const int64x2_t cd1_64 = vreinterpretq_s64_s32(cd1);
    *a = vreinterpretq_s32_s64(vtrn1q_s64(ab0_64, cd0_64));
    *c = vreinterpretq_s32_s64(vtrn2q_s64(ab0_64, cd0_64));
    *b = vreinterpretq_s32_s64(vtrn1q_s64(ab1_64, cd1_64));
    *d = vreinterpretq_s32_s64(vtrn2q_s64(ab1_64, cd1_64));
}

/*
 * Transpose an 8x8 int32 matrix held as 16 int32x4_t registers
 * (lo/hi per row). Decomposes into four 4x4 transposes of the
 * quadrant blocks plus a lo/hi regroup across the row axis: the
 * top-right quadrant ends up as the lo half of the transposed
 * bottom rows, and the bottom-left as the hi half of the top rows.
 */
static inline void transpose8x8_s32(int32x4_t *r0_lo, int32x4_t *r0_hi, int32x4_t *r1_lo,
                                    int32x4_t *r1_hi, int32x4_t *r2_lo, int32x4_t *r2_hi,
                                    int32x4_t *r3_lo, int32x4_t *r3_hi, int32x4_t *r4_lo,
                                    int32x4_t *r4_hi, int32x4_t *r5_lo, int32x4_t *r5_hi,
                                    int32x4_t *r6_lo, int32x4_t *r6_hi, int32x4_t *r7_lo,
                                    int32x4_t *r7_hi)
{
    /* Transpose the four 4x4 quadrants in place. */
    transpose4x4_s32(r0_lo, r1_lo, r2_lo, r3_lo); /* top-left */
    transpose4x4_s32(r0_hi, r1_hi, r2_hi, r3_hi); /* top-right */
    transpose4x4_s32(r4_lo, r5_lo, r6_lo, r7_lo); /* bottom-left */
    transpose4x4_s32(r4_hi, r5_hi, r6_hi, r7_hi); /* bottom-right */

    /* Swap top-right block with bottom-left block: after the 4x4
     * transposes, original top-right (r0_hi..r3_hi) holds cols 0-3
     * of transposed rows 4-7, and original bottom-left (r4_lo..r7_lo)
     * holds cols 4-7 of transposed rows 0-3. */
    int32x4_t tmp;
    tmp = *r0_hi;
    *r0_hi = *r4_lo;
    *r4_lo = tmp;
    tmp = *r1_hi;
    *r1_hi = *r5_lo;
    *r5_lo = tmp;
    tmp = *r2_hi;
    *r2_hi = *r6_lo;
    *r6_lo = tmp;
    tmp = *r3_hi;
    *r3_hi = *r7_lo;
    *r7_lo = tmp;
}

/* Load 8 rows of a 8x8 int32 block into 16 int32x4_t (lo + hi per row). */
static inline void load_8x8_block_s32(const int32_t *x, ptrdiff_t xs, int32x4_t *r_lo,
                                      int32x4_t *r_hi)
{
    for (int k = 0; k < 8; k++) {
        r_lo[k] = vld1q_s32(x + k * xs);
        r_hi[k] = vld1q_s32(x + k * xs + 4);
    }
}

/* Store 16 int32x4_t (lo + hi per row) into 8 rows of a int32 block. */
static inline void store_8x8_block_s32(int32_t *y, ptrdiff_t ys, const int32x4_t *r_lo,
                                       const int32x4_t *r_hi)
{
    for (int k = 0; k < 8; k++) {
        vst1q_s32(y + k * ys, r_lo[k]);
        vst1q_s32(y + k * ys + 4, r_hi[k]);
    }
}

void od_bin_fdct8x8_neon(int32_t *y, int32_t ystride, const int32_t *x, int32_t xstride)
{
    assert(y != NULL);
    assert(x != NULL);
    assert(xstride >= 8);
    assert(ystride >= 8);
    const ptrdiff_t xs = (ptrdiff_t)xstride;
    const ptrdiff_t ys = (ptrdiff_t)ystride;

    int32x4_t r_lo[8];
    int32x4_t r_hi[8];
    load_8x8_block_s32(x, xs, r_lo, r_hi);

    /* First pass: butterfly on cols 0-3 then cols 4-7. Mirrors the
     * AVX2 single 8-lane butterfly invocation split in half. */
    od_bin_fdct8_simd(r_lo[0], r_lo[1], r_lo[2], r_lo[3], r_lo[4], r_lo[5], r_lo[6], r_lo[7],
                      &r_lo[0], &r_lo[1], &r_lo[2], &r_lo[3], &r_lo[4], &r_lo[5], &r_lo[6],
                      &r_lo[7]);
    od_bin_fdct8_simd(r_hi[0], r_hi[1], r_hi[2], r_hi[3], r_hi[4], r_hi[5], r_hi[6], r_hi[7],
                      &r_hi[0], &r_hi[1], &r_hi[2], &r_hi[3], &r_hi[4], &r_hi[5], &r_hi[6],
                      &r_hi[7]);

    transpose8x8_s32(&r_lo[0], &r_hi[0], &r_lo[1], &r_hi[1], &r_lo[2], &r_hi[2], &r_lo[3], &r_hi[3],
                     &r_lo[4], &r_hi[4], &r_lo[5], &r_hi[5], &r_lo[6], &r_hi[6], &r_lo[7],
                     &r_hi[7]);

    /* Second pass. */
    od_bin_fdct8_simd(r_lo[0], r_lo[1], r_lo[2], r_lo[3], r_lo[4], r_lo[5], r_lo[6], r_lo[7],
                      &r_lo[0], &r_lo[1], &r_lo[2], &r_lo[3], &r_lo[4], &r_lo[5], &r_lo[6],
                      &r_lo[7]);
    od_bin_fdct8_simd(r_hi[0], r_hi[1], r_hi[2], r_hi[3], r_hi[4], r_hi[5], r_hi[6], r_hi[7],
                      &r_hi[0], &r_hi[1], &r_hi[2], &r_hi[3], &r_hi[4], &r_hi[5], &r_hi[6],
                      &r_hi[7]);

    transpose8x8_s32(&r_lo[0], &r_hi[0], &r_lo[1], &r_hi[1], &r_lo[2], &r_hi[2], &r_lo[3], &r_hi[3],
                     &r_lo[4], &r_hi[4], &r_lo[5], &r_hi[5], &r_lo[6], &r_hi[6], &r_lo[7],
                     &r_hi[7]);

    store_8x8_block_s32(y, ys, r_lo, r_hi);
}

/*
 * Per-block scratch state threaded through the calc_psnrhvs_neon
 * helpers. Mirrors the AVX2 TU one-for-one — the non-SIMD scalar
 * plumbing is bit-exact to scalar by construction (same C code,
 * same summation order).
 */
typedef struct {
    od_coeff dct_s[8 * 8];
    od_coeff dct_d[8 * 8];
    float s_means[4];
    float d_means[4];
    float s_vars[4];
    float d_vars[4];
    float s_gmean;
    float d_gmean;
    float s_gvar;
    float d_gvar;
    float s_mask;
    float d_mask;
} psnr_hvs_block;

/* Load one 8x8 block, compute global + quadrant means in one pass. */
static void load_block_and_means(psnr_hvs_block *b, const unsigned char *src, int systride,
                                 const unsigned char *dst, int dystride, int depth, int x, int y)
{
    b->s_gmean = 0;
    b->d_gmean = 0;
    for (int i = 0; i < 4; i++) {
        b->s_means[i] = 0;
        b->d_means[i] = 0;
        b->s_vars[i] = 0;
        b->d_vars[i] = 0;
    }
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const int sub = ((i & 12) >> 2) + ((j & 12) >> 1);
            if (depth > 8) {
                b->dct_s[i * 8 + j] = src[(y + i) * systride + (j + x) * 2] +
                                      (src[(y + i) * systride + (j + x) * 2 + 1] << 8);
                b->dct_d[i * 8 + j] = dst[(y + i) * dystride + (j + x) * 2] +
                                      (dst[(y + i) * dystride + (j + x) * 2 + 1] << 8);
            } else {
                b->dct_s[i * 8 + j] = src[(y + i) * systride + (j + x)];
                b->dct_d[i * 8 + j] = dst[(y + i) * dystride + (j + x)];
            }
            b->s_gmean += b->dct_s[i * 8 + j];
            b->d_gmean += b->dct_d[i * 8 + j];
            b->s_means[sub] += b->dct_s[i * 8 + j];
            b->d_means[sub] += b->dct_d[i * 8 + j];
        }
    }
    b->s_gmean /= 64.f;
    b->d_gmean /= 64.f;
    for (int i = 0; i < 4; i++) {
        b->s_means[i] /= 16.f;
    }
    for (int i = 0; i < 4; i++) {
        b->d_means[i] /= 16.f;
    }
}

/* Compute global + quadrant variances; fold quadrant-to-global ratio as the
 * scalar reference does at the end. */
static void compute_vars(psnr_hvs_block *b)
{
    b->s_gvar = 0;
    b->d_gvar = 0;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const int sub = ((i & 12) >> 2) + ((j & 12) >> 1);
            b->s_gvar += (b->dct_s[i * 8 + j] - b->s_gmean) * (b->dct_s[i * 8 + j] - b->s_gmean);
            b->d_gvar += (b->dct_d[i * 8 + j] - b->d_gmean) * (b->dct_d[i * 8 + j] - b->d_gmean);
            b->s_vars[sub] +=
                (b->dct_s[i * 8 + j] - b->s_means[sub]) * (b->dct_s[i * 8 + j] - b->s_means[sub]);
            b->d_vars[sub] +=
                (b->dct_d[i * 8 + j] - b->d_means[sub]) * (b->dct_d[i * 8 + j] - b->d_means[sub]);
        }
    }
    b->s_gvar *= 1 / 63.f * 64;
    b->d_gvar *= 1 / 63.f * 64;
    for (int i = 0; i < 4; i++) {
        b->s_vars[i] *= 1 / 15.f * 16;
    }
    for (int i = 0; i < 4; i++) {
        b->d_vars[i] *= 1 / 15.f * 16;
    }
    if (b->s_gvar > 0) {
        b->s_gvar = (b->s_vars[0] + b->s_vars[1] + b->s_vars[2] + b->s_vars[3]) / b->s_gvar;
    }
    if (b->d_gvar > 0) {
        b->d_gvar = (b->d_vars[0] + b->d_vars[1] + b->d_vars[2] + b->d_vars[3]) / b->d_gvar;
    }
}

/* DCT + AC-only mask accumulation; `sqrt` is double-precision to match
 * scalar. d_mask > s_mask fold mirrors the scalar reference. */
static void compute_masks(psnr_hvs_block *b, const float mask[8][8])
{
    od_bin_fdct8x8_neon(b->dct_s, 8, b->dct_s, 8);
    od_bin_fdct8x8_neon(b->dct_d, 8, b->dct_d, 8);
    b->s_mask = 0;
    b->d_mask = 0;
    for (int i = 0; i < 8; i++) {
        for (int j = (i == 0); j < 8; j++) {
            b->s_mask += b->dct_s[i * 8 + j] * b->dct_s[i * 8 + j] * mask[i][j];
        }
    }
    for (int i = 0; i < 8; i++) {
        for (int j = (i == 0); j < 8; j++) {
            b->d_mask += b->dct_d[i * 8 + j] * b->dct_d[i * 8 + j] * mask[i][j];
        }
    }
    /* ADR-0141: `sqrt` (double) matches the scalar reference's float->double
     * promotion before sqrt; switching to `sqrtf` would diverge from the
     * bit-exact contract. */
    // NOLINTNEXTLINE(performance-type-promotion-in-math-fn) — ADR-0141 / ADR-0159 / ADR-0278
    b->s_mask = sqrt(b->s_mask * b->s_gvar) / 32.f;
    // NOLINTNEXTLINE(performance-type-promotion-in-math-fn) ADR-0141 as above.
    b->d_mask = sqrt(b->d_mask * b->d_gvar) / 32.f;
    if (b->d_mask > b->s_mask) {
        b->s_mask = b->d_mask;
    }
}

/* Per-coefficient error accumulation; AC coefficients have the mask-
 * threshold subtraction applied, DC (i==j==0) is compared raw. Adds the
 * 64 per-coefficient contributions directly into `*ret`; increments
 * `*pixels` by 64.
 *
 * ADR-0159 bit-exactness: `*ret` is the cross-block running accumulator
 * (scalar's outer `ret`). Accumulating into a local float here and then
 * adding the per-block total to the caller would change the float
 * summation tree (IEEE-754 add is non-associative) and break byte-for-byte
 * parity with the scalar reference's inline accumulation at
 * third_party/xiph/psnr_hvs.c:355. */
static void accumulate_error(const psnr_hvs_block *b, const float mask[8][8], float csf[8][8],
                             float *ret, int *pixels)
{
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            float err = abs(b->dct_s[i * 8 + j] - b->dct_d[i * 8 + j]);
            if (i != 0 || j != 0) {
                err = err < b->s_mask / mask[i][j] ? 0 : err - b->s_mask / mask[i][j];
            }
            *ret += (err * csf[i][j]) * (err * csf[i][j]);
            (*pixels)++;
        }
    }
}

double calc_psnrhvs_neon(const unsigned char *src, int systride, const unsigned char *dst,
                         int dystride, double par, int depth, int w, int h, int step,
                         float csf[8][8])
{
    float mask[8][8];
    psnr_hvs_block b;
    float ret = 0;
    int pixels = 0;
    int32_t samplemax;
    (void)par;

    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            mask[x][y] = (csf[x][y] * 0.3885746225901003) * (csf[x][y] * 0.3885746225901003);
        }
    }

    for (int y = 0; y < h - 7; y += step) {
        for (int x = 0; x < w - 7; x += step) {
            load_block_and_means(&b, src, systride, dst, dystride, depth, x, y);
            compute_vars(&b);
            compute_masks(&b, mask);
            accumulate_error(&b, mask, csf, &ret, &pixels);
        }
    }
    ret /= pixels;
    samplemax = (1 << depth) - 1;
    ret /= samplemax * samplemax;
    return ret;
}
