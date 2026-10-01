/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Host tail of float_motion.c::compute_motion_simd() for the GPU twins
 * (ADR-1409): feature/float_motion_sad.h must add the row sums one by one
 * into a single float and divide by the pixel count in float, exactly as the
 * CPU extractor does. Device-free.
 *
 * The fixtures put a row sum of 2^24 next to row sums of 1. A float at 2^24
 * has a spacing of 2, so `2^24 + 1` rounds back to 2^24: a running float sum
 * that meets the large row first absorbs every later 1, while a sum that is
 * widened or reordered does not. Each expected value below therefore holds
 * for the CPU's order and type only.
 */

#include <math.h>

#include "test.h"

#include "feature/float_motion_sad.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define ROWS 8u
#define BIG 16777216.0f /* 2^24 */

static void fill(float *rows, unsigned count, float value)
{
    for (unsigned i = 0; i < count; i++)
        rows[i] = value;
}

static char *test_sum_is_one_running_float(void)
{
    float rows[ROWS];
    fill(rows, ROWS, 1.0f);
    rows[0] = BIG;
    /* 2^24, then seven additions of 1 that each round back to 2^24; the
     * exact sum is 2^24 + 7. Pixel count 2 * 8. */
    const double score = vmaf_float_motion_score_from_row_sads(rows, 2u, ROWS);
    mu_assert("the rows are not added into one running float", score == (double)(BIG / 16.0f));
    return NULL;
}

static char *test_sum_runs_top_to_bottom(void)
{
    float rows[ROWS];
    fill(rows, ROWS, 1.0f);
    rows[ROWS - 1u] = BIG;
    /* Seven ones first, exactly 7, then 2^24: one rounding, to 2^24 + 8.
     * The other order stays at 2^24. */
    const double score = vmaf_float_motion_score_from_row_sads(rows, 2u, ROWS);
    mu_assert("the rows are not added top to bottom", score == (double)((BIG + 7.0f) / 16.0f));
    mu_assert("the fixture does not tell the two orders apart", BIG + 7.0f != BIG);
    return NULL;
}

static char *test_division_is_float(void)
{
    const float rows[1] = {1.0f};
    /* 1 / 3 rounded to float, then widened: not the double quotient. */
    const double score = vmaf_float_motion_score_from_row_sads(rows, 3u, 1u);
    mu_assert("the mean is not taken in float", score == (double)(1.0f / 3.0f));
    mu_assert("the fixture does not tell float from double", score != 1.0 / 3.0);
    return NULL;
}

static char *test_single_pixel(void)
{
    const float rows[1] = {0.125f};
    mu_assert("a 1x1 plane does not return its only difference",
              vmaf_float_motion_score_from_row_sads(rows, 1u, 1u) == 0.125);
    return NULL;
}

static char *test_empty_plane_is_nan(void)
{
    const float rows[1] = {1.0f};
    /* No rows: 0 / 0. The extractors refuse such a frame before they get
     * here; the helper must not report it as a score. */
    mu_assert("zero rows give a score", isnan(vmaf_float_motion_score_from_row_sads(rows, 4u, 0u)));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_sum_is_one_running_float);
    mu_run_test(test_sum_runs_top_to_bottom);
    mu_run_test(test_division_is_float);
    mu_run_test(test_single_pixel);
    mu_run_test(test_empty_plane_is_nan);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
