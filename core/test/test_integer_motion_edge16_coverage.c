/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 3 — core/src/feature/integer_motion.h gap-fill.
 *
 *  The `edge_16` static inline drives mirror-extension along the 5-tap
 *  motion blur filter for both horizontal and vertical scans.  It is
 *  reached transitively from integer_motion.c via the corner / edge
 *  paths of `motion_blur_5x5` — but the interior-frame test fixtures
 *  in test_integer_motion_coverage.c (PR #433 round 2) only graze a
 *  subset of the four mirror branches.  Drive every combination
 *  directly to push integer_motion.h line coverage from 58 % (baseline
 *  2026-05-31) towards 100 %.
 *
 *  The helper is a pure function — given the 5-tap filter
 *  {3571, 16004, 26386, 16004, 3571} and a known source pattern,
 *  the expected accumulator value is computable by hand.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/integer_motion.h"

/* ------------------------------------------------------------------ */
/* Horizontal scan                                                    */
/* ------------------------------------------------------------------ */

/* Interior column on a 7-wide / 1-tall row of {10,20,30,40,50,60,70}.
 * With i=0, j=3 and radius=2, no mirror is triggered — the filter
 * reads img[0..4] dotted with the 5-tap. Expected: */
static char *test_edge16_horizontal_interior(void)
{
    uint16_t src[7] = {10, 20, 30, 40, 50, 60, 70};
    /* j=3, k=0..4 -> j_tap = 3-2+k = 1..5 -> src[1..5] */
    uint32_t expected = 3571u * 20u + 16004u * 30u + 26386u * 40u + 16004u * 50u + 3571u * 60u;
    uint32_t got = edge_16(true, src, /*width=*/7, /*height=*/1, /*stride=*/7, 0, 3);
    mu_assert("edge_16 horizontal interior matches expected", got == expected);
    return NULL;
}

/* Left mirror: j=0 with radius=2 reaches j_tap = -2, -1, 0, 1, 2.
 * j_tap=-2 -> 2 (mirror), j_tap=-1 -> 1 (mirror). */
static char *test_edge16_horizontal_left_mirror(void)
{
    uint16_t src[7] = {100, 200, 300, 400, 500, 600, 700};
    /* j_tap sequence with mirror: 2, 1, 0, 1, 2 -> src[2], src[1], src[0], src[1], src[2] */
    uint32_t expected = 3571u * 300u + 16004u * 200u + 26386u * 100u + 16004u * 200u + 3571u * 300u;
    uint32_t got = edge_16(true, src, 7, 1, 7, 0, 0);
    mu_assert("edge_16 horizontal left mirror matches", got == expected);
    return NULL;
}

/* Right mirror: with width=5 and j=4, radius=2 -> j_tap = 2,3,4,5,6.
 * j_tap=5: 5 >= 5 -> width - (5 - 5 + 2) = 5 - 2 = 3.
 * j_tap=6: 6 >= 5 -> width - (6 - 5 + 2) = 5 - 3 = 2. */
static char *test_edge16_horizontal_right_mirror(void)
{
    uint16_t src[5] = {1, 2, 3, 4, 5};
    /* indices: 2, 3, 4, 3, 2 -> src[2..4] folded back */
    uint32_t expected = 3571u * 3u + 16004u * 4u + 26386u * 5u + 16004u * 4u + 3571u * 3u;
    uint32_t got = edge_16(true, src, 5, 1, 5, 0, 4);
    mu_assert("edge_16 horizontal right mirror matches", got == expected);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Vertical scan                                                      */
/* ------------------------------------------------------------------ */

/* Vertical pass on a 1-wide / 7-tall column. With horizontal=false the
 * helper indexes (i - radius + k) along the row dimension. Top edge
 * (i=0) mirrors negative i_tap to its absolute value. */
static char *test_edge16_vertical_top_mirror(void)
{
    /* stride=1 to keep indexing trivial; arrange values 10..70 down
     * the column. */
    uint16_t src[7] = {10, 20, 30, 40, 50, 60, 70};
    /* i_tap sequence with mirror at i=0: -2, -1, 0, 1, 2 -> 2, 1, 0, 1, 2.
     * src[i_tap*stride+j] with stride=1, j=0 -> src[2], src[1], src[0], src[1], src[2]. */
    uint32_t expected = 3571u * 30u + 16004u * 20u + 26386u * 10u + 16004u * 20u + 3571u * 30u;
    uint32_t got = edge_16(false, src, /*width=*/1, /*height=*/7, /*stride=*/1, 0, 0);
    mu_assert("edge_16 vertical top mirror matches", got == expected);
    return NULL;
}

/* Vertical bottom mirror: with height=5 and i=4, radius=2 ->
 * i_tap = 2, 3, 4, 5, 6. i_tap=5 -> height - (5 - 5 + 2) = 3.
 * i_tap=6 -> height - (6 - 5 + 2) = 2. */
static char *test_edge16_vertical_bottom_mirror(void)
{
    uint16_t src[5] = {7, 11, 13, 17, 19};
    /* indices: 2, 3, 4, 3, 2 -> 13, 17, 19, 17, 13 */
    uint32_t expected = 3571u * 13u + 16004u * 17u + 26386u * 19u + 16004u * 17u + 3571u * 13u;
    uint32_t got = edge_16(false, src, 1, 5, 1, 4, 0);
    mu_assert("edge_16 vertical bottom mirror matches", got == expected);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_edge16_horizontal_interior);
    mu_run_test(test_edge16_horizontal_left_mirror);
    mu_run_test(test_edge16_horizontal_right_mirror);
    mu_run_test(test_edge16_vertical_top_mirror);
    mu_run_test(test_edge16_vertical_bottom_mirror);
    return NULL;
}
