/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#include <math.h>

#include "feature/adm_score.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's documented
 * /std:clatest C23 surface does not include nullptr (ADR-1138). */

static char *test_undefined_aim_ratio_is_rejected_atomically(void)
{
    double score = 42.0;
    double score_aim = 43.0;

    const int err = vmaf_adm_finalize_scores(1.0, 1.0, 0.0, 0.0, &score, &score_aim);

    mu_assert("undefined AIM ratio must fail", err != 0);
    mu_assert("failed finalization must not modify outputs", score == 42.0 && score_aim == 43.0);
    return NULL;
}

static char *test_nonfinite_adm3_is_rejected_atomically(void)
{
    double adm3 = 44.0;

    const int err = vmaf_adm3_score(NAN, 0.5, 0, 0.5, 0.0, &adm3);

    mu_assert("non-finite ADM3 input must fail", err != 0);
    mu_assert("failed ADM3 calculation must not modify output", adm3 == 44.0);

    const int inf_err = vmaf_adm3_score(INFINITY, 0.5, 1, 0.5, 0.0, &adm3);
    mu_assert("infinite ADM3 input must fail", inf_err != 0);
    mu_assert("failed infinite ADM3 calculation must not modify output", adm3 == 44.0);
    return NULL;
}

static char *test_finite_scores_preserve_existing_results(void)
{
    double score = 0.0;
    double score_aim = 0.0;
    double adm3 = -1.0;

    int err = vmaf_adm_finalize_scores(1.0, 2.0, 3.0, 4.0, &score, &score_aim);
    mu_assert("finite ADM finalization succeeds", err == 0);
    mu_assert("finite ADM ratios are unchanged", score == 0.5 && score_aim == 0.75);

    err = vmaf_adm3_score(0.0, 0.0, 1, 0.5, 0.0, &adm3);
    mu_assert("zero harmonic mean succeeds", err == 0);
    mu_assert("zero harmonic mean stays zero", adm3 == 0.0);

    score = 0.0;
    score_aim = 0.0;
    err = vmaf_adm_finalize_scores(0.0, 0.0, 0.0, 0.0, &score, &score_aim);
    mu_assert("finite flat-frame ADM finalization succeeds", err == 0);
    mu_assert("finite flat frame remains perfect", score == 1.0 && score_aim == 1.0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_undefined_aim_ratio_is_rejected_atomically);
    mu_run_test(test_nonfinite_adm3_is_rejected_atomically);
    mu_run_test(test_finite_scores_preserve_existing_results);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
