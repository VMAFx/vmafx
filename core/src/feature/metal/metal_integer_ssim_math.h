/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2 AND BSD-2-Clause
 *
 *  The window moments and the per-pixel term of the fixed-point SSIM
 *  extractor for integer_ssim_metal (ADR-1498), valid as Metal Shading
 *  Language (integer_ssim.metal) and as host C and C++, where
 *  test_metal_integer_ssim_math holds it against integer_ssim.c. The term is
 *  the fp64 expression of integer_ssim.c::ssim_reduce_row_range():
 *
 *      w_d = m.w;
 *      c1 = sm * sm * SSIM_K1 * w_d * w_d;
 *      c2 = sm * sm * SSIM_K2 * w_d * w_d;
 *      mx2 = m.mux * (double)m.mux;
 *      mxy = m.mux * (double)m.muy;
 *      my2 = m.muy * (double)m.muy;
 *      *ssim += m.w * (2 * mxy + c1) * (c2 + 2 * (m.xy * w_d - mxy)) /
 *               ((mx2 + my2 + c1) * (m.x2 * w_d - mx2 + m.y2 * w_d - my2 + c2));
 *
 *  Metal has no fp64 type and calc_ssim() adds the terms into one double, so
 *  the term has to be that double: vmaf_mtl_issim_term_bits() runs the
 *  reference's operations one for one, in the reference's order, on fp64
 *  values held in 64-bit integers (metal_soft_signed.h), and returns the
 *  term's fp64 bit pattern. The kernel stores it per pixel and the host adds
 *  the plane in calc_ssim()'s raster order.
 *
 *  This is core/src/feature/sycl/sycl_integer_ssim_math.h statement for
 *  statement (ADR-1443), with the moment accumulation and the window weight
 *  of integer_ssim_sycl.cpp; the two are one behaviour in two files until
 *  T-METAL-SYCL-FP64-FREE-ARITHMETIC-COPIES-2026-10-03 merges them. Name
 *  mapping (namespace vmaf_sycl_issim -> Metal):
 *    struct Moments, Stabilisers,
 *           Products, ProductSums -> VmafMtlIssimMoments, ...Stabilisers,
 *                                    ...Products, ...ProductSums
 *    make_stabilisers()           -> vmaf_mtl_issim_stabilisers() from the
 *                                    two fp64 bit patterns the host forms
 *                                    (integer_ssim_metal.mm)
 *    times_weight, products,
 *    product_sums_rounded,
 *    products_are_exact,
 *    product_sums_exact,
 *    term_bits                    -> vmaf_mtl_issim_<name>
 *    kFullWeight, kFullWeightLog2,
 *    kExactProductBound           -> VMAF_MTL_ISSIM_FULL_WEIGHT, ...
 *    ISSIM_KERNEL, tap_range(),
 *    tap_weight() (twin TU)       -> vmaf_mtl_issim_kernel,
 *                                    vmaf_mtl_issim_tap_range(),
 *                                    vmaf_mtl_issim_tap_weight()
 *
 *  The moments are integers and non-negative. A product of two of them is
 *  exact in 64 bits at every bit depth (mux, muy < 2^32; x2, xy, y2 < 2^48;
 *  w <= 2^16), so each of the reference's products of two converted integers
 *  is one rounding of an exact integer, vmaf_mtl_signed_from_u64().
 */

#ifndef VMAF_FEATURE_METAL_METAL_INTEGER_SSIM_MATH_H_
#define VMAF_FEATURE_METAL_METAL_INTEGER_SSIM_MATH_H_

#include "metal_portable.h"
#include "metal_soft_signed.h"

/* gaussian_filter_init(1.5, 5) of integer_ssim.c: nine taps summing to 256,
 * used for both passes. */
#define VMAF_MTL_ISSIM_TAPS 9
#define VMAF_MTL_ISSIM_HALF 4

VMAF_MTL_CONSTANT vmaf_mtl_i32 vmaf_mtl_issim_kernel[VMAF_MTL_ISSIM_TAPS] = {2,  9,  28, 55, 68,
                                                                             55, 28, 9,  2};

/* The weight of a window that lies inside the frame: 256 in each direction. */
#define VMAF_MTL_ISSIM_FULL_WEIGHT VMAF_MTL_U64(65536)
#define VMAF_MTL_ISSIM_FULL_WEIGHT_LOG2 16
/* The products below this bound make sums the reference does not round. */
#define VMAF_MTL_ISSIM_EXACT_PRODUCT_BOUND (VMAF_MTL_U64(1) << 52)

