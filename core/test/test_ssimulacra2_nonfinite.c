/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#include <math.h>

#include "feature/ssimulacra2_score.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's documented
 * /std:clatest C23 surface does not include nullptr (ADR-1138). */

static char *test_nonfinite_pool_input_is_not_perfect(void)
{
    const double nan_score = vmaf_ss2_finalize_score(NAN);
    const double pos_inf_score = vmaf_ss2_finalize_score(INFINITY);
    const double neg_inf_score = vmaf_ss2_finalize_score(-INFINITY);

    mu_assert("NaN pool input must not become SSIMULACRA 2's perfect 100", !isfinite(nan_score));
    mu_assert("+Inf pool input must remain non-finite", !isfinite(pos_inf_score));
    mu_assert("-Inf pool input must remain non-finite", !isfinite(neg_inf_score));
    return NULL;
}

static char *test_nonfinite_edge_difference_is_not_zeroed(void)
{
    double artifact = 0.0;
    double detail = 0.0;

    vmaf_ss2_split_edge_difference(NAN, &artifact, &detail);

    mu_assert("NaN edge artifact must remain non-finite", !isfinite(artifact));
    mu_assert("NaN edge detail must remain non-finite", !isfinite(detail));
    return NULL;
}

static char *test_finite_edge_difference_keeps_sign_split(void)
{
    double artifact = 0.0;
    double detail = 0.0;

    vmaf_ss2_split_edge_difference(2.5, &artifact, &detail);
    mu_assert("positive edge difference is artifact", artifact == 2.5 && detail == 0.0);
    vmaf_ss2_split_edge_difference(-3.5, &artifact, &detail);
    mu_assert("negative edge difference is detail", artifact == 0.0 && detail == 3.5);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_nonfinite_pool_input_is_not_perfect);
    mu_run_test(test_nonfinite_edge_difference_is_not_zeroed);
    mu_run_test(test_finite_edge_difference_keeps_sign_split);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
