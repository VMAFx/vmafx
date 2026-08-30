/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 3 — core/src/feature/adm_csf_tools.h gap-fill.
 *
 *  `adm_native_csf` is the DLM contrast-sensitivity function used by
 *  the float-ADM extractor.  It is only reached transitively from
 *  adm_tools.c's spatial-decomposition setup, which the existing
 *  test_adm_csf.c does NOT compile against (that suite covers a
 *  different barten-style CSF curve).  As a result the inline body
 *  (lines 46-63) shows up at 0 % line / 45 % branch coverage in the
 *  2026-05-31 gcovr baseline.
 *
 *  Drive both the theta=0 (horizontal / vertical) branch and the
 *  theta=45 (diagonal — `spatial_frequency /= 0.7` divide) branch
 *  across a representative sweep of (lambda, viewing-distance,
 *  display-height) inputs.  Cross-check values against the closed-form
 *  formula so any silent algebraic regression in the inline body
 *  surfaces immediately.
 */

#include <math.h>
#include <stdlib.h>

#include "test.h"

#include "feature/adm_csf_tools.h"

static int isclose(float a, float b, float rel)
{
    const float diff = a > b ? a - b : b - a;
    const float scale = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
    return diff <= rel * (scale + 1e-9f);
}

/* Closed-form reference matching the inline body. Keeping it inline
 * (rather than re-importing the production symbol) guards against a
 * silent edit-in-place regression: if adm_native_csf ever changes
 * algebraic form, this reference still computes the DLM-paper curve
 * and the test fails. */
static float reference_adm_native_csf(int lambda, double view_dist, int display_height, int theta)
{
    float r = view_dist * display_height * M_PI / 180.0;
    float spatial_frequency = r / pow(2, lambda + 1);
    if (theta == 45) {
        spatial_frequency /= 0.7;
    }
    return (0.31 + 0.69 * spatial_frequency) * exp(-0.29 * spatial_frequency);
}

/* ------------------------------------------------------------------ */
/* theta = 0 branch (horizontal / vertical channels)                  */
/* ------------------------------------------------------------------ */

static char *test_adm_csf_theta0_lambda0_1080p(void)
{
    /* Canonical 3H / 1080-row viewing — adm_tools.c's default input. */
    float got = adm_native_csf(0, 3.0, 1080, 0);
    float ref = reference_adm_native_csf(0, 3.0, 1080, 0);
    mu_assert("theta=0 lambda=0 1080p matches reference", isclose(got, ref, 1e-5f));
    return NULL;
}

static char *test_adm_csf_theta0_lambda_sweep_4k(void)
{
    /* Higher DVR (2160 rows) — exercises the same path with a larger
     * spatial_frequency input range. */
    for (int lambda = 0; lambda < 5; ++lambda) {
        float got = adm_native_csf(lambda, 3.0, 2160, 0);
        float ref = reference_adm_native_csf(lambda, 3.0, 2160, 0);
        mu_assert("theta=0 lambda sweep 4K matches reference", isclose(got, ref, 1e-5f));
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* theta = 45 branch (diagonal channels — `spatial_frequency /= 0.7`) */
/* ------------------------------------------------------------------ */

static char *test_adm_csf_theta45_lambda0_1080p(void)
{
    float got = adm_native_csf(0, 3.0, 1080, 45);
    float ref = reference_adm_native_csf(0, 3.0, 1080, 45);
    mu_assert("theta=45 lambda=0 1080p matches reference", isclose(got, ref, 1e-5f));
    return NULL;
}

static char *test_adm_csf_theta45_strictly_less_than_theta0(void)
{
    /* Oblique-effect physics: at the same spatial_frequency the
     * diagonal CSF must be lower (the divide by 0.7 increases
     * spatial_frequency, and the (0.31 + 0.69*f) * exp(-0.29*f)
     * envelope decays past f ~ 5 cycles/degree). For a typical 1080p
     * 3H setup at lambda=1 the spatial_frequency is around 14 -- well
     * past the peak. */
    float h_v = adm_native_csf(1, 3.0, 1080, 0);
    float diag = adm_native_csf(1, 3.0, 1080, 45);
    mu_assert("diagonal CSF must be < H/V CSF past the curve peak", diag < h_v);
    return NULL;
}

static char *test_adm_csf_theta45_lambda_sweep_2k(void)
{
    /* Mid-resolution sweep with the diagonal branch. */
    for (int lambda = 0; lambda < 4; ++lambda) {
        float got = adm_native_csf(lambda, 3.0, 1440, 45);
        float ref = reference_adm_native_csf(lambda, 3.0, 1440, 45);
        mu_assert("theta=45 lambda sweep 2K matches reference", isclose(got, ref, 1e-5f));
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Viewing-distance edge cases                                        */
/* ------------------------------------------------------------------ */

static char *test_adm_csf_5h_viewing_distance(void)
{
    /* Non-default 5H viewing distance — increases the DVR, which
     * shifts spatial_frequency up; both branches must still match
     * the closed-form. */
    float h_v = adm_native_csf(0, 5.0, 1080, 0);
    float diag = adm_native_csf(0, 5.0, 1080, 45);
    float ref_h_v = reference_adm_native_csf(0, 5.0, 1080, 0);
    float ref_diag = reference_adm_native_csf(0, 5.0, 1080, 45);
    mu_assert("5H theta=0 matches reference", isclose(h_v, ref_h_v, 1e-5f));
    mu_assert("5H theta=45 matches reference", isclose(diag, ref_diag, 1e-5f));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_csf_theta0_lambda0_1080p);
    mu_run_test(test_adm_csf_theta0_lambda_sweep_4k);
    mu_run_test(test_adm_csf_theta45_lambda0_1080p);
    mu_run_test(test_adm_csf_theta45_strictly_less_than_theta0);
    mu_run_test(test_adm_csf_theta45_lambda_sweep_2k);
    mu_run_test(test_adm_csf_5h_viewing_distance);
    return NULL;
}
