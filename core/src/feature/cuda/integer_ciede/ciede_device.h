/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2019 Joshua Holmer
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND MIT
 *
 *  ciede2000 on CUDA: the per-pixel arithmetic of ciede.c, shared by the
 *  kernels (integer_ciede/ciede_score.cu) and the device-free host test
 *  (core/test/test_ciede_device_math.c) (ADR-1426).
 *
 *  ciede.c computes in double and stores in float, and where it does which is
 *  part of its result: get_lab_color() is fp64 up to the cube root, whose
 *  result xyz_to_lab_map() returns as a float; ciede2000() names almost every
 *  intermediate `const float` and evaluates the expression that initialises
 *  it in fp64, because a double literal or a libm call is part of it. This
 *  header is those two functions and their helpers, statement for statement,
 *  with every float-to-double promotion written out: C promotes a float
 *  argument of sqrt(), atan2(), sin(), cos() and exp() to double, C++ (the
 *  language of the kernel) picks the float overload instead.
 *
 *  What cannot be shared is the math library. The host calls glibc, the
 *  device CUDA's implementations, and the two round differently on some
 *  arguments:
 *
 *   - A square is a product, as in ciede.c (ADR-1467): the fp64 square of a
 *     float is exact, and the one float square (`degrees * degrees`) is the
 *     correctly rounded value on both sides.
 *   - sqrt() is correctly rounded on both sides.
 *   - pow(x, 2.4), pow(x, 1 / 3), pow(x, 7), atan2(), sin(), cos() and exp()
 *     are fp64 on both sides and may differ in the last place. Their results
 *     are rounded to float a few operations later, so a difference survives
 *     only when it straddles a float rounding boundary.
 *   - powf(x, 7) is a float function in ciede.c. glibc's powf is not
 *     correctly rounded (0.07 % of the arguments ciede2000 passes round the
 *     other way); CIEDE_POWF() is the correctly rounded value on the device.
 *
 *  A host TU that includes this header must be built without FP contraction;
 *  the fatbin is (ADR-1403).
 */

#ifndef VMAF_SRC_FEATURE_CUDA_INTEGER_CIEDE_CIEDE_DEVICE_H_
#define VMAF_SRC_FEATURE_CUDA_INTEGER_CIEDE_CIEDE_DEVICE_H_

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
#include <numbers>
#endif

#define CIEDE_BLOCK_X 16
#define CIEDE_BLOCK_Y 16

#if defined(DEVICE_CODE)
#define CIEDE_HD __device__ __forceinline__
/* The correctly rounded power: fp64 pow() is accurate to about a unit in its
 * last place, 29 bits below a float's. */
#define CIEDE_POWF(x, y) ((float)pow((double)(x), (double)(y)))
#else
#define CIEDE_HD static inline
#define CIEDE_POWF(x, y) powf((x), (y))
#endif

#ifdef __cplusplus
/* std::numbers::pi is the double nearest to the C branch's literal. */
#define CIEDE_PI std::numbers::pi
#else
#define CIEDE_PI 3.14159265358979323846
#endif
/* powf(25., 7) and pow(25, 7): 25^7 = 6103515625 rounded to float, and exact. */
#define CIEDE_POWF_25_7 6103515648.0f
#define CIEDE_POW_25_7 6103515625.0

/* ciede.c's LABColor. */
/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no `using`. ADR-1138. */
typedef struct CiedeLab {
    float l;
    float a;
    float b;
} CiedeLab;
/* NOLINTEND(modernize-use-using) */

/* pow(x, 2) for a float x: the exact fp64 square. */
CIEDE_HD double ciede_sq(float x)
{
    return (double)x * (double)x;
}

/* ------------------------------------------------------------------ */
/* get_lab_color() and its helpers.                                     */
/* ------------------------------------------------------------------ */

/* rgb_to_xyz_map() */
CIEDE_HD double ciede_rgb_to_xyz_map(double c)
{
    if (c > 10. / 255.) {
        const double A = 0.055;
        const double D = 1.0 / 1.055;
        return pow((c + A) * D, 2.4);
    }
    const double D = 1.0 / 12.92;
    return (c * D);
}

/* xyz_to_lab_map(): returns float, as the reference does. */
CIEDE_HD float ciede_xyz_to_lab_map(double c)
{
    const double KAPPA = 24389.0 / 27.0;
    const double EPSILON = 216.0 / 24389.0;

    if (c > EPSILON)
        return (float)pow(c, 1.0 / 3.0);
    return (float)((KAPPA * c + 16.0) * (1.0 / 116.0));
}