/* The arguments of the term kernel, laid out alike in MSL and on the host:
 * the frame and the fp64 bit patterns of fl64(sm * sm * SSIM_K1) and
 * fl64(sm * sm * SSIM_K2), which the host forms with the reference's
 * expression. */
typedef struct VmafMtlIssimParams {
    vmaf_mtl_u32 width;
    vmaf_mtl_u32 height;
    vmaf_mtl_u64 k1_bits;
    vmaf_mtl_u64 k2_bits;
} VmafMtlIssimParams;

/* The taps of the window at `position` that lie inside a line of `extent`
 * samples: the reference's k_min and k_max (last is exclusive). */
typedef struct VmafMtlIssimTaps {
    vmaf_mtl_i32 first;
    vmaf_mtl_i32 last;
} VmafMtlIssimTaps;

VMAF_MTL_FUNC VmafMtlIssimTaps vmaf_mtl_issim_tap_range(vmaf_mtl_u32 position, vmaf_mtl_u32 extent)
{
    const vmaf_mtl_i32 at = (vmaf_mtl_i32)position;
    const vmaf_mtl_i32 first = at < VMAF_MTL_ISSIM_HALF ? VMAF_MTL_ISSIM_HALF - at : 0;
    const vmaf_mtl_i32 last =
        (at + VMAF_MTL_ISSIM_HALF >= (vmaf_mtl_i32)extent) ?
            VMAF_MTL_ISSIM_TAPS - (at + VMAF_MTL_ISSIM_HALF - (vmaf_mtl_i32)extent + 1) :
            VMAF_MTL_ISSIM_TAPS;
    const VmafMtlIssimTaps taps = {first, last};
    return taps;
}

/* The sum of the taps of a range: a line's part of the window weight. */
VMAF_MTL_FUNC vmaf_mtl_i64 vmaf_mtl_issim_tap_weight(VmafMtlIssimTaps taps)
{
    vmaf_mtl_i64 weight = 0;
    for (vmaf_mtl_i32 tap = taps.first; tap < taps.last; ++tap) {
        weight += (vmaf_mtl_i64)vmaf_mtl_issim_kernel[tap];
    }
    return weight;
}

/* The five moment sums of one pass (the weight is not a plane: it is the
 * product of the two tap sums). */
typedef struct VmafMtlIssimSums {
    vmaf_mtl_i64 mux;
    vmaf_mtl_i64 muy;
    vmaf_mtl_i64 x2;
    vmaf_mtl_i64 xy;
    vmaf_mtl_i64 y2;
} VmafMtlIssimSums;

VMAF_MTL_FUNC VmafMtlIssimSums vmaf_mtl_issim_sums_make(vmaf_mtl_i64 mux, vmaf_mtl_i64 muy,
                                                        vmaf_mtl_i64 x2, vmaf_mtl_i64 xy,
                                                        vmaf_mtl_i64 y2)
{
    const VmafMtlIssimSums sums = {mux, muy, x2, xy, y2};
    return sums;
}

/* One horizontal tap of ssim_accumulate_row(): the samples `s` (reference)
 * and `d` (distorted) under the tap `coefficient`. */
VMAF_MTL_FUNC VmafMtlIssimSums vmaf_mtl_issim_horizontal_tap(VmafMtlIssimSums acc,
                                                             vmaf_mtl_i64 coefficient,
                                                             vmaf_mtl_i64 s, vmaf_mtl_i64 d)
{
    return vmaf_mtl_issim_sums_make(acc.mux + coefficient * s, acc.muy + coefficient * d,
                                    acc.x2 + coefficient * s * s, acc.xy + coefficient * s * d,
                                    acc.y2 + coefficient * d * d);
}

/* One vertical tap of ssim_reduce_row_range() over a row of horizontal sums. */
VMAF_MTL_FUNC VmafMtlIssimSums vmaf_mtl_issim_vertical_tap(VmafMtlIssimSums acc,
                                                           vmaf_mtl_i64 coefficient,
                                                           VmafMtlIssimSums row)
{
    return vmaf_mtl_issim_sums_make(acc.mux + coefficient * row.mux,
                                    acc.muy + coefficient * row.muy, acc.x2 + coefficient * row.x2,
                                    acc.xy + coefficient * row.xy, acc.y2 + coefficient * row.y2);
}

