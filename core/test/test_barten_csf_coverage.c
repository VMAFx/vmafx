/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 2 — barten_csf_tools.h gap-fill.
 *
 *  test_barten_csf.c covers the 1080p / 3H legacy curve. The header's
 *  resolution-dispatching helpers (barten_watson_blend_csf,
 *  barten_watson_blend_csf_mae) gate on 4 resolutions x 2 viewing
 *  distances = 8 paths each, plus the default "unsupported"
 *  fallthrough that returns -EINVAL.  Drives all branches at lines
 *  218-233 and 246-261 of core/src/feature/barten_csf_tools.h.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "test.h"

#include "feature/barten_csf_tools.h"

static int isclose(float a, float b)
{
    const float diff = a > b ? a - b : b - a;
    return diff < 1e-5f;
}

/* ----------------------------------------------------------------- */
/* Legacy barten_watson_blend_csf — non-1080p resolutions             */
/* ----------------------------------------------------------------- */

static char *test_blend_legacy_1080_5h(void)
{
    mu_assert("1080p 5H H/V scale0", isclose(barten_watson_blend_csf(0, 0, 5.0, 1080), 0.004212f));
    mu_assert("1080p 5H D scale3", isclose(barten_watson_blend_csf(3, 1, 5.0, 1080), 0.027574f));
    return NULL;
}

static char *test_blend_legacy_2160(void)
{
    mu_assert("2160p 3H H/V scale1", isclose(barten_watson_blend_csf(1, 0, 3.0, 2160), 0.01183f));
    mu_assert("2160p 5H D scale2", isclose(barten_watson_blend_csf(2, 1, 5.0, 2160), 0.005852f));
    return NULL;
}

static char *test_blend_legacy_720(void)
{
    mu_assert("720p 3H D scale0", isclose(barten_watson_blend_csf(0, 1, 3.0, 720), 0.007999f));
    mu_assert("720p 5H H/V scale2", isclose(barten_watson_blend_csf(2, 0, 5.0, 720), 0.040309f));
    return NULL;
}

static char *test_blend_legacy_480(void)
{
    mu_assert("480p 3H H/V scale1", isclose(barten_watson_blend_csf(1, 0, 3.0, 480), 0.045875f));
    mu_assert("480p 5H D scale3", isclose(barten_watson_blend_csf(3, 1, 5.0, 480), 0.037777f));
    return NULL;
}

static char *test_blend_legacy_unsupported_returns_einval(void)
{
    float v = barten_watson_blend_csf(0, 0, 3.0, 999);
    mu_assert("legacy unsupported resolution returns -EINVAL", (int)v == -EINVAL);
    v = barten_watson_blend_csf(0, 0, 9.0, 1080);
    mu_assert("legacy unsupported distance returns -EINVAL", (int)v == -EINVAL);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* MAE (v1.0.17+) variant — exercise every supported (res, dist) pair */
/* ----------------------------------------------------------------- */

static char *test_blend_mae_1080(void)
{
    mu_assert("1080p 3H MAE H/V scale0",
              isclose(barten_watson_blend_csf_mae(0, 0, 3.0, 1080), 0.011249f));
    mu_assert("1080p 5H MAE D scale3",
              isclose(barten_watson_blend_csf_mae(3, 1, 5.0, 1080), 0.024515f));
    return NULL;
}

static char *test_blend_mae_2160(void)
{
    mu_assert("2160p 3H MAE D scale1",
              isclose(barten_watson_blend_csf_mae(1, 1, 3.0, 2160), 0.004097f));
    mu_assert("2160p 5H MAE H/V scale2",
              isclose(barten_watson_blend_csf_mae(2, 0, 5.0, 2160), 0.013939f));
    return NULL;
}

static char *test_blend_mae_720(void)
{
    mu_assert("720p 3H MAE H/V scale0",
              isclose(barten_watson_blend_csf_mae(0, 0, 3.0, 720), 0.017329f));
    mu_assert("720p 5H MAE D scale1",
              isclose(barten_watson_blend_csf_mae(1, 1, 5.0, 720), 0.009579f));
    return NULL;
}

static char *test_blend_mae_480(void)
{
    mu_assert("480p 3H MAE H/V scale2",
              isclose(barten_watson_blend_csf_mae(2, 0, 3.0, 480), 0.046676f));
    mu_assert("480p 5H MAE D scale0",
              isclose(barten_watson_blend_csf_mae(0, 1, 5.0, 480), 0.006514f));
    return NULL;
}

static char *test_blend_mae_unsupported_returns_einval(void)
{
    float v = barten_watson_blend_csf_mae(0, 0, 3.0, 999);
    mu_assert("MAE unsupported resolution returns -EINVAL", (int)v == -EINVAL);
    v = barten_watson_blend_csf_mae(0, 0, 9.0, 1080);
    mu_assert("MAE unsupported distance returns -EINVAL", (int)v == -EINVAL);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* barten_csf — extra spatial-frequency clamps to drive the interp   */
/* selector branch (line 101-113).                                   */
/* ----------------------------------------------------------------- */

static char *test_barten_csf_high_scale_and_low_lum(void)
{
    /* Drive a scale far from those in the existing test (5) and a
     * non-1080p reference height so adm_csf_scale flows through. */
    float v = barten_csf(5, 5.0, 720, 0.5, 1.0);
    mu_assert("barten_csf 720p 5H scale5 lum0.5 finite", isfinite(v));
    return NULL;
}

/* Cover the linear-interp slice between every anchor pair, ensuring the
 * `while (i < 5)` selector iterates each successive pair (line 109-117
 * of barten_csf_tools.h). */
static char *test_barten_csf_every_anchor_pair(void)
{
    /* Anchors are {0.002, 0.02, 0.2, 2, 20, 150} — pick a mid-point
     * in each segment to drive distinct left_lum_index/right_lum_index. */
    const float pairs[5] = {0.005f, 0.05f, 0.5f, 5.0f, 50.0f};
    for (unsigned i = 0; i < 5u; ++i) {
        float v = barten_csf(2, 3.0, 1080, pairs[i], 1.0);
        mu_assert("barten_csf interp anchor pair finite", isfinite(v));
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_blend_legacy_1080_5h);
    mu_run_test(test_blend_legacy_2160);
    mu_run_test(test_blend_legacy_720);
    mu_run_test(test_blend_legacy_480);
    mu_run_test(test_blend_legacy_unsupported_returns_einval);
    mu_run_test(test_blend_mae_1080);
    mu_run_test(test_blend_mae_2160);
    mu_run_test(test_blend_mae_720);
    mu_run_test(test_blend_mae_480);
    mu_run_test(test_blend_mae_unsupported_returns_einval);
    mu_run_test(test_barten_csf_high_scale_and_low_lum);
    mu_run_test(test_barten_csf_every_anchor_pair);
    return NULL;
}
