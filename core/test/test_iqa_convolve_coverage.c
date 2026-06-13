/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 2 — core/src/feature/iqa/convolve.c gap-fill.
 *
 *  test_iqa_convolve.c exercises the IQA_CONVOLVE_1D fast path
 *  against SIMD. The boundary-extension helpers and the 2-D-only
 *  surface stayed at 41 %. This file plugs:
 *
 *    1. KBND_SYMMETRIC mirror across both axes and the wrap-around
 *       (line 40-69).
 *    2. KBND_REPLICATE clamp branches on x/y under-/over-flow (line
 *       71-83).
 *    3. KBND_CONSTANT under-flow + corner OOB returning the
 *       constant (line 85-94).
 *    4. iqa_img_filter() null-kernel rejection (line 239-240).
 *    5. iqa_img_filter() owns-its-buffer path (line 242-246, 257-266).
 *    6. iqa_filter_pixel() with NULL kernel (line 273-274).
 *    7. iqa_filter_pixel() edge / non-edge dispatch (line 285-300).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/iqa/convolve.h"

/* ----------------------------------------------------------------- */
/* Boundary helpers                                                  */
/* ----------------------------------------------------------------- */

static char *test_kbnd_symmetric_in_range(void)
{
    float img[9] = {0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
    /* (0,0) is in range — identity. */
    mu_assert("KBND_SYMMETRIC in-range identity", KBND_SYMMETRIC(img, 3, 3, 0, 0, 0.0f) == 0.f);
    mu_assert("KBND_SYMMETRIC center", KBND_SYMMETRIC(img, 3, 3, 1, 1, 0.0f) == 4.f);
    return NULL;
}

static char *test_kbnd_symmetric_negative(void)
{
    float img[9] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f};
    /* x = -1, y = 0 → mirror to (0,0) */
    mu_assert("KBND_SYMMETRIC x=-1 reflects to x=0", KBND_SYMMETRIC(img, 3, 3, -1, 0, 0.0f) == 1.f);
    /* y = -1 → mirror to y = 0 */
    mu_assert("KBND_SYMMETRIC y=-1 reflects to y=0", KBND_SYMMETRIC(img, 3, 3, 0, -1, 0.0f) == 1.f);
    return NULL;
}

static char *test_kbnd_symmetric_overflow(void)
{
    float img[9] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f};
    /* x = 3 (out of 0..2) → reflect to x = 2 */
    mu_assert("KBND_SYMMETRIC x=3 reflects to x=2", KBND_SYMMETRIC(img, 3, 3, 3, 1, 0.0f) == 6.f);
    /* y = 3 → reflect to y = 2 */
    mu_assert("KBND_SYMMETRIC y=3 reflects to y=2", KBND_SYMMETRIC(img, 3, 3, 1, 3, 0.0f) == 8.f);
    return NULL;
}

static char *test_kbnd_replicate_clamp(void)
{
    float img[9] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f};
    mu_assert("KBND_REPLICATE x<0 → x=0", KBND_REPLICATE(img, 3, 3, -1, 1, 0.0f) == 4.f);
    mu_assert("KBND_REPLICATE x>w → x=w-1", KBND_REPLICATE(img, 3, 3, 5, 1, 0.0f) == 6.f);
    mu_assert("KBND_REPLICATE y<0 → y=0", KBND_REPLICATE(img, 3, 3, 1, -1, 0.0f) == 2.f);
    mu_assert("KBND_REPLICATE y>h → y=h-1", KBND_REPLICATE(img, 3, 3, 1, 5, 0.0f) == 8.f);
    return NULL;
}

static char *test_kbnd_constant_oob(void)
{
    float img[9] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f};
    /* x or y >= w/h → return bnd_const. */
    mu_assert("KBND_CONSTANT x>=w returns constant",
              KBND_CONSTANT(img, 3, 3, 7, 1, 42.0f) == 42.0f);
    mu_assert("KBND_CONSTANT y>=h returns constant",
              KBND_CONSTANT(img, 3, 3, 1, 7, 99.0f) == 99.0f);
    /* x or y < 0 → clamped to 0 (no constant fallthrough). */
    mu_assert("KBND_CONSTANT x<0 → x=0", KBND_CONSTANT(img, 3, 3, -2, 1, 7.0f) == 4.f);
    mu_assert("KBND_CONSTANT y<0 → y=0", KBND_CONSTANT(img, 3, 3, 1, -2, 7.0f) == 2.f);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* iqa_img_filter — null kernel / owns-its-buffer paths              */
