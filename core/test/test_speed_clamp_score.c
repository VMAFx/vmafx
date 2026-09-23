/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Unit tests for the SpEED score clamp shared by the CPU extractor and every
 *  GPU twin.
 *
 *  What is asserted here is the one thing the old clamps got wrong. Each
 *  backend bounded its score with a less-than comparison — `MIN(x, max)`,
 *  `x < max ? x : max`, a local `CLIP` macro — and every comparison against
 *  NaN is false, so all of them published a NaN as `max_val`: a finite,
 *  plausible 1000.0 standing in for a computation that produced no number.
 *  The cross-backend parity harness asserts `isfinite` on what it reads, so
 *  the clamp defeated exactly the check meant to catch it.
 *
 *  The boundary cases matter as much as the NaN one: the clamp is strict
 *  less-than, so a score exactly equal to the maximum must come back as the
 *  maximum, and the guard must not reject an ordinary finite score on either
 *  side of it.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "test.h"
#include "mu_table.h"

#include "feature/speed_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and the `char *` NULL a
 * passing mu_run_test returns is the C spelling the harness defines. ADR-1138. */

#define SENTINEL (-12345.0)

/* positive */

static char *test_a_score_below_the_maximum_passes_through(void)
{
    double out = SENTINEL;
    const int err =
        speed_internal_clamp_score(6.141235, 1000.0, 0u, "unit-test", "speed_chroma_uv", &out);
    mu_assert("a finite score is accepted", err == 0);
    mu_assert("a score below the maximum is unchanged", out == 6.141235);
    return NULL;
}

static char *test_a_score_above_the_maximum_is_clamped(void)
{
    double out = SENTINEL;
    const int err =
        speed_internal_clamp_score(4321.0, 1000.0, 7u, "unit-test", "speed_chroma_uv", &out);
    mu_assert("a finite score is accepted", err == 0);
    mu_assert("a score above the maximum is clamped to it", out == 1000.0);
    return NULL;
}

static char *test_a_negative_finite_score_is_not_rejected(void)
{
    /* The guard is about finiteness, not sign. A negative score is a value the
     * caller may legitimately publish; rejecting it here would turn a guard
     * into a policy the extractors never asked for. */
    double out = SENTINEL;
    const int err =
        speed_internal_clamp_score(-3.5, 1000.0, 0u, "unit-test", "speed_temporal", &out);
    mu_assert("a negative finite score is accepted", err == 0);
    mu_assert("a negative finite score is unchanged", out == -3.5);
    return NULL;
}

/* negative — the regression this helper exists for */

static char *test_nan_is_refused_and_not_published_as_the_maximum(void)
{
    double out = SENTINEL;
    const int err =
        speed_internal_clamp_score(NAN, 1000.0, 42u, "unit-test", "speed_chroma_uv", &out);
    mu_assert("a NaN score fails the frame", err == -EINVAL);
    mu_assert("a NaN score does not become the maximum", out != 1000.0);
    mu_assert("the output is left untouched on failure", out == SENTINEL);
    return NULL;
}

static char *test_positive_infinity_is_refused(void)
{
    double out = SENTINEL;
    const int err =
        speed_internal_clamp_score(INFINITY, 1000.0, 1u, "unit-test", "speed_chroma_u", &out);
    mu_assert("+Inf fails the frame", err == -EINVAL);
    mu_assert("the output is left untouched on failure", out == SENTINEL);
    return NULL;
}

static char *test_negative_infinity_is_refused(void)
{
    /* -Inf would have passed the old clamp untouched rather than becoming the
     * maximum, so it was published as a score of -inf. Still not a number. */
    double out = SENTINEL;
    const int err =
        speed_internal_clamp_score(-INFINITY, 1000.0, 2u, "unit-test", "speed_chroma_v", &out);
    mu_assert("-Inf fails the frame", err == -EINVAL);
    mu_assert("the output is left untouched on failure", out == SENTINEL);
    return NULL;
}

/* boundary */

static char *test_a_score_exactly_at_the_maximum_yields_the_maximum(void)
{
    double out = SENTINEL;
    const int err =
        speed_internal_clamp_score(1000.0, 1000.0, 0u, "unit-test", "speed_chroma_uv", &out);
    mu_assert("a score equal to the maximum is accepted", err == 0);
    mu_assert("a strict less-than clamp returns the maximum for equality", out == 1000.0);
    return NULL;
}

static char *test_the_maximum_is_the_callers_option_not_a_constant(void)
{
    /* The golden Python assertions drive speed_chroma with mxv_45, so the
     * bound has to come from the option rather than DEFAULT_SPEED_MAX_VAL. */
    double out = SENTINEL;
    const int err =
        speed_internal_clamp_score(60.0, 45.0, 0u, "unit-test", "speed_chroma_uv", &out);
    mu_assert("a finite score is accepted", err == 0);
    mu_assert("the caller's maximum is what bounds the score", out == 45.0);
    return NULL;
}

static char *test_a_nan_maximum_still_refuses_a_nan_score(void)
{
    /* Ordering matters: the finiteness check must precede the comparison, so a
     * nonsense bound cannot turn into a published score either. */
    double out = SENTINEL;
    const int err = speed_internal_clamp_score(NAN, NAN, 0u, "unit-test", "speed_chroma_uv", &out);
    mu_assert("a NaN score fails regardless of the bound", err == -EINVAL);
    mu_assert("the output is left untouched on failure", out == SENTINEL);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_a_score_below_the_maximum_passes_through),
        MU_TEST(test_a_score_above_the_maximum_is_clamped),
        MU_TEST(test_a_negative_finite_score_is_not_rejected),
        MU_TEST(test_nan_is_refused_and_not_published_as_the_maximum),
        MU_TEST(test_positive_infinity_is_refused),
        MU_TEST(test_negative_infinity_is_refused),
        MU_TEST(test_a_score_exactly_at_the_maximum_yields_the_maximum),
        MU_TEST(test_the_maximum_is_the_callers_option_not_a_constant),
        MU_TEST(test_a_nan_maximum_still_refuses_a_nan_score),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
