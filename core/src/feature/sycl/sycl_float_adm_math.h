/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  The float ADM reference's per-sample arithmetic for SYCL kernels
 *  (adm_tools.c), operation for operation, without the fp64 type (ADR-0220).
 *  It is the SYCL counterpart of cuda/float_adm/float_adm_device.h
 *  (ADR-1420): the same functions in the same order, so the two can be read
 *  side by side.
 *
 *   - divs() is DIVS(): the IEEE fp32 quotient on both sides (ADR-1442). The
 *     device's `/` is correctly rounded under the flag line every SYCL
 *     feature TU is compiled with (ADR-1367).
 *   - angle_flag() and decouple_band() are adm_angle_flag_s() and
 *     adm_decouple_band_s(): the threshold is (cos^2 * |o|^2) * |t|^2 in that
 *     association, the clamp is the reference's two ternaries.
 *   - csf_flt() and thresh_band() are adm_csf_s() and adm_cm_thresh3x3_s().
 *     The other eight terms of a band's sum are fp32 adds in the reference's
 *     order, one sum per band.
 *   - den_term() and cm_term() are the terms adm_csf_den_scale_s() and
 *     adm_cm_s() accumulate. At adm_p_norm = 3 they are (x * x) * x in fp32
 *     and at 1 they are x. Any other adm_p_norm is powf() on both sides, and
 *     the device's pow is not glibc's: that option is close to the
 *     reference, not equal.
 *   - row_sum() is the reference's per-row accumulator, left to right in
 *     fp32.
 *   - decouple_sample(), terms_sample() and row_item() are what one work-item
 *     of the three kernels does. The extractor's kernels and the test's
 *     probe both call them, so the test replays the extractor's code.
 *
 *  Three expressions of the reference are fp64, because a constant or an
 *  argument in them is a `double`, and each is rounded to fp32 once:
 *
 *      rst * adm_enhn_gain_limit                    (the enhancement gain)
 *      FLOAT_ONE_BY_30 * fabsf(csf)                 (the filtered value)
 *      sum + FLOAT_ONE_BY_15 * fabsf(centre)        (the centre tap)
 *
 *  The first is an fp32 product whenever the limit is an fp32 value (the
 *  default 100 and the models' 1 are): the fp64 product is then exact, and
 *  its rounding is the fp32 product's. The other two are evaluated as exact
 *  fp32 pairs (sycl_exact_fp.h); a pair that lies next to an fp32 rounding
 *  boundary, and any limit that is not an fp32 value, replays the reference's
 *  fp64 operations in 64-bit integers (sycl_soft_double.h). So every result
 *  is the reference's by construction (ADR-1434).
 *
 *  Kernel code may use everything here except make_gain_limit(), which is
 *  host code and uses fp64. A translation unit that includes this header is
 *  compiled with contraction off, as every SYCL feature TU is (ADR-1367).
 */

#ifndef VMAF_FEATURE_SYCL_SYCL_FLOAT_ADM_MATH_H_
#define VMAF_FEATURE_SYCL_SYCL_FLOAT_ADM_MATH_H_

#include <sycl/sycl.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "sycl_exact_fp.h"
#include "sycl_soft_double.h"

namespace vmaf_sycl_fadm
{

using vmaf_sycl_exact::Ff;
using vmaf_sycl_exact::ff_add;
using vmaf_sycl_exact::quick_two_sum;
using vmaf_sycl_exact::two_prod;
using vmaf_sycl_soft::soft_add;
using vmaf_sycl_soft::soft_from_float_any;
using vmaf_sycl_soft::soft_less;
using vmaf_sycl_soft::soft_mul;
using vmaf_sycl_soft::soft_to_float_any;
using vmaf_sycl_soft::SoftDouble;

inline constexpr int kBands = 3;
inline constexpr unsigned kSlotDen = 0u; /* |csf(ref)|^p, the adm2 denominator */
inline constexpr unsigned kSlotCm = 3u;  /* masked restored signal, the adm2 numerator */
inline constexpr unsigned kSlotAim = 6u; /* masked additive signal, the AIM numerator */
inline constexpr unsigned kTermSlots = 9u;

/* The decouple's epsilon: `const float eps = 1e-30`. */
inline constexpr float kEps = (float)1e-30;

/* ------------------------------------------------------------------ */
/* fp64 constants and arguments as the kernels take them               */
/* ------------------------------------------------------------------ */

/* A positive fp64 value as a pair of floats (good to 2^-48) and exactly. */
struct DoubleValue {
    float hi;
    float lo;
    uint64_t mant; /* the value is mant * 2^exp, mant in [2^52, 2^53) */
    int32_t exp;
};

/* adm_tools.c's FLOAT_ONE_BY_30 (0.0333333351) and FLOAT_ONE_BY_15
 * (0.0666666701): double literals, so not the fp32 values of those names. */
inline constexpr DoubleValue kOneBy30 = {
    .hi = 0x1.111112p-5f, .lo = 0x1.f00fep-36f, .mant = 0x111111203e01fcULL, .exp = -57};
inline constexpr DoubleValue kOneBy15 = {
    .hi = 0x1.111112p-4f, .lo = -0x1.7f8c18p-35f, .mant = 0x1111111fd00e7dULL, .exp = -56};

/* adm_enhn_gain_limit. */
struct GainLimit {
    float value;      /* (float)limit */
    int32_t is_float; /* the limit is an fp32 value: rst * limit is exact in fp64 */
    uint64_t mant;    /* the limit itself, for the others */
    int32_t exp;
};

/* ------------------------------------------------------------------ */
/* The two fp64 products with a constant                               */
/* ------------------------------------------------------------------ */

/* True when the pair does not decide its rounding to fp32: it is within
 * 2^-18 of an fp32 step of the point where the rounding changes, or too small
 * for the pair to be exact. The pair is good to about 2^-24 of a step (the
 * constant's low word is an fp32 value, and one product with it is rounded),
 * so the margin is a factor of 64. `sum` is positive and normalised. */
inline bool undecided(Ff sum)
{
    if (!(sum.hi >= vmaf_sycl_exact::kFastLow) || !sycl::isfinite(sum.hi)) {
        return true;
    }
    const auto bits = sycl::bit_cast<uint32_t>(sum.hi);
    const uint32_t exponent = bits & 0x7F800000u;
    const float unit = sycl::bit_cast<float>(exponent - (uint32_t{23} << 23));
    /* Below a power of two the spacing halves. */
    const bool lower_binade = (bits & 0x007FFFFFu) == 0u && sum.lo < 0.0f;
    const float half = lower_binade ? 0.25f * unit : 0.5f * unit;
    return sycl::fabs(sum.lo) >= half - unit * 0x1p-18f;
}

/* constant * a as a pair, for a positive normal a. */
inline Ff scaled_pair(float a, const DoubleValue &constant)
{
    const Ff product = two_prod(a, constant.hi);
    const float cross = a * constant.lo;
    return quick_two_sum(product.hi, product.lo + cross);
}

/* fl64(constant * a), exactly. */
inline SoftDouble scaled_exact(float a, const DoubleValue &constant)
{
    return soft_mul(soft_from_float_any(a), SoftDouble{.mant = constant.mant, .exp = constant.exp});
}

/* (float)(constant * (double)a) by replaying the fp64 product, for a positive
 * finite a. */
inline float times_constant_replayed(float a, const DoubleValue &constant)
{
    return soft_to_float_any(scaled_exact(a, constant));
}

/* (float)((double)sum + constant * (double)a) by replaying the fp64 product
 * and sum, for positive finite operands. */
inline float add_scaled_replayed(float sum, float a, const DoubleValue &constant)
{
    return soft_to_float_any(soft_add(soft_from_float_any(sum), scaled_exact(a, constant)));
}

/* (float)(constant * (double)a) for a >= 0. */
inline float times_constant(float a, const DoubleValue &constant)
{
    if (a == 0.0f) {
        return 0.0f;
    }
    if (!sycl::isfinite(a)) {
        return a;
    }
    const Ff pair = scaled_pair(a, constant);
    if (!undecided(pair)) {
        return pair.hi;
    }
    return times_constant_replayed(a, constant);
}

/* (float)((double)sum + constant * (double)a) for sum >= 0 and a >= 0. */
inline float add_scaled(float sum, float a, const DoubleValue &constant)
{
    if (a == 0.0f) {
        return sum; /* the product is +0 and the fp64 sum is sum itself */
    }
    if (sum == 0.0f) {
        return times_constant(a, constant);
    }
    if (!sycl::isfinite(a) || !sycl::isfinite(sum)) {
        return sum + a;
    }
    const Ff pair = ff_add(Ff{.hi = sum, .lo = 0.0f}, scaled_pair(a, constant));
    const bool small = !(a >= vmaf_sycl_exact::kFastLow) || !(sum >= vmaf_sycl_exact::kFastLow);
    if (!small && !undecided(pair)) {
        return pair.hi;
    }
    return add_scaled_replayed(sum, a, constant);
}

/* ------------------------------------------------------------------ */
/* The reference's arithmetic                                          */
/* ------------------------------------------------------------------ */

/* DIVS(n, d): the IEEE fp32 quotient (ADR-1442). */
inline float divs(float n, float d)
{
    return n / d;
}

/* adm_angle_flag_s() with ADM_OPT_AVOID_ATAN: the angle between (oh, ov) and
 * (th, tv) is below one degree. */
inline bool angle_flag(float oh, float ov, float th, float tv, float cos_1deg_sq)
{
    const float oh_th = oh * th;
    const float ov_tv = ov * tv;
    const float ot_dp = oh_th + ov_tv;
    const float oh_sq = oh * oh;
    const float ov_sq = ov * ov;
    const float o_mag_sq = oh_sq + ov_sq;
    const float th_sq = th * th;
    const float tv_sq = tv * tv;
    const float t_mag_sq = th_sq + tv_sq;
    const float lhs = ot_dp * ot_dp;
    const float scaled = cos_1deg_sq * o_mag_sq;
    const float rhs = scaled * t_mag_sq;
    return (ot_dp >= 0.0f) && (lhs >= rhs);
}

/* (float)MIN(rst * limit, t) for rst > 0, (float)MAX(rst * limit, t) for
 * rst < 0, the product and the comparison in fp64. */
inline float gain_limited(float rst, float t, const GainLimit &limit)
{
    if (limit.is_float != 0) {
        /* The fp64 product is exact, so its rounding is this fp32 product's,
         * and comparing the rounded product with t selects as the exact
         * comparison does. */
        const float gained = rst * limit.value;
        if (rst > 0.0f) {
            return gained < t ? gained : t;
        }
        return gained > t ? gained : t;
    }
    /* The product keeps rst's sign. It is selected when it is closer to zero
     * than t and t has its sign; otherwise t is. */
    const bool same_sign = (rst > 0.0f) == (t > 0.0f) && t != 0.0f;
    if (!same_sign || !sycl::isfinite(rst) || !sycl::isfinite(t)) {
        const float gained = rst * limit.value;
        if (rst > 0.0f) {
            return gained < t ? gained : t;
        }
        return gained > t ? gained : t;
    }
    const SoftDouble gained = soft_mul(soft_from_float_any(sycl::fabs(rst)),
                                       SoftDouble{.mant = limit.mant, .exp = limit.exp});
    if (!soft_less(gained, soft_from_float_any(sycl::fabs(t)))) {
        return t;
    }
    const float magnitude = soft_to_float_any(gained);
    return rst > 0.0f ? magnitude : -magnitude;
}

/* adm_decouple_band_s(): the restored signal of one band. */
inline float decouple_band(const GainLimit &limit, float o, float t, bool flag)
{
    const float denominator = o + kEps;
    float k = divs(t, denominator);
    k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
    float rst = k * o;
    if (flag && (rst > 0.0f)) {
        rst = gain_limited(rst, t, limit);
    }
    if (flag && (rst < 0.0f)) {
        rst = gain_limited(rst, t, limit);
    }
    return rst;
}

/* adm_csf_s()'s filtered value: FLOAT_ONE_BY_30 * fabsf(csf). */
inline float csf_flt(float csf)
{
    return times_constant(sycl::fabs(csf), kOneBy30);
}

/* What the decouple kernel stores for one band sample: the CSF-weighted
 * additive (a = t - rst) and restored (r = rst) signals, each with its
 * filtered magnitude. */
struct CsfSample {
    float csf_a;
    float csf_fa;
    float csf_r;
    float csf_fr;
};

/* adm_decouple_s() and both adm_csf_s() calls of compute_adm() for one band
 * of one sample. */
inline CsfSample decouple_csf(const GainLimit &limit, float rfactor, float o, float t, bool flag)
{
    const float rst = decouple_band(limit, o, t, flag);
    const float add = t - rst;
    const float csf_a = rfactor * add;
    const float csf_r = rfactor * rst;
    return {.csf_a = csf_a, .csf_fa = csf_flt(csf_a), .csf_r = csf_r, .csf_fr = csf_flt(csf_r)};
}

/* The eight filtered neighbours of one band in row order (above-left, above,
 * above-right, left, right, below-left, below, below-right). */
struct Neighbours {
    float above_left;
    float above;
    float above_right;
    float left;
    float right;
    float below_left;
    float below;
    float below_right;
};

/* One band of adm_cm_thresh3x3_s(): the neighbours and the unfiltered centre,
 * which is added fifth, as an fp64 addend. */
inline float thresh_band(const Neighbours &n, float centre)
{
    float sum = 0.0f;
    sum += n.above_left;
    sum += n.above;
    sum += n.above_right;
    sum += n.left;
    sum = add_scaled(sum, sycl::fabs(centre), kOneBy15);
    sum += n.right;
    sum += n.below_left;
    sum += n.below;
    sum += n.below_right;
    return sum;
}

/* adm_cm_thresh3x3_s()'s neighbour indices: the sample before the first one
 * mirrors to index 1, the sample past the last one clamps to the last index.
 * A one-sample band has no index 1; the reference reads past the band there
 * and the device reads the only sample it has. */
inline int before(int i, int n)
{
    if (i != 0) {
        return i - 1;
    }
    return (n > 1) ? 1 : 0;
}

inline int after(int i, int n)
{
    return (i == n - 1) ? n - 1 : i + 1;
}

/* x^adm_p_norm as the reductions take it, for x >= 0. At 3 the reference's
 * _p3 routines multiply; at 1 its powf(x, 1) is x. Any other exponent is
 * powf() on both sides, and the device's is not the host's. */
inline float pnorm(float x, bool is_cube, float p_norm)
{
    if (is_cube) {
        const float square = x * x;
        return square * x;
    }
    if (p_norm == 1.0f) {
        return x;
    }
    return sycl::pow(x, p_norm);
}

/* The term adm_csf_den_scale_s() accumulates for one band sample. */
inline float den_term(float rfactor, float src, bool is_cube, float p_norm)
{
    const float weighted = rfactor * src;
    return pnorm(sycl::fabs(weighted), is_cube, p_norm);
}

/* The term adm_cm_s() accumulates for one band sample: `csf` is the sample
 * times its CSF weight, `thr` the masking threshold. */
inline float cm_term(float csf, float thr, bool is_cube, float p_norm)
{
    float x = sycl::fabs(csf) - thr;
    x = x < 0.0f ? 0.0f : x;
    return pnorm(x, is_cube, p_norm);
}

/* One row of one slot: `count` terms `stride` floats apart, left to right in
 * one fp32 accumulator. The order is the result; do not split, stride or
 * reduce this loop. */
inline float row_sum(const float *terms, size_t stride, unsigned count)
{
    float inner = 0.0f;
    for (unsigned x = 0u; x < count; x++) {
        inner += terms[(size_t)x * stride];
    }
    return inner;
}

/* ------------------------------------------------------------------ */
/* One scale's buffers, and what each kernel does for one work-item    */
/* ------------------------------------------------------------------ */

/* The DWT bands of one scale and the four CSF buffers derived from them. A
 * band buffer holds its sub-bands back to back, `buf_stride` floats per row
 * and `half_h` rows per sub-band: (a, h, v, d) for the DWT bands, (h, v, d)
 * for the CSF buffers. The three CSF weights are named scalars: selecting one
 * by a constant band keeps them out of private memory (ADR-1395). */
struct Bands {
    const float *ref_band;
    const float *dis_band;
    float *csf_a;
    float *csf_fa;
    float *csf_r;
    float *csf_fr;
    int half_w;
    int half_h;
    int buf_stride;
    float rfactor_h;
    float rfactor_v;
    float rfactor_d;
};

/* Sample (x, y) of sub-band `band` of a band buffer. */
inline size_t band_index(const Bands &bd, int band, int y, int x)
{
    return ((size_t)band * (size_t)bd.half_h + (size_t)y) * (size_t)bd.buf_stride + (size_t)x;
}

/* The decouple kernel's arguments. */
struct DecoupleArgs {
    Bands bands;
    GainLimit limit;
    float cos_1deg_sq; /* adm_decouple_cos_1deg_sq_s() */
};

/* adm_decouple_s() and the two adm_csf_s() calls for sub-band `band` (0 = h,
 * 1 = v, 2 = d, a constant at every call) of one sample. */
inline void store_csf(const DecoupleArgs &a, int band, float rfactor, int y, int x, bool flag)
{
    const Bands &bd = a.bands;
    const size_t source = band_index(bd, band + 1, y, x);
    const CsfSample c =
        decouple_csf(a.limit, rfactor, bd.ref_band[source], bd.dis_band[source], flag);
    const size_t index = band_index(bd, band, y, x);
    bd.csf_a[index] = c.csf_a;
    bd.csf_fa[index] = c.csf_fa;
    bd.csf_r[index] = c.csf_r;
    bd.csf_fr[index] = c.csf_fr;
}

/* The decouple kernel for sample (x, y) of the band: the angle test of the
 * (h, v) vectors, then the three sub-bands. */
inline void decouple_sample(const DecoupleArgs &a, int y, int x)
{
    const Bands &bd = a.bands;
    const size_t index_h = band_index(bd, 1, y, x);
    const size_t index_v = band_index(bd, 2, y, x);
    const bool flag = angle_flag(bd.ref_band[index_h], bd.ref_band[index_v], bd.dis_band[index_h],
                                 bd.dis_band[index_v], a.cos_1deg_sq);
    store_csf(a, 0, bd.rfactor_h, y, x, flag);
    store_csf(a, 1, bd.rfactor_v, y, x, flag);
    store_csf(a, 2, bd.rfactor_d, y, x, flag);
}

/* The term kernel's arguments. It stores what adm_csf_den_scale_s() and the
 * two adm_cm_s() calls of compute_adm() accumulate for one sample of the
 * reduced region:
 *   kSlotDen  |rfactor * ref|^p
 *   kSlotCm   decouple_r masked by the threshold of decouple_a (adm2)
 *   kSlotAim  decouple_a masked by the threshold of decouple_r (AIM) */
struct TermArgs {
    Bands bands;
    float *terms;
    int left; /* the reduced region, adm_border_s() */
    int top;
    unsigned region_w;
    unsigned region_h;
    float p_norm;   /* (float)adm_p_norm, read when !is_cube */
    bool is_cube;   /* adm_p_norm == 3.0 */
    bool bypass_cm; /* adm_bypass_cm: no masking threshold */
};

/* The term kernel's shape, for the twin and the probe (ADR-1501): no required
 * sub-group size and the large register file. With the default register file
 * icpx compiles the kernel at SIMD-32 for lnl-m, bmg-g21 and bmg-g31 (Xe2),
 * where it spills two registers (128 bytes of scratch memory, measured on an
 * Arc B580; ADR-1395). A required size of 16 spills on every other target of
 * the default AOT list (58 to 91 registers), and Xe-LP has no large register
 * file. With 256 registers and the size left to the compiler no target
 * spills. */
inline constexpr int kTermsSubGroup = 0;
inline constexpr int kTermsGrf = 256;

/* The per-sample terms are stored slot by slot and, inside a slot, column by
 * column: the row kernel runs one work-item per (slot, row), so at every step
 * neighbouring work-items read neighbouring addresses. */
inline size_t term_index(unsigned slot, unsigned x, unsigned y, unsigned region_w,
                         unsigned region_h)
{
    return ((size_t)slot * (size_t)region_w + (size_t)x) * (size_t)region_h + (size_t)y;
}

/* The 3x3 window of adm_cm_thresh3x3_s() around (x, y), as indices. */
struct Window {
    int ym;
    int y;
    int yp;
    int xm;
    int x;
    int xp;
};

inline Neighbours neighbours(const float *csf_f, const Bands &bd, int band, const Window &w)
{
    return {.above_left = csf_f[band_index(bd, band, w.ym, w.xm)],
            .above = csf_f[band_index(bd, band, w.ym, w.x)],
            .above_right = csf_f[band_index(bd, band, w.ym, w.xp)],
            .left = csf_f[band_index(bd, band, w.y, w.xm)],
            .right = csf_f[band_index(bd, band, w.y, w.xp)],
            .below_left = csf_f[band_index(bd, band, w.yp, w.xm)],
            .below = csf_f[band_index(bd, band, w.yp, w.x)],
            .below_right = csf_f[band_index(bd, band, w.yp, w.xp)]};
}

/* adm_cm_thresh3x3_s(): one sum per band, the three added in band order.
 * `csf` is the unfiltered CSF buffer whose samples supply the centre taps,
 * `csf_f` its filtered companion. */
inline float threshold(const float *csf, const float *csf_f, const Bands &bd, const Window &w)
{
    float accum = 0.0f;
    accum += thresh_band(neighbours(csf_f, bd, 0, w), csf[band_index(bd, 0, w.y, w.x)]);
    accum += thresh_band(neighbours(csf_f, bd, 1, w), csf[band_index(bd, 1, w.y, w.x)]);
    accum += thresh_band(neighbours(csf_f, bd, 2, w), csf[band_index(bd, 2, w.y, w.x)]);
    return accum;
}

struct Thresholds {
    float additive; /* of decouple_a, masks the restored signal */
    float restored; /* of decouple_r, masks the additive signal */
};

/* The three terms of sub-band `band` (a constant at every call) of region
 * sample (rx, ry). */
inline void store_terms(const TermArgs &a, unsigned band, float rfactor, unsigned ry, unsigned rx,
                        const Window &w, const Thresholds &thr)
{
    const Bands &bd = a.bands;
    const float src = bd.ref_band[band_index(bd, (int)band + 1, w.y, w.x)];
    const size_t index = band_index(bd, (int)band, w.y, w.x);
    a.terms[term_index(kSlotDen + band, rx, ry, a.region_w, a.region_h)] =
        den_term(rfactor, src, a.is_cube, a.p_norm);
    a.terms[term_index(kSlotCm + band, rx, ry, a.region_w, a.region_h)] =
        cm_term(bd.csf_r[index], thr.additive, a.is_cube, a.p_norm);
    a.terms[term_index(kSlotAim + band, rx, ry, a.region_w, a.region_h)] =
        cm_term(bd.csf_a[index], thr.restored, a.is_cube, a.p_norm);
}

/* The term kernel for sample (rx, ry) of the reduced region. */
inline void terms_sample(const TermArgs &a, unsigned ry, unsigned rx)
{
    const Bands &bd = a.bands;
    const int x = a.left + (int)rx;
    const int y = a.top + (int)ry;
    const Window w = {.ym = before(y, bd.half_h),
                      .y = y,
                      .yp = after(y, bd.half_h),
                      .xm = before(x, bd.half_w),
                      .x = x,
                      .xp = after(x, bd.half_w)};
    Thresholds thr = {.additive = 0.0f, .restored = 0.0f};
    if (!a.bypass_cm) {
        thr.additive = threshold(bd.csf_a, bd.csf_fa, bd, w);
        thr.restored = threshold(bd.csf_r, bd.csf_fr, bd, w);
    }
    store_terms(a, 0u, bd.rfactor_h, ry, rx, w, thr);
    store_terms(a, 1u, bd.rfactor_v, ry, rx, w, thr);
    store_terms(a, 2u, bd.rfactor_d, ry, rx, w, thr);
}

/* The row kernel's arguments. */
struct RowArgs {
    const float *terms;
    float *rows; /* kTermSlots x region_h, slot by slot */
    unsigned region_w;
    unsigned region_h;
};

/* The row kernel for work-item `id` of kTermSlots * region_h: row `id %
 * region_h` of slot `id / region_h`. */
inline void row_item(const RowArgs &a, size_t id)
{
    const auto slot = (unsigned)(id / a.region_h);
    const auto y = (unsigned)(id - (size_t)slot * a.region_h);
    a.rows[id] =
        row_sum(a.terms + term_index(slot, 0u, y, a.region_w, a.region_h), a.region_h, a.region_w);
}

/* Host code: adm_fold3_s() over the rows of one slot, top to bottom. */
inline float fold_rows(const float *rows, unsigned count)
{
    float accum = 0.0f;
    for (unsigned y = 0u; y < count; y++) {
        accum += rows[y];
    }
    return accum;
}

/* Host code: the gain limit in the forms above. */
inline GainLimit make_gain_limit(double limit)
{
    GainLimit out = {};
    out.value = (float)limit;
    out.is_float = ((double)out.value == limit) ? 1 : 0;
    int exponent = 0;
    const double fraction = std::frexp(limit, &exponent);
    out.mant = (uint64_t)std::ldexp(fraction, 53);
    out.exp = exponent - 53;
    return out;
}

} // namespace vmaf_sycl_fadm

#endif /* VMAF_FEATURE_SYCL_SYCL_FLOAT_ADM_MATH_H_ */