/* ----------------------------------------------------------------- */

static char *test_iqa_img_filter_null_kernel(void)
{
    float img[9] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f};
    /* k == NULL → must return non-zero */
    int rc = iqa_img_filter(img, 3, 3, NULL, NULL);
    mu_assert("iqa_img_filter NULL kernel → non-zero", rc != 0);
    return NULL;
}

static char *test_iqa_img_filter_owns_buffer(void)
{
    /* 3x3 identity kernel (odd dims). */
    float k_vals[9] = {0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f};
    struct iqa_kernel k;
    k.kernel = k_vals;
    k.kernel_h = NULL;
    k.kernel_v = NULL;
    k.w = 3;
    k.h = 3;
    k.normalized = 1;
    k.bnd_opt = KBND_REPLICATE;
    k.bnd_const = 0.0f;

    /* When result == NULL, iqa_img_filter mallocs its own dst and
     * copies the result back into img in place. */
    float img[9] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f};
    int rc = iqa_img_filter(img, 3, 3, &k, NULL);
    mu_assert("iqa_img_filter owns_buffer ok", rc == 0);
    /* Identity kernel — img unchanged (assert one interior cell). */
    mu_assert("identity convolve preserves center", img[4] == 5.f);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* iqa_filter_pixel — null kernel + edge dispatch                    */
/* ----------------------------------------------------------------- */

static char *test_iqa_filter_pixel_null_kernel(void)
{
    float img[9] = {0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
    float v = iqa_filter_pixel(img, 3, 3, 1, 1, NULL, 1.0f);
    mu_assert("filter_pixel NULL kernel returns raw pixel", v == 4.f);
    return NULL;
}

static char *test_iqa_filter_pixel_edge_branch(void)
{
    /* 5x5 image, 3x3 kernel — (0,0) hits the edge branch (line 286). */
    float img[25] = {1.f, 2.f, 3.f, 4.f, 5.f, 1.f, 2.f, 3.f, 4.f, 5.f, 1.f, 2.f, 3.f,
                     4.f, 5.f, 1.f, 2.f, 3.f, 4.f, 5.f, 1.f, 2.f, 3.f, 4.f, 5.f};
    float k_vals[9] = {0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f};
    struct iqa_kernel k;
    k.kernel = k_vals;
    k.kernel_h = NULL;
    k.kernel_v = NULL;
    k.w = 3;
    k.h = 3;
    k.normalized = 1;
    k.bnd_opt = KBND_REPLICATE;
    k.bnd_const = 0.0f;

    /* Edge case: identity kernel at (0,0) is still img[0] = 1. */
    float edge = iqa_filter_pixel(img, 5, 5, 0, 0, &k, 1.0f);
    mu_assert("filter_pixel edge identity at (0,0) returns 1", edge == 1.f);

    /* Inner case: identity at center (2,2) returns the center pixel. */
    float inner = iqa_filter_pixel(img, 5, 5, 2, 2, &k, 1.0f);
    mu_assert("filter_pixel inner identity at (2,2) returns 3", inner == 3.f);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_kbnd_symmetric_in_range);
    mu_run_test(test_kbnd_symmetric_negative);
    mu_run_test(test_kbnd_symmetric_overflow);
    mu_run_test(test_kbnd_replicate_clamp);
    mu_run_test(test_kbnd_constant_oob);
    mu_run_test(test_iqa_img_filter_null_kernel);
    mu_run_test(test_iqa_img_filter_owns_buffer);
    mu_run_test(test_iqa_filter_pixel_null_kernel);
    mu_run_test(test_iqa_filter_pixel_edge_branch);
    return NULL;
}
