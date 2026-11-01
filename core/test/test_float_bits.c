/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_bits.h: bit identity of doubles and floats. The cases `==` gets
 * wrong for a bit-identity test (+0 against -0, a NaN against itself) and the
 * neighbouring value, at both widths.
 */

#include <math.h>
#include <stdint.h>

#include "test.h"

#include "float_bits.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

static char *test_bits_are_the_ieee_patterns(void)
{
    mu_assert("1.0 is 0x3ff0000000000000", vmaf_test_bits_f64(1.0) == 0x3ff0000000000000u);
    mu_assert("-0.0 is the sign bit alone", vmaf_test_bits_f64(-0.0) == 0x8000000000000000u);
    mu_assert("1.0f is 0x3f800000", vmaf_test_bits_f32(1.0f) == 0x3f800000u);
    mu_assert("-0.0f is the sign bit alone", vmaf_test_bits_f32(-0.0f) == 0x80000000u);
    return NULL;
}

static char *test_identical_values_are_identical(void)
{
    const double third = 1.0 / 3.0;
    mu_assert("a double is identical to itself", vmaf_test_identical_f64(third, 1.0 / 3.0));
    mu_assert("+inf is identical to +inf", vmaf_test_identical_f64(INFINITY, INFINITY));
    mu_assert("+0 is identical to +0", vmaf_test_identical_f64(0.0, 0.0));
    mu_assert("a float is identical to itself", vmaf_test_identical_f32(0.1f, 0.1f));
    mu_assert("-inf is identical to -inf", vmaf_test_identical_f32(-INFINITY, -INFINITY));
    mu_assert("the reporting form agrees on a match",
              vmaf_test_expect_identical_f64("match", third, 1.0 / 3.0));
    mu_assert("the reporting float form agrees on a match",
              vmaf_test_expect_identical_f32("match", 0.1f, 0.1f));
    return NULL;
}

/* The case a `==` passes: +0 and -0 compare equal but differ in the sign bit. */
static char *test_signed_zeros_differ(void)
{
    mu_assert("+0 and -0 differ in bits", !vmaf_test_identical_f64(0.0, -0.0));
    mu_assert("-0 and +0 differ in bits", !vmaf_test_identical_f64(-0.0, 0.0));
    mu_assert("+0.0f and -0.0f differ in bits", !vmaf_test_identical_f32(0.0f, -0.0f));
    mu_assert("the reporting form refuses -0 for +0",
              !vmaf_test_expect_identical_f64("expected mismatch", 0.0, -0.0));
    return NULL;
}

/* Values one bit apart, and the two infinities. */
static char *test_neighbours_differ(void)
{
    mu_assert("1.0 and its successor differ", !vmaf_test_identical_f64(1.0, nextafter(1.0, 2.0)));
    mu_assert("1.0f and its successor differ",
              !vmaf_test_identical_f32(1.0f, nextafterf(1.0f, 2.0f)));
    mu_assert("+inf and -inf differ", !vmaf_test_identical_f64(INFINITY, -INFINITY));
    mu_assert("the reporting float form refuses the successor",
              !vmaf_test_expect_identical_f32("expected mismatch", 1.0f, nextafterf(1.0f, 2.0f)));
    return NULL;
}

/* A NaN is identical to nothing, itself included, as with `==`. */
static char *test_nan_is_never_identical(void)
{
    const double nan64 = (double)NAN;
    const float nan32 = NAN;
    mu_assert("a NaN is not identical to the same NaN", !vmaf_test_identical_f64(nan64, nan64));
    mu_assert("a NaN is not identical to a number", !vmaf_test_identical_f64(nan64, 1.0));
    mu_assert("a number is not identical to a NaN", !vmaf_test_identical_f64(1.0, nan64));
    mu_assert("a float NaN is not identical to itself", !vmaf_test_identical_f32(nan32, nan32));
    mu_assert("a number is not identical to a float NaN", !vmaf_test_identical_f32(1.0f, nan32));
    return NULL;
}

/* Zero tolerance is bit identity: -0 against +0 fails it; a positive one
 * admits the difference it names and nothing beyond. */
static char *test_within_tolerance(void)
{
    mu_assert("zero tolerance refuses -0 for +0", !vmaf_test_within_f64(0.0, -0.0, 0.0));
    mu_assert("zero tolerance accepts identity", vmaf_test_within_f64(0.25, 0.25, 0.0));
    mu_assert("zero tolerance refuses the successor",
              !vmaf_test_within_f64(1.0, nextafter(1.0, 2.0), 0.0));
    mu_assert("a positive tolerance accepts -0 for +0", vmaf_test_within_f64(0.0, -0.0, 1e-9));
    mu_assert("a tolerance admits its own difference", vmaf_test_within_f64(1.0, 1.5, 0.5));
    mu_assert("a tolerance refuses a larger difference", !vmaf_test_within_f64(1.0, 1.75, 0.5));
    mu_assert("a NaN is within nothing", !vmaf_test_within_f64((double)NAN, 1.0, 1.0));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_bits_are_the_ieee_patterns);
    mu_run_test(test_identical_values_are_identical);
    mu_run_test(test_signed_zeros_differ);
    mu_run_test(test_neighbours_differ);
    mu_run_test(test_nan_is_never_identical);
    mu_run_test(test_within_tolerance);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