/* get_lab_color(): the samples arrive as the floats the reference converts
 * them to before the call. */
CIEDE_HD CiedeLab ciede_lab_color(double y, double u, double v, unsigned bpc)
{
    const double scale = (double)(1u << (bpc - 8u));

    y = (y - 16. * scale) * (1. / (219. * scale));
    u = (u - 128. * scale) * (1. / (224. * scale));
    v = (v - 128. * scale) * (1. / (224. * scale));

    /* Assumes BT.709 */
    double r = y + 1.28033 * v;
    double g = y - 0.21482 * u - 0.38059 * v;
    double b = y + 2.12798 * u;

    r = ciede_rgb_to_xyz_map(r);
    g = ciede_rgb_to_xyz_map(g);
    b = ciede_rgb_to_xyz_map(b);

    double x = r * 0.4124564390896921 + g * 0.357576077643909 + b * 0.18043748326639894;
    y = r * 0.21267285140562248 + g * 0.715152155287818 + b * 0.07217499330655958;
    double z = r * 0.019333895582329317 + g * 0.119192025881303 + b * 0.9503040785363677;

    x = (double)ciede_xyz_to_lab_map(x * (1.0 / 0.95047));
    y = (double)ciede_xyz_to_lab_map(y);
    z = (double)ciede_xyz_to_lab_map(z * (1.0 / 1.08883));

    CiedeLab lab;
    lab.l = (float)((116.0 * y) - 16.0);
    lab.a = (float)(500.0 * (x - y));
    lab.b = (float)(200.0 * (y - z));
    return lab;
}

/* ------------------------------------------------------------------ */
/* ciede2000() and its helpers.                                         */
/* ------------------------------------------------------------------ */

/* get_h_prime() */
CIEDE_HD float ciede_h_prime(const float x, const float y)
{
    if ((x == 0.0f) && (y == 0.0f))
        return 0.0f;
    float hue_angle = (float)atan2((double)x, (double)y);
    if (hue_angle < 0.0f)
        hue_angle = (float)((double)hue_angle + 2. * CIEDE_PI);
    return hue_angle;
}

/* get_delta_h_prime() */
CIEDE_HD float ciede_delta_h_prime(const float c1, const float c2, const float h_prime_1,
                                   const float h_prime_2)
{
    if ((c1 == 0.0f) || (c2 == 0.0f))
        return 0.0f;
    const float diff = h_prime_1 - h_prime_2;
    const float abs_diff = (diff < 0.0f) ? -diff : diff;
    if ((double)abs_diff <= CIEDE_PI)
        return h_prime_2 - h_prime_1;
    if (h_prime_2 <= h_prime_1)
        return (float)((double)(h_prime_2 - h_prime_1) + 2. * CIEDE_PI);
    return (float)((double)(h_prime_2 - h_prime_1) - 2. * CIEDE_PI);
}

/* get_upcase_h_bar_prime() */
CIEDE_HD float ciede_upcase_h_bar_prime(const float h_prime_1, const float h_prime_2)
{
    const float diff = h_prime_1 - h_prime_2;
    const double abs_diff = (diff < 0.0f) ? -(double)diff : (double)diff;
    const float sum = h_prime_1 + h_prime_2;
    return (abs_diff > CIEDE_PI) ? (float)(((double)sum + 2.0 * CIEDE_PI) / 2.0) :
                                   (float)((double)sum / 2.0);
}

/* get_upcase_t() */
CIEDE_HD float ciede_upcase_t(const float upcase_h_bar_prime)
{
    const double h = (double)upcase_h_bar_prime;
    return (float)(1.0 - 0.17 * cos(h - CIEDE_PI / 6.0) + 0.24 * cos(2.0 * h) +
                   0.32 * cos(3.0 * h + CIEDE_PI / 30.0) -
                   0.20 * cos(4.0 * h - 7.0 * CIEDE_PI / 20.0));
}

/* radians_to_degrees() */
CIEDE_HD float ciede_radians_to_degrees(const float radians)
{
    return (float)((double)radians * (180.0 / CIEDE_PI));
}

/* degrees_to_radians() */
CIEDE_HD float ciede_degrees_to_radians(const float degrees)
{
    return (float)((double)degrees * (CIEDE_PI / 180.0));
}

