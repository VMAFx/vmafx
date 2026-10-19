/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include "test.h"
#include "feature/barten_csf_tools.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define EPS 0.00001

/* Test support function. `static` (vs. upstream's external linkage) is a
 * fork-local lint-cleanup per CLAUDE.md §12 r12; the helper is only used
 * inside this translation unit. Matches the convention already used in
 * upstream's own test_vif_tools.c. */
static int almost_equal(double a, double b)
{
    double diff = a > b ? a - b : b - a;
    return diff < EPS;
}

static char *test_barten_csf_sens(void)
{
    mu_assert("barten rod/cone sensitivity mismatch",
              almost_equal(barten_rod_cone_sens(150.0), 30.141537));
    mu_assert("barten rod/cone sensitivity mismatch",
              almost_equal(barten_rod_cone_sens(100.0), 30.121951521378232));
    mu_assert("barten mtf mismatch", almost_equal(barten_mtf(3.0), 0.5792610023712589));
    return NULL;
}

static char *test_barten_csf_lum_high(void)
{
    mu_assert("barten csf mismatch lum 100",
              almost_equal(barten_csf(3, 3.0, 1080, 100.0, 1.0), 26.97588185355269));
    mu_assert("barten csf mismatch lum 100",
              almost_equal(barten_csf(3, 3.0, 1080, 100.0, 0.5), 13.4879409268));
    mu_assert("barten csf mismatch lum 80",
              almost_equal(barten_csf(3, 3.0, 1080, 80.0, 1.0), 26.949246017567383));
    mu_assert("barten csf mismatch lum 50",
              almost_equal(barten_csf(3, 3.0, 1080, 50.0, 1.0), 26.9958));
    return NULL;
}

static char *test_barten_csf_lum_low(void)
{
    mu_assert("barten csf mismatch lum 0.001",
              almost_equal(barten_csf(3, 3.0, 1080, 0.001, 1.0), 0.112742));
    mu_assert("barten csf mismatch lum 0.002",
              almost_equal(barten_csf(3, 3.0, 1080, 0.002, 1.0), 0.154019));
    mu_assert("barten csf mismatch lum 0.01",
              almost_equal(barten_csf(3, 3.0, 1080, 0.01, 1.0), 0.439781));
    mu_assert("barten csf mismatch lum 0.05",
              almost_equal(barten_csf(3, 3.0, 1080, 0.05, 1.0), 1.240470));
    return NULL;
}

static char *test_barten_csf_lum_mid(void)
{
    mu_assert("barten csf mismatch lum 1.0",
              almost_equal(barten_csf(3, 3.0, 1080, 1.0, 1.0), 9.063454));
    mu_assert("barten csf mismatch lum 10.0",
              almost_equal(barten_csf(3, 3.0, 1080, 10.0, 1.0), 23.722300));
    mu_assert("barten csf mismatch lum 150.0",
              almost_equal(barten_csf(3, 3.0, 1080, 150.0, 1.0), 27.113543));
    mu_assert("barten csf mismatch lum 155.0",
              almost_equal(barten_csf(3, 3.0, 1080, 155.0, 1.0), 27.114515));
    return NULL;
}

static char *test_barten_csf_interp(void)
{
    mu_assert("linear interpolate",
              almost_equal(linear_interpolate((float)log10(20), barten_csf_params[4][1],
                                              (float)log10(150.0), barten_csf_params[5][1],
                                              (float)log10(50.0)),
                           0.361679));
    return NULL;
}

static char *test_barten_csf_blend_1080p_hv(void)
{
    mu_assert("csf blend scale 0 H/V 1080p 3H",
              almost_equal(barten_watson_blend_csf(0, 0, 3.0, 1080), 0.01183));
    mu_assert("csf blend scale 1 H/V 1080p 3H",
              almost_equal(barten_watson_blend_csf(1, 0, 3.0, 1080), 0.025026));
    mu_assert("csf blend scale 2 H/V 1080p 3H",
              almost_equal(barten_watson_blend_csf(2, 0, 3.0, 1080), 0.04295));
    mu_assert("csf blend scale 3 H/V 1080p 3H",
              almost_equal(barten_watson_blend_csf(3, 0, 3.0, 1080), 0.058621));
    return NULL;
}

static char *test_barten_csf_blend_1080p_d(void)
{
    mu_assert("csf blend scale 0 D 1080p 3H",
              almost_equal(barten_watson_blend_csf(0, 1, 3.0, 1080), 0.004302));
    mu_assert("csf blend scale 1 D 1080p 3H",
              almost_equal(barten_watson_blend_csf(1, 1, 3.0, 1080), 0.011778));
    mu_assert("csf blend scale 2 D 1080p 3H",
              almost_equal(barten_watson_blend_csf(2, 1, 3.0, 1080), 0.023918));
    mu_assert("csf blend scale 3 D 1080p 3H",
              almost_equal(barten_watson_blend_csf(3, 1, 3.0, 1080), 0.035901));
    return NULL;
}

