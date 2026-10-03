/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2019 Joshua Holmer
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND MIT
 *
 *  ciede2000 in fp32 pairs: the per-pixel arithmetic of ciede.c, statement
 *  for statement, without the fp64 type. It is the counterpart of
 *  cuda/integer_ciede/ciede_device.h (ADR-1426), which has fp64, and follows
 *  it function for function, so the two can be read side by side. The SYCL
 *  twin runs it because a SYCL kernel has no fp64 type (ADR-0220, ADR-1436);
 *  the HIP twin because its device's fp64 math library costs 17 times the
 *  frame time (ADR-1448). One definition for both: each includes this header
 *  after ff_math.h and the backend primitives that header names, plus
 *
 *    VMAF_FF_LDEXP(x, k)    x * 2^k, exact
 *    vmaf_ffm_base::div_rn(a, b)   the correctly rounded fp32 quotient
 *
 *  ciede.c computes in double and stores in float, and where it does which
 *  is part of its result: get_lab_color() is fp64 up to the cube root, whose
 *  result xyz_to_lab_map() returns as a float; ciede2000() names almost every
 *  intermediate `const float` and evaluates the expression that initialises
 *  it in fp64. Here every fp64 value is an fp32 pair (about 48 bits) and
 *  every fp64 math-library call a pair function (ff_math.h, about 2^-44).
 *  Every `float` of the reference is a float here, rounded from the pair
 *  once, at the statement where the reference rounds.
 *
 *  That makes a per-pixel value the reference's except where the fp64 value
 *  behind one of its floats lies within about 2^-20 of an fp32 step of a
 *  rounding boundary, or where the host's math library itself is not
 *  correctly rounded there. A twin is therefore close to the CPU extractor
 *  by the weight of those pixels (about 1e-11 of the frame score measured),
 *  not bit-identical to it. The CUDA twin, which has fp64, is closer by the
 *  first of the two causes only.
 *
 *  ciede.c's float power call, powf(x, 7), is the correctly rounded value
 *  here, as on CUDA; glibc's is not on 0.07 % of the arguments. Its float
 *  square is a product there as here (ADR-1467).
 *
 *  Device code may use everything here except make_pair() and
 *  make_constants(), which use fp64: they are host code, or evaluated by the
 *  compiler where a backend's host is C (both are constexpr). A translation
 *  unit that includes this header is compiled with contraction off.
 *
 *  The Metal twin compiles the subset Metal Shading Language and host C++
 *  share (VMAF_FF_MSL_SUBSET, see ff_math.h; ADR-1498). Metal has no fp64
 *  type at all, so make_pair() and make_constants() do not exist in a Metal
 *  kernel: the twin's host evaluates them and hands the kernel the result.
 *  Every reference in Metal names its address space: the backend defines
 *  VMAF_FF_REF_SPACE, the space of the Constants and Tables the functions
 *  below take by reference, and this header spells the two parameter types
 *  with it. For the other backends the preprocessed header is what it was
 *  before the subset existed.
 */

#ifndef VMAF_FEATURE_CIEDE_FF_MATH_H_
#define VMAF_FEATURE_CIEDE_FF_MATH_H_

#if !defined(__METAL_VERSION__)
#include <cstddef>
#include <cstdint>
#include <numbers>
#endif
#include "ff_math.h"

#if !defined(VMAF_FF_LDEXP)
#error "ciede_ff_math.h: define VMAF_FF_LDEXP before including it (see the header comment)"
#endif