/* One pixel's window moments: integer_ssim.c's ssim_moments. */
typedef struct VmafMtlIssimMoments {
    vmaf_mtl_u64 mux;
    vmaf_mtl_u64 muy;
    vmaf_mtl_u64 x2;
    vmaf_mtl_u64 xy;
    vmaf_mtl_u64 y2;
    vmaf_mtl_u64 w;
} VmafMtlIssimMoments;

/* The window's moments from its vertical sums and the tap sums of its rows
 * and columns. Every moment is a sum of non-negative products. */
VMAF_MTL_FUNC VmafMtlIssimMoments vmaf_mtl_issim_moments(VmafMtlIssimSums v,
                                                         vmaf_mtl_i64 row_weight,
                                                         vmaf_mtl_i64 column_weight)
{
    const VmafMtlIssimMoments m = {(vmaf_mtl_u64)v.mux, (vmaf_mtl_u64)v.muy,
                                   (vmaf_mtl_u64)v.x2,  (vmaf_mtl_u64)v.xy,
                                   (vmaf_mtl_u64)v.y2,  (vmaf_mtl_u64)(row_weight * column_weight)};
    return m;
}

/* The part of the reference's c1 and c2 that is the same for every pixel:
 * fl64(sm * sm * SSIM_K1) and fl64(sm * sm * SSIM_K2). */
typedef struct VmafMtlIssimStabilisers {
    VmafMtlSoftSigned k1;
    VmafMtlSoftSigned k2;
} VmafMtlIssimStabilisers;

VMAF_MTL_FUNC VmafMtlIssimStabilisers vmaf_mtl_issim_stabilisers(vmaf_mtl_u64 k1_bits,
                                                                 vmaf_mtl_u64 k2_bits)
{
    const VmafMtlIssimStabilisers k = {vmaf_mtl_signed_from_bits(k1_bits),
                                       vmaf_mtl_signed_from_bits(k2_bits)};
    return k;
}

/* fl64(a * w_d) for a window weight. Every window that lies inside the frame
 * has the weight 2^16, and a product with a power of two is exact: only a
 * window the frame's edge truncates takes the multiplication. */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_issim_times_weight(VmafMtlSoftSigned a, vmaf_mtl_u64 w)
{
    vmaf_mtl_u64 mant = a.mant;
    vmaf_mtl_i32 exp = a.exp + VMAF_MTL_ISSIM_FULL_WEIGHT_LOG2;
    if (w != VMAF_MTL_ISSIM_FULL_WEIGHT) {
        const VmafMtlSoftSigned product = vmaf_mtl_signed_mul(a, vmaf_mtl_signed_from_u64(w));
        mant = product.mant;
        exp = product.exp;
    }
    return vmaf_mtl_signed_make(mant, exp, a.negative != 0u);
}

/* The six products of two integers in the reference's expression, each exact
 * in 64 bits: mux * mux, mux * muy, muy * muy and x2, xy, y2 times the
 * weight. */
typedef struct VmafMtlIssimProducts {
    vmaf_mtl_u64 mx2;
    vmaf_mtl_u64 mxy;
    vmaf_mtl_u64 my2;
    vmaf_mtl_u64 x2w;
    vmaf_mtl_u64 xyw;
    vmaf_mtl_u64 y2w;
} VmafMtlIssimProducts;

VMAF_MTL_FUNC VmafMtlIssimProducts vmaf_mtl_issim_products(VmafMtlIssimMoments m)
{
    const VmafMtlIssimProducts p = {m.mux * m.mux, m.mux * m.muy, m.muy * m.muy,
                                    m.x2 * m.w,    m.xy * m.w,    m.y2 * m.w};
    return p;
}

/* The four values of the reference's expression that the products alone
 * make, before a stabiliser is added:
 *
 *     2 * mxy
 *     2 * (m.xy * w_d - mxy)
 *     mx2 + my2
 *     m.x2 * w_d - mx2 + m.y2 * w_d - my2
 */
typedef struct VmafMtlIssimProductSums {
    VmafMtlSoftSigned twice_mxy;
    VmafMtlSoftSigned twice_covariance;
    VmafMtlSoftSigned mean_squares;
    VmafMtlSoftSigned variances;
} VmafMtlIssimProductSums;

/* As the reference forms them at any bit depth: each product is rounded to
 * fp64 and each sum and difference rounds again, left to right. */