static char *test_barten_csf_blend_2160p_hv(void)
{
    mu_assert("csf blend scale 0 H/V 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf(0, 0, 1.5, 2160), 0.01183));
    mu_assert("csf blend scale 1 H/V 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf(1, 0, 1.5, 2160), 0.025026));
    mu_assert("csf blend scale 2 H/V 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf(2, 0, 1.5, 2160), 0.04295));
    mu_assert("csf blend scale 3 H/V 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf(3, 0, 1.5, 2160), 0.058621));
    return NULL;
}

static char *test_barten_csf_blend_2160p_d(void)
{
    mu_assert("csf blend scale 0 D 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf(0, 1, 1.5, 2160), 0.004302));
    mu_assert("csf blend scale 1 D 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf(1, 1, 1.5, 2160), 0.011778));
    mu_assert("csf blend scale 2 D 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf(2, 1, 1.5, 2160), 0.023918));
    mu_assert("csf blend scale 3 D 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf(3, 1, 1.5, 2160), 0.035901));
    return NULL;
}

static char *test_barten_csf_blend_mae_hv(void)
{
    mu_assert("v1017 csf blend scale 0 H/V 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(0, 0, 1.5, 2160), 0.011249));
    mu_assert("v1017 csf blend scale 1 H/V 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(1, 0, 1.5, 2160), 0.022606));
    mu_assert("v1017 csf blend scale 2 H/V 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(2, 0, 1.5, 2160), 0.035930));
    mu_assert("v1017 csf blend scale 3 H/V 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(3, 0, 1.5, 2160), 0.045673));
    return NULL;
}

static char *test_barten_csf_blend_mae_d(void)
{
    mu_assert("v1017 csf blend scale 0 D 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(0, 1, 1.5, 2160), 0.004097));
    mu_assert("v1017 csf blend scale 1 D 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(1, 1, 1.5, 2160), 0.010921));
    mu_assert("v1017 csf blend scale 2 D 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(2, 1, 1.5, 2160), 0.021430));
    mu_assert("v1017 csf blend scale 3 D 2160p 1.5H == 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(3, 1, 1.5, 2160), 0.031313));
    return NULL;
}

/* Netflix/vmaf 12aa1cb44: the v1.0.17 CSF coefficients
 * (barten_watson_blend_csf_mae) at 1080p and 2160p for 3H and 5H. */
static char *test_barten_csf_blend_mae_1080p_3h_hv(void)
{
    mu_assert("v1017 csf blend scale 0 H/V 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(0, 0, 3.0, 1080), 0.011249));
    mu_assert("v1017 csf blend scale 1 H/V 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(1, 0, 3.0, 1080), 0.022606));
    mu_assert("v1017 csf blend scale 2 H/V 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(2, 0, 3.0, 1080), 0.035930));
    mu_assert("v1017 csf blend scale 3 H/V 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(3, 0, 3.0, 1080), 0.045673));
    return NULL;
}

static char *test_barten_csf_blend_mae_1080p_3h_d(void)
{
    mu_assert("v1017 csf blend scale 0 D 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(0, 1, 3.0, 1080), 0.004097));
    mu_assert("v1017 csf blend scale 1 D 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(1, 1, 3.0, 1080), 0.010921));
    mu_assert("v1017 csf blend scale 2 D 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(2, 1, 3.0, 1080), 0.021430));
    mu_assert("v1017 csf blend scale 3 D 1080p 3H",
              almost_equal(barten_watson_blend_csf_mae(3, 1, 3.0, 1080), 0.031313));
    return NULL;
}

static char *test_barten_csf_blend_mae_1080p_5h_hv(void)
{
    mu_assert("v1017 csf blend scale 0 H/V 1080p 5H",
              almost_equal(barten_watson_blend_csf_mae(0, 0, 5.0, 1080), 0.004052));
    mu_assert("v1017 csf blend scale 1 H/V 1080p 5H",
              almost_equal(barten_watson_blend_csf_mae(1, 0, 5.0, 1080), 0.013939));
    mu_assert("v1017 csf blend scale 2 H/V 1080p 5H",
              almost_equal(barten_watson_blend_csf_mae(2, 0, 5.0, 1080), 0.026298));
    mu_assert("v1017 csf blend scale 3 H/V 1080p 5H",
              almost_equal(barten_watson_blend_csf_mae(3, 0, 5.0, 1080), 0.038833));
    return NULL;
}

static char *test_barten_csf_blend_mae_1080p_5h_d(void)
{
    mu_assert("v1017 csf blend scale 0 D 1080p 5H",
              almost_equal(barten_watson_blend_csf_mae(0, 1, 5.0, 1080), 0.000927));
    mu_assert("v1017 csf blend scale 1 D 1080p 5H",
              almost_equal(barten_watson_blend_csf_mae(1, 1, 5.0, 1080), 0.005544));
    mu_assert("v1017 csf blend scale 2 D 1080p 5H",
              almost_equal(barten_watson_blend_csf_mae(2, 1, 5.0, 1080), 0.013415));
    mu_assert("v1017 csf blend scale 3 D 1080p 5H",
              almost_equal(barten_watson_blend_csf_mae(3, 1, 5.0, 1080), 0.024515));
    return NULL;
}

static char *test_barten_csf_blend_mae_2160p_3h_hv(void)
{
    mu_assert("v1017 csf blend scale 0 H/V 2160p 3H",
              almost_equal(barten_watson_blend_csf_mae(0, 0, 3.0, 2160), 0.002166));
    mu_assert("v1017 csf blend scale 1 H/V 2160p 3H",
              almost_equal(barten_watson_blend_csf_mae(1, 0, 3.0, 2160), 0.011249));
    mu_assert("v1017 csf blend scale 2 H/V 2160p 3H",
              almost_equal(barten_watson_blend_csf_mae(2, 0, 3.0, 2160), 0.022606));
    mu_assert("v1017 csf blend scale 3 H/V 2160p 3H",
              almost_equal(barten_watson_blend_csf_mae(3, 0, 3.0, 2160), 0.035930));
    return NULL;
}

static char *test_barten_csf_blend_mae_2160p_3h_d(void)
{
    mu_assert("v1017 csf blend scale 0 D 2160p 3H",
              almost_equal(barten_watson_blend_csf_mae(0, 1, 3.0, 2160), 0.000447));
    mu_assert("v1017 csf blend scale 1 D 2160p 3H",
              almost_equal(barten_watson_blend_csf_mae(1, 1, 3.0, 2160), 0.004097));
    mu_assert("v1017 csf blend scale 2 D 2160p 3H",
              almost_equal(barten_watson_blend_csf_mae(2, 1, 3.0, 2160), 0.010921));
    mu_assert("v1017 csf blend scale 3 D 2160p 3H",
              almost_equal(barten_watson_blend_csf_mae(3, 1, 3.0, 2160), 0.021430));
    return NULL;
}

static char *test_barten_csf_blend_mae_2160p_5h_hv(void)
{
    mu_assert("v1017 csf blend scale 0 H/V 2160p 5H",
              almost_equal(barten_watson_blend_csf_mae(0, 0, 5.0, 2160), 0.000077));
    mu_assert("v1017 csf blend scale 1 H/V 2160p 5H",
              almost_equal(barten_watson_blend_csf_mae(1, 0, 5.0, 2160), 0.004052));
    mu_assert("v1017 csf blend scale 2 H/V 2160p 5H",
              almost_equal(barten_watson_blend_csf_mae(2, 0, 5.0, 2160), 0.013939));
    mu_assert("v1017 csf blend scale 3 H/V 2160p 5H",
              almost_equal(barten_watson_blend_csf_mae(3, 0, 5.0, 2160), 0.026298));
    return NULL;
}

static char *test_barten_csf_blend_mae_2160p_5h_d(void)
{
    mu_assert("v1017 csf blend scale 0 D 2160p 5H",
              almost_equal(barten_watson_blend_csf_mae(0, 1, 5.0, 2160), 0.000045));
    mu_assert("v1017 csf blend scale 1 D 2160p 5H",
              almost_equal(barten_watson_blend_csf_mae(1, 1, 5.0, 2160), 0.000927));
    mu_assert("v1017 csf blend scale 2 D 2160p 5H",
              almost_equal(barten_watson_blend_csf_mae(2, 1, 5.0, 2160), 0.005544));
    mu_assert("v1017 csf blend scale 3 D 2160p 5H",
              almost_equal(barten_watson_blend_csf_mae(3, 1, 5.0, 2160), 0.013415));
    return NULL;
}

static char *run_tests_csf(void)
{
    mu_run_test(test_barten_csf_sens);
    mu_run_test(test_barten_csf_lum_high);
    mu_run_test(test_barten_csf_lum_low);
    mu_run_test(test_barten_csf_lum_mid);
    mu_run_test(test_barten_csf_interp);
    return NULL;
}

static char *run_tests_blend(void)
{
    mu_run_test(test_barten_csf_blend_1080p_hv);
    mu_run_test(test_barten_csf_blend_1080p_d);
    mu_run_test(test_barten_csf_blend_2160p_hv);
    mu_run_test(test_barten_csf_blend_2160p_d);
    mu_run_test(test_barten_csf_blend_mae_hv);
    mu_run_test(test_barten_csf_blend_mae_d);
    return NULL;
}

static char *run_tests_blend_mae_v1017_hv(void)
{
    mu_run_test(test_barten_csf_blend_mae_1080p_3h_hv);
    mu_run_test(test_barten_csf_blend_mae_1080p_5h_hv);
    mu_run_test(test_barten_csf_blend_mae_2160p_3h_hv);
    mu_run_test(test_barten_csf_blend_mae_2160p_5h_hv);
    return NULL;
}

static char *run_tests_blend_mae_v1017_d(void)
{
    mu_run_test(test_barten_csf_blend_mae_1080p_3h_d);
    mu_run_test(test_barten_csf_blend_mae_1080p_5h_d);
    mu_run_test(test_barten_csf_blend_mae_2160p_3h_d);
    mu_run_test(test_barten_csf_blend_mae_2160p_5h_d);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(run_tests_csf);
    mu_run_test(run_tests_blend);
    mu_run_test(run_tests_blend_mae_v1017_hv);
    mu_run_test(run_tests_blend_mae_v1017_d);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