namespace vmaf_ciede_ff
{

using vmaf_ffm::add_f;
using vmaf_ffm::from_float;
using vmaf_ffm::less;
using vmaf_ffm::mul_f;
using vmaf_ffm::scale;
using vmaf_ffm::sub;
using vmaf_ffm::Tables;
using vmaf_ffm::to_float;
using vmaf_ffm_base::div_rn;
using vmaf_ffm_base::Ff;
using vmaf_ffm_base::ff_add;
using vmaf_ffm_base::ff_mul;
using vmaf_ffm_base::ff_neg;
using vmaf_ffm_base::two_prod;
using vmaf_ffm_base::two_sum;

/* powf(25., 7): 25^7 = 6103515625 rounded to float. */
VMAF_FF_CONSTANT float kPowf25To7 = 6103515648.0f;
/* pow(25, 7), exactly: 6103515648 - 23. */
VMAF_FF_CONSTANT Ff kPow25To7 = VMAF_FF_PAIR_INIT(6103515648.0f, -23.0f);

/* ciede.c's fp64 constants as pairs. make_constants() builds them from the
 * reference's own expressions, on the host or at compile time. */
struct Constants {
    /* get_lab_color(): the limited-range offsets and gains of the bit depth */
    float luma_offset;   /* 16 * scale */
    float chroma_offset; /* 128 * scale */
    Ff luma_gain;        /* 1 / (219 * scale) */
    Ff chroma_gain;      /* 1 / (224 * scale) */
    /* BT.709 */
    Ff r_from_v; /* 1.28033 */
    Ff g_from_u; /* 0.21482 */
    Ff g_from_v; /* 0.38059 */
    Ff b_from_u; /* 2.12798 */
    /* rgb_to_xyz_map() */
    Ff gamma_knee;   /* 10 / 255 */
    Ff gamma_offset; /* 0.055 */
    Ff gamma_gain;   /* 1 / 1.055 */
    Ff linear_gain;  /* 1 / 12.92 */
    /* RGB to XYZ, row by row */
    Ff xyz[9];
    Ff inverse_xn; /* 1 / 0.95047 */
    Ff inverse_zn; /* 1 / 1.08883 */
    /* xyz_to_lab_map() */
    Ff kappa;        /* 24389 / 27 */
    Ff epsilon;      /* 216 / 24389 */
    Ff one_over_116; /* 1 / 116 */
    /* ciede2000() */
    Ff pi;          /* pi */
    Ff two_pi;      /* 2 * pi */
    Ff pi_over_6;   /* pi / 6 */
    Ff pi_over_30;  /* pi / 30 */
    Ff seven_pi_20; /* 7 * pi / 20 */
    Ff degrees;     /* 180 / pi */
    Ff radians;     /* pi / 180 */
    Ff one_over_25; /* 1 / 25 */
    Ff weight_015;  /* 0.015 */
    Ff weight_045;  /* 0.045 */
    Ff t_weight[4]; /* 0.17, 0.24, 0.32, 0.20 */
    float ksub_l;   /* (float)0.65 */
};

/* ciede.c's LABColor. */
struct Lab {
    float l;
    float a;
    float b;
};

#if defined(VMAF_FF_MSL_SUBSET)
#if !defined(VMAF_FF_REF_SPACE)
#error "ciede_ff_math.h: VMAF_FF_MSL_SUBSET needs VMAF_FF_REF_SPACE (see the header comment)"
#endif
/* `const Constants &k` and `const Tables &tables` below name their address
 * space, up to the #undef after pixel(). A macro is not expanded again inside
 * its own expansion. */
#define Constants VMAF_FF_REF_SPACE Constants
#define Tables VMAF_FF_REF_SPACE Tables
#endif

/* pow(x, 2) for a float x: the exact fp64 square. */
VMAF_FF_INLINE Ff sq(float x)
{
    return two_prod(x, x);
}

/* ------------------------------------------------------------------ */
/* get_lab_color() and its helpers                                     */
/* ------------------------------------------------------------------ */

/* rgb_to_xyz_map() */
VMAF_FF_INLINE Ff rgb_to_xyz_map(Ff c, const Constants &k)
{
    if (less(k.gamma_knee, c)) {
        return vmaf_ffm::pow_2_4(ff_mul(ff_add(c, k.gamma_offset), k.gamma_gain));
    }
    return ff_mul(c, k.linear_gain);
}

/* xyz_to_lab_map(): returns float, as the reference does. */
VMAF_FF_INLINE float xyz_to_lab_map(Ff c, const Constants &k)
{
    if (less(k.epsilon, c)) {
        return to_float(vmaf_ffm::cbrt(c));
    }
    return to_float(ff_mul(add_f(ff_mul(k.kappa, c), 16.0f), k.one_over_116));
}

/* get_lab_color(): the samples arrive as the floats the reference converts
 * them to before the call. */
VMAF_FF_INLINE Lab lab_color(float y, float u, float v, const Constants &k)
{
    const Ff luma = mul_f(k.luma_gain, y - k.luma_offset);
    const Ff cb = mul_f(k.chroma_gain, u - k.chroma_offset);
    const Ff cr = mul_f(k.chroma_gain, v - k.chroma_offset);

    /* Assumes BT.709 */
    Ff r = ff_add(luma, ff_mul(k.r_from_v, cr));
    Ff g = sub(sub(luma, ff_mul(k.g_from_u, cb)), ff_mul(k.g_from_v, cr));
    Ff b = ff_add(luma, ff_mul(k.b_from_u, cb));

    r = rgb_to_xyz_map(r, k);
    g = rgb_to_xyz_map(g, k);
    b = rgb_to_xyz_map(b, k);

    const Ff x = ff_add(ff_add(ff_mul(r, k.xyz[0]), ff_mul(g, k.xyz[1])), ff_mul(b, k.xyz[2]));
    const Ff yy = ff_add(ff_add(ff_mul(r, k.xyz[3]), ff_mul(g, k.xyz[4])), ff_mul(b, k.xyz[5]));
    const Ff z = ff_add(ff_add(ff_mul(r, k.xyz[6]), ff_mul(g, k.xyz[7])), ff_mul(b, k.xyz[8]));

    const float fx = xyz_to_lab_map(ff_mul(x, k.inverse_xn), k);
    const float fy = xyz_to_lab_map(yy, k);
    const float fz = xyz_to_lab_map(ff_mul(z, k.inverse_zn), k);

    /* The three results are exact in fp64 before the rounding to float:
     * products of a float with a small integer, sums of two floats. */
#if defined(VMAF_FF_MSL_SUBSET)
    // NOLINTNEXTLINE(modernize-use-designated-initializers): MSL has none, ADR-1498
    return {to_float(add_f(two_prod(116.0f, fy), -16.0f)),
            to_float(mul_f(two_sum(fx, -fy), 500.0f)), to_float(mul_f(two_sum(fy, -fz), 200.0f))};
#else
    return {.l = to_float(add_f(two_prod(116.0f, fy), -16.0f)),
            .a = to_float(mul_f(two_sum(fx, -fy), 500.0f)),
            .b = to_float(mul_f(two_sum(fy, -fz), 200.0f))};
#endif
}

/* ------------------------------------------------------------------ */
/* ciede2000() and its helpers                                         */
/* ------------------------------------------------------------------ */

/* get_h_prime() */
VMAF_FF_INLINE float h_prime(float x, float y, const Constants &k, const Tables &tables)
{
    if ((x == 0.0f) && (y == 0.0f)) {
        return 0.0f;
    }
    float hue_angle = to_float(vmaf_ffm::atan2(x, y, tables.atan));
    if (hue_angle < 0.0f) {
        hue_angle = to_float(add_f(k.two_pi, hue_angle));
    }
    return hue_angle;
}

/* True when |diff| as a double exceeds pi. */
VMAF_FF_INLINE bool beyond_pi(float diff, const Constants &k)
{
    return less(k.pi, from_float(VMAF_FF_FABS(diff)));
}

/* get_delta_h_prime() */
VMAF_FF_INLINE float delta_h_prime(float c1, float c2, float h_prime_1, float h_prime_2,
                                   const Constants &k)
{
    if ((c1 == 0.0f) || (c2 == 0.0f)) {
        return 0.0f;
    }
    const float turn = h_prime_2 - h_prime_1;
    if (!beyond_pi(h_prime_1 - h_prime_2, k)) {
        return turn;
    }
    if (h_prime_2 <= h_prime_1) {
        return to_float(add_f(k.two_pi, turn));
    }
    return to_float(add_f(ff_neg(k.two_pi), turn));
}

/* get_upcase_h_bar_prime() */
VMAF_FF_INLINE float upcase_h_bar_prime(float h_prime_1, float h_prime_2, const Constants &k)
{
    const float sum = h_prime_1 + h_prime_2;
    if (beyond_pi(h_prime_1 - h_prime_2, k)) {
        return to_float(scale(add_f(k.two_pi, sum), 0.5f));
    }
    return sum * 0.5f;
}

/* get_upcase_t() */
VMAF_FF_INLINE float upcase_t(float h, const Constants &k, const Tables &tables)
{
    const VMAF_FF_TABLE_SPACE float *table = tables.sin_cos;
    const Ff cos_1 = vmaf_ffm::sin_cos(sub(from_float(h), k.pi_over_6), table).cos;
    const Ff cos_2 = vmaf_ffm::sin_cos(from_float(2.0f * h), table).cos;
    const Ff cos_3 = vmaf_ffm::sin_cos(ff_add(two_prod(3.0f, h), k.pi_over_30), table).cos;
    const Ff cos_4 = vmaf_ffm::sin_cos(sub(from_float(4.0f * h), k.seven_pi_20), table).cos;
    Ff t = sub(from_float(1.0f), ff_mul(k.t_weight[0], cos_1));
    t = ff_add(t, ff_mul(k.t_weight[1], cos_2));
    t = ff_add(t, ff_mul(k.t_weight[2], cos_3));
    t = sub(t, ff_mul(k.t_weight[3], cos_4));
    return to_float(t);
}

/* get_r_sub_t() */
VMAF_FF_INLINE float r_sub_t(float c_bar_prime, float h_bar, const Constants &k,
                             const Tables &tables)
{
    /* radians_to_degrees(), then (degrees - 275) / 25 */
    const float in_degrees = to_float(mul_f(k.degrees, h_bar));
    const float degrees = to_float(ff_mul(two_sum(in_degrees, -275.0f), k.one_over_25));
    const float c7 = to_float(vmaf_ffm::pow_7(c_bar_prime));
    const float ratio = div_rn(c7, c7 + kPowf25To7);
    const float exponent = -(degrees * degrees);
    /* 60 * exp(), rounded to float on its way into degrees_to_radians() */
    const vmaf_ffm::Exp e = vmaf_ffm::exp(exponent);
    const float sixty = VMAF_FF_LDEXP(to_float(mul_f(e.value, 60.0f)), e.k);
    const float angle = to_float(mul_f(k.radians, sixty));
    const Ff root = vmaf_ffm::sqrt(from_float(ratio));
    const Ff sine = vmaf_ffm::sin_cos(from_float(angle), tables.sin_cos).sin;
    return to_float(scale(ff_mul(root, sine), -2.0f));
}

/* (float)sqrt(pow(a, 2) + pow(b, 2)) */
VMAF_FF_INLINE float hypot_float(float a, float b)
{
    return to_float(vmaf_ffm::sqrt(ff_add(sq(a), sq(b))));
}

/* ciede2000() with ksub = {0.65, 1.0, 4.0}, the values ciede.c passes. */
VMAF_FF_INLINE float delta_e(Lab color_1, Lab color_2, const Constants &k, const Tables &tables)
{
    const float delta_l_prime = color_2.l - color_1.l;
    const float l_bar = (color_1.l + color_2.l) * 0.5f;
    const float c1 = hypot_float(color_1.a, color_1.b);
    const float c2 = hypot_float(color_2.a, color_2.b);
    const float c_bar = (c1 + c2) * 0.5f;
    const Ff c_bar_7 = vmaf_ffm::pow_7(c_bar);
    const Ff g_factor =
        sub(from_float(1.0f), vmaf_ffm::sqrt(vmaf_ffm::div(c_bar_7, ff_add(c_bar_7, kPow25To7))));
    const float a_prime_1 = to_float(add_f(mul_f(g_factor, color_1.a * 0.5f), color_1.a));
    const float a_prime_2 = to_float(add_f(mul_f(g_factor, color_2.a * 0.5f), color_2.a));
    const float c_prime_1 = hypot_float(a_prime_1, color_1.b);
    const float c_prime_2 = hypot_float(a_prime_2, color_2.b);
    const float c_bar_prime = (c_prime_1 + c_prime_2) * 0.5f;
    const float delta_c_prime = c_prime_2 - c_prime_1;
    const Ff l_sq = sq(l_bar - 50.0f);
    const float s_sub_l = to_float(
        add_f(vmaf_ffm::div(ff_mul(k.weight_015, l_sq), vmaf_ffm::sqrt(add_f(l_sq, 20.0f))), 1.0f));
    const float s_sub_c = to_float(add_f(mul_f(k.weight_045, c_bar_prime), 1.0f));
    const float h_prime_1 = h_prime(color_1.b, a_prime_1, k, tables);
    const float h_prime_2 = h_prime(color_2.b, a_prime_2, k, tables);
    const float delta_h = delta_h_prime(c1, c2, h_prime_1, h_prime_2, k);
    /* A float product, as the reference's (ADR-1476). */
    const Ff chord = vmaf_ffm::sqrt(from_float(c_prime_1 * c_prime_2));
    const Ff half_sine = vmaf_ffm::sin_cos(from_float(delta_h * 0.5f), tables.sin_cos).sin;
    const float delta_upcase_h_prime = to_float(scale(ff_mul(chord, half_sine), 2.0f));
    const float h_bar = upcase_h_bar_prime(h_prime_1, h_prime_2, k);
    const float t = upcase_t(h_bar, k, tables);
    const float s_sub_upcase_h = to_float(add_f(mul_f(mul_f(k.weight_015, c_bar_prime), t), 1.0f));
    const float rotation = r_sub_t(c_bar_prime, h_bar, k, tables);
    const float lightness = div_rn(delta_l_prime, k.ksub_l * s_sub_l);
    const float chroma = div_rn(delta_c_prime, s_sub_c);
    const float hue = div_rn(delta_upcase_h_prime, 4.0f * s_sub_upcase_h);

    const Ff squares = ff_add(ff_add(sq(lightness), sq(chroma)), sq(hue));
    /* Two float products, rounded to float before the fp64 sum (ADR-1476). */
    const float cross = rotation * chroma * hue;
    return to_float(vmaf_ffm::sqrt(add_f(squares, cross)));
}

/* One pixel: the six samples as ciede.c hands them to get_lab_color(). */
struct Samples {
    float y;
    float u;
    float v;
};

VMAF_FF_INLINE float pixel(Samples ref, Samples dis, const Constants &k, const Tables &tables)
{
    const Lab c1 = lab_color(ref.y, ref.u, ref.v, k);
    const Lab c2 = lab_color(dis.y, dis.u, dis.v, k);
    return delta_e(c1, c2, k, tables);
}

#if defined(VMAF_FF_MSL_SUBSET)
#undef Constants
#undef Tables
#endif

/* ------------------------------------------------------------------ */
/* Host code, or the compiler's (constexpr)                            */
/* ------------------------------------------------------------------ */

#if !defined(__METAL_VERSION__)

/* An fp64 value as a pair: hi its nearest float, lo what remains. */
VMAF_FF_INLINE constexpr Ff make_pair(double value)
{
    const float high = (float)value;
    return {.hi = high, .lo = (float)(value - (double)high)};
}

/* Not device code: ciede.c's constants for a bit depth, from its
 * expressions. */
VMAF_FF_INLINE constexpr Constants make_constants(unsigned bpc)
{
    const double pi = std::numbers::pi; /* the constant ciede.c names */
#if defined(VMAF_FF_MSL_SUBSET)
    /* The same power of two, shifted unsigned. */
    const double depth = (double)(1u << (bpc - 8u));
#else
    const double depth = (double)(1 << (bpc - 8u));
#endif
    Constants k = {};
    k.luma_offset = (float)(16. * depth);
    k.chroma_offset = (float)(128. * depth);
    k.luma_gain = make_pair(1. / (219. * depth));
    k.chroma_gain = make_pair(1. / (224. * depth));
    k.r_from_v = make_pair(1.28033);
    k.g_from_u = make_pair(0.21482);
    k.g_from_v = make_pair(0.38059);
    k.b_from_u = make_pair(2.12798);
    k.gamma_knee = make_pair(10. / 255.);
    k.gamma_offset = make_pair(0.055);
    k.gamma_gain = make_pair(1.0 / 1.055);
    k.linear_gain = make_pair(1.0 / 12.92);
    const double xyz[9] = {0.4124564390896921,   0.357576077643909, 0.18043748326639894,
                           0.21267285140562248,  0.715152155287818, 0.07217499330655958,
                           0.019333895582329317, 0.119192025881303, 0.9503040785363677};
    for (size_t i = 0; i < 9; i++) {
        k.xyz[i] = make_pair(xyz[i]);
    }
    k.inverse_xn = make_pair(1.0 / 0.95047);
    k.inverse_zn = make_pair(1.0 / 1.08883);
    k.kappa = make_pair(24389.0 / 27.0);
    k.epsilon = make_pair(216.0 / 24389.0);
    k.one_over_116 = make_pair(1.0 / 116.0);
    k.pi = make_pair(pi);
    k.two_pi = make_pair(2. * pi);
    k.pi_over_6 = make_pair(pi / 6.0);
    k.pi_over_30 = make_pair(pi / 30.0);
    k.seven_pi_20 = make_pair(7.0 * pi / 20.0);
    k.degrees = make_pair(180.0 / pi);
    k.radians = make_pair(pi / 180.0);
    k.one_over_25 = make_pair(1.0 / 25.0);
    k.weight_015 = make_pair(0.015);
    k.weight_045 = make_pair(0.045);
    k.t_weight[0] = make_pair(0.17);
    k.t_weight[1] = make_pair(0.24);
    k.t_weight[2] = make_pair(0.32);
    k.t_weight[3] = make_pair(0.20);
    k.ksub_l = (float)0.65;
    return k;
}

#endif /* !__METAL_VERSION__ */

} // namespace vmaf_ciede_ff

#endif /* VMAF_FEATURE_CIEDE_FF_MATH_H_ */