VMAF_MTL_FUNC VmafMtlIssimProductSums vmaf_mtl_issim_product_sums_rounded(VmafMtlIssimProducts p)
{
    const VmafMtlSoftSigned mx2 = vmaf_mtl_signed_from_u64(p.mx2);
    const VmafMtlSoftSigned mxy = vmaf_mtl_signed_from_u64(p.mxy);
    const VmafMtlSoftSigned my2 = vmaf_mtl_signed_from_u64(p.my2);
    const VmafMtlSoftSigned reference_variance =
        vmaf_mtl_signed_sub(vmaf_mtl_signed_from_u64(p.x2w), mx2);
    const VmafMtlSoftSigned both =
        vmaf_mtl_signed_add(reference_variance, vmaf_mtl_signed_from_u64(p.y2w));
    const VmafMtlIssimProductSums sums = {
        vmaf_mtl_signed_twice(mxy),
        vmaf_mtl_signed_twice(vmaf_mtl_signed_sub(vmaf_mtl_signed_from_u64(p.xyw), mxy)),
        vmaf_mtl_signed_add(mx2, my2), vmaf_mtl_signed_sub(both, my2)};
    return sums;
}

/* Whether every product is below 2^52. Then each is an fp64 value as it
 * stands, and so is every sum and difference of the four expressions: an
 * integer below 2^53. That is every window at 8 and 10 bits (mux, muy < 2^26;
 * x2, xy, y2 < 2^36). */
VMAF_MTL_FUNC bool vmaf_mtl_issim_products_are_exact(VmafMtlIssimProducts p)
{
    return (p.mx2 | p.mxy | p.my2 | p.x2w | p.xyw | p.y2w) < VMAF_MTL_ISSIM_EXACT_PRODUCT_BOUND;
}

/* The same four values when vmaf_mtl_issim_products_are_exact(): integer
 * arithmetic, and one conversion each that does not round. The variances are
 * not negative: a weighted mean of squares is at least the square of the
 * weighted mean. */
VMAF_MTL_FUNC VmafMtlIssimProductSums vmaf_mtl_issim_product_sums_exact(VmafMtlIssimProducts p)
{
    const bool anticorrelated = p.xyw < p.mxy;
    const vmaf_mtl_u64 covariance = anticorrelated ? p.mxy - p.xyw : p.xyw - p.mxy;
    const VmafMtlSoftSigned twice_covariance = vmaf_mtl_signed_from_exact(2u * covariance);
    const VmafMtlIssimProductSums sums = {
        vmaf_mtl_signed_from_exact(2u * p.mxy),
        vmaf_mtl_signed_make(twice_covariance.mant, twice_covariance.exp, anticorrelated),
        vmaf_mtl_signed_from_exact(p.mx2 + p.my2),
        vmaf_mtl_signed_from_exact(p.x2w - p.mx2 + p.y2w - p.my2)};
    return sums;
}

/* The fp64 bit pattern of the term the reference adds to `*ssim`. */
VMAF_MTL_FUNC vmaf_mtl_u64 vmaf_mtl_issim_term_bits(VmafMtlIssimMoments m,
                                                    VmafMtlIssimStabilisers k)
{
    const VmafMtlSoftSigned c1 =
        vmaf_mtl_issim_times_weight(vmaf_mtl_issim_times_weight(k.k1, m.w), m.w);
    const VmafMtlSoftSigned c2 =
        vmaf_mtl_issim_times_weight(vmaf_mtl_issim_times_weight(k.k2, m.w), m.w);
    const VmafMtlIssimProducts p = vmaf_mtl_issim_products(m);
    VmafMtlIssimProductSums sums = vmaf_mtl_issim_product_sums_exact(p);
    if (!vmaf_mtl_issim_products_are_exact(p)) {
        sums = vmaf_mtl_issim_product_sums_rounded(p);
    }
    /* m.w * (2 * mxy + c1) * (c2 + 2 * (m.xy * w_d - mxy)) */
    const VmafMtlSoftSigned means =
        vmaf_mtl_issim_times_weight(vmaf_mtl_signed_add(sums.twice_mxy, c1), m.w);
    const VmafMtlSoftSigned numerator =
        vmaf_mtl_signed_mul(means, vmaf_mtl_signed_add(c2, sums.twice_covariance));
    /* (mx2 + my2 + c1) * (m.x2 * w_d - mx2 + m.y2 * w_d - my2 + c2) */
    const VmafMtlSoftSigned denominator = vmaf_mtl_signed_mul(
        vmaf_mtl_signed_add(sums.mean_squares, c1), vmaf_mtl_signed_add(sums.variances, c2));
    return vmaf_mtl_signed_bits(vmaf_mtl_signed_div(numerator, denominator));
}

#endif /* VMAF_FEATURE_METAL_METAL_INTEGER_SSIM_MATH_H_ */
