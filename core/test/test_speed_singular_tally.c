/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Unit tests for the rate-limited singular-covariance notice shared by the
 *  CPU SpEED extractor and every GPU twin.
 *
 *  A singular covariance matrix is an ordinary outcome — flat or
 *  linearly-graded chroma has no rank-25 covariance — so it used to be
 *  reported once per solve: four lines a frame per channel, which on a real
 *  encode buried every other line of output. What is asserted here is the
 *  contract the fix rests on: every solve is counted, singular or not, so the
 *  summary the close path prints is a true ratio rather than a guess, and the
 *  counter is monotonic across an arbitrary interleaving of the two outcomes.
 */

#include <stdbool.h>
#include <stddef.h>

#include "test.h"

#include "feature/speed_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and the `char *` NULL a
 * passing mu_run_test returns is the C spelling the harness defines. ADR-1138. */

static char *test_tally_counts_every_solve(void)
{
    SpeedInternalSingularTally tally = {0};

    for (unsigned i = 0; i < 10u; i++) {
        speed_internal_tally_solve(&tally, false, "unit-test");
    }
    mu_assert("a regular solve is counted", tally.solves == 10u);
    mu_assert("a regular solve is not singular", tally.singular == 0u);

    return NULL;
}

static char *test_tally_counts_singular_solves(void)
{
    SpeedInternalSingularTally tally = {0};

    /* Interleaved so a counter that only tracked runs of one outcome, or that
     * stopped counting after the first report, would disagree. */
    const bool singular[] = {true, false, true, true, false, true};
    for (size_t i = 0; i < sizeof(singular) / sizeof(singular[0]); i++) {
        speed_internal_tally_solve(&tally, singular[i], "unit-test");
    }

    mu_assert("every solve is counted", tally.solves == 6u);
    mu_assert("every singular solve is counted", tally.singular == 4u);

    return NULL;
}

static char *test_report_tolerates_a_clean_run(void)
{
    SpeedInternalSingularTally tally = {0};

    speed_internal_tally_solve(&tally, false, "unit-test");
    /* Nothing was singular, so this must stay silent rather than print a
     * "0 of 1" line on every close of every clean run. */
    speed_internal_report_singular(&tally, "unit-test");

    mu_assert("reporting does not disturb the counters", tally.solves == 1u);
    mu_assert("reporting does not invent a singular solve", tally.singular == 0u);

    return NULL;
}

static char *test_report_is_idempotent(void)
{
    SpeedInternalSingularTally tally = {0};

    speed_internal_tally_solve(&tally, true, "unit-test");
    speed_internal_report_singular(&tally, "unit-test");
    speed_internal_report_singular(&tally, "unit-test");

    mu_assert("reporting leaves the solve count alone", tally.solves == 1u);
    mu_assert("reporting leaves the singular count alone", tally.singular == 1u);

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_tally_counts_every_solve);
    mu_run_test(test_tally_counts_singular_solves);
    mu_run_test(test_report_tolerates_a_clean_run);
    mu_run_test(test_report_is_idempotent);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
