/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  ADR-1467: the powf() calls of ciede.c's get_r_sub_t() are calls of the C
 *  library.
 *
 *  get_r_sub_t() writes `powf(degrees, 2)`. A compiler may replace that call
 *  by `degrees * degrees`, and clang does; GCC emits the call. The product is
 *  the correctly rounded square. glibc's powf() is not correctly rounded:
 *  where the exact square lies half way between two floats it can return the
 *  other one. The two forms then differ by one float step, and with them the
 *  `ciede2000` score of a clang build and of a GCC build
 *  (T-CIEDE-CLANG-POWF-BUILTIN-2026-10-02). The GCC build is the reference of
 *  the Netflix golden values, so core/src/meson.build builds ciede.c with
 *  `vmaf_libm_call_args`, which keeps the call under clang.
 *
 *  This test includes ciede.c, built with the argument list of the library's
 *  own ciede.c (core/test/test_ciede_libm_call_args.py pins that the two
 *  targets name the same list), and compares get_r_sub_t() over a sweep of
 *  hues with the same expression whose powf() calls go through a volatile
 *  function pointer, which no compiler can replace. Every value must have the
 *  same bits. A build that folds the call fails on the hues whose square is a
 *  tie the library rounds the other way: 4827 of 5.2 million values with
 *  glibc 2.44.
 *
 *  On a C library whose powf() is correctly rounded the two forms agree by
 *  construction and the test cannot tell them apart; it reports the number of
 *  arguments that distinguish them. test_ciede_device_math compares whole
 *  frames of the library's ciede.c with a replay that calls the C library.
 *
 *  Registered for GCC, clang and Apple's clang. icx folds the call and links
 *  Intel's math library, so its build is not held to this (ADR-1467).
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
// NOLINTNEXTLINE(bugprone-suspicious-include): white-box test deliberately includes ciede.c to reach the static get_r_sub_t() (ADR-0141 / ADR-0278).
#include "feature/ciede.c"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Hues: 2^20 floats across [0, 2 * pi), which get_r_sub_t() maps to `degrees`
 * in [-11, 3.4]. */
#define HUE_STEPS (1u << 20)

typedef float (*PowfFn)(float, float);

/* get_r_sub_t(), statement for statement, with every powf() made through
 * `library_powf`. */
static float r_sub_t_library_calls(const float c_bar_prime, const float upcase_h_bar_prime,
                                   PowfFn library_powf)
{
    const float degrees = (radians_to_degrees(upcase_h_bar_prime) - 275.0) * (1.0 / 25.0);

    /* The promotions of get_r_sub_t(), which this reproduces. ADR-0141. */
    // NOLINTBEGIN(performance-type-promotion-in-math-fn)
    return -2.0 *
           sqrt(library_powf(c_bar_prime, 7) /
                (library_powf(c_bar_prime, 7) + library_powf(25., 7))) *
           sin(degrees_to_radians(60.0 * exp(-(library_powf(degrees, 2)))));
    // NOLINTEND(performance-type-promotion-in-math-fn)
}

static uint32_t float_bits(const float value)
{
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float hue_at(const unsigned step)
{
    return (float)((double)step * (2.0 * M_PI / (double)HUE_STEPS));
}

static char *test_r_sub_t_calls_the_library(void)
{
    /* A volatile pointer: the compiler cannot know the callee, so it cannot
     * replace the call. */
    PowfFn volatile library_powf = powf;
    static const float chroma[] = {0.5f, 7.25f, 25.0f, 61.5f, 130.0f};
    unsigned differing = 0u;
    for (unsigned c = 0; c < sizeof(chroma) / sizeof(chroma[0]); c++) {
        for (unsigned step = 0; step < HUE_STEPS; step++) {
            const float hue = hue_at(step);
            const float built = get_r_sub_t(chroma[c], hue);
            const float called = r_sub_t_library_calls(chroma[c], hue, library_powf);
            if (float_bits(built) == float_bits(called))
                continue;
            if (differing++ == 0u) {
                (void)fprintf(stderr, "\nchroma %a hue %a: get_r_sub_t() %a, with library calls %a",
                              (double)chroma[c], (double)hue, (double)built, (double)called);
            }
        }
    }
    if (differing != 0u)
        (void)fprintf(stderr, "\n%u values differ\n", differing);
    mu_assert("get_r_sub_t() must return what its powf() calls return from the C library "
              "(ADR-1467)",
              differing == 0u);
    return NULL;
}

/* Reports whether this C library separates the two forms at all: the number
 * of sweep arguments whose powf(degrees, 2) is not the rounded product. */
static char *test_report_distinguishing_arguments(void)
{
    PowfFn volatile library_powf = powf;
    unsigned ties = 0u;
    for (unsigned step = 0; step < HUE_STEPS; step++) {
        const float degrees = (radians_to_degrees(hue_at(step)) - 275.0) * (1.0 / 25.0);
        volatile float product = degrees * degrees;
        if (float_bits(library_powf(degrees, 2)) != float_bits(product))
            ties++;
    }
    (void)fprintf(stderr, "[powf(x, 2) is not the rounded product on %u of %u arguments] ", ties,
                  HUE_STEPS);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_r_sub_t_calls_the_library);
    mu_run_test(test_report_distinguishing_arguments);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