/* get_r_sub_t() */
CIEDE_HD float ciede_r_sub_t(const float c_bar_prime, const float upcase_h_bar_prime)
{
    const float degrees =
        (float)(((double)ciede_radians_to_degrees(upcase_h_bar_prime) - 275.0) * (1.0 / 25.0));
    const float c7 = CIEDE_POWF(c_bar_prime, 7.0f);
    const float ratio = c7 / (c7 + CIEDE_POWF_25_7);
    const float exponent = -(degrees * degrees);
    /* degrees_to_radians() takes a float: 60 * exp() is rounded on the way in. */
    const float sixty = (float)(60.0 * exp((double)exponent));
    return (float)(-2.0 * sqrt((double)ratio) * sin((double)ciede_degrees_to_radians(sixty)));
}

/* ciede2000() with ksub = {0.65, 1.0, 4.0}, the values ciede.c passes. */
CIEDE_HD float ciede_delta_e(CiedeLab color_1, CiedeLab color_2)
{
    const float ksub_l = (float)0.65;
    const float ksub_c = (float)1.0;
    const float ksub_h = (float)4.0;

    const float delta_l_prime = color_2.l - color_1.l;
    const float l_bar = (color_1.l + color_2.l) / 2.0f;
    const float c1 = (float)sqrt(ciede_sq(color_1.a) + ciede_sq(color_1.b));
    const float c2 = (float)sqrt(ciede_sq(color_2.a) + ciede_sq(color_2.b));
    const float c_bar = (c1 + c2) / 2.0f;
    const double c_bar_7 = pow((double)c_bar, 7.0);
    const double g_factor = 1.0 - sqrt(c_bar_7 / (c_bar_7 + CIEDE_POW_25_7));
    const float a_prime_1 = (float)((double)color_1.a + (double)(color_1.a / 2.0f) * g_factor);
    const float a_prime_2 = (float)((double)color_2.a + (double)(color_2.a / 2.0f) * g_factor);
    const float c_prime_1 = (float)sqrt(ciede_sq(a_prime_1) + ciede_sq(color_1.b));
    const float c_prime_2 = (float)sqrt(ciede_sq(a_prime_2) + ciede_sq(color_2.b));
    const float c_bar_prime = (c_prime_1 + c_prime_2) / 2.0f;
    const float delta_c_prime = c_prime_2 - c_prime_1;
    const double l_sq = ciede_sq(l_bar - 50.0f);
    const float s_sub_l = (float)(1. + ((0.015 * l_sq) / sqrt(20.0 + l_sq)));
    const float s_sub_c = (float)(1. + 0.045 * (double)c_bar_prime);
    const float h_prime_1 = ciede_h_prime(color_1.b, a_prime_1);
    const float h_prime_2 = ciede_h_prime(color_2.b, a_prime_2);
    const float delta_h_prime = ciede_delta_h_prime(c1, c2, h_prime_1, h_prime_2);
    /* A float product, as the reference's (ADR-1476). */
    const float chroma_product = c_prime_1 * c_prime_2;
    const float delta_upcase_h_prime =
        (float)(2.0 * sqrt((double)chroma_product) * sin((double)delta_h_prime / 2.0));
    const float upcase_h_bar_prime = ciede_upcase_h_bar_prime(h_prime_1, h_prime_2);
    const float upcase_t = ciede_upcase_t(upcase_h_bar_prime);
    const float s_sub_upcase_h = (float)(1.0 + 0.015 * (double)c_bar_prime * (double)upcase_t);
    const float r_sub_t = ciede_r_sub_t(c_bar_prime, upcase_h_bar_prime);
    const float lightness = delta_l_prime / (ksub_l * s_sub_l);
    const float chroma = delta_c_prime / (ksub_c * s_sub_c);
    const float hue = delta_upcase_h_prime / (ksub_h * s_sub_upcase_h);

    /* Two float products, rounded to float before the fp64 sum (ADR-1476). */
    const float rotation = r_sub_t * chroma * hue;
    return (float)sqrt(ciede_sq(lightness) + ciede_sq(chroma) + ciede_sq(hue) + (double)rotation);
}

/* One pixel: the six samples as ciede.c hands them to get_lab_color(). */
CIEDE_HD float ciede_pixel(float ref_y, float ref_u, float ref_v, float dis_y, float dis_u,
                           float dis_v, unsigned bpc)
{
    const CiedeLab c1 = ciede_lab_color((double)ref_y, (double)ref_u, (double)ref_v, bpc);
    const CiedeLab c2 = ciede_lab_color((double)dis_y, (double)dis_u, (double)dis_v, bpc);
    return ciede_delta_e(c1, c2);
}

#if !defined(DEVICE_CODE)
/* extract()'s `de00_sum`, shared with the SYCL and HIP twins' hosts. */
#include "feature/ciede_frame_sum.h"
#endif

#endif /* VMAF_SRC_FEATURE_CUDA_INTEGER_CIEDE_CIEDE_DEVICE_H_ */
