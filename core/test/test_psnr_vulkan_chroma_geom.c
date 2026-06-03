/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Pure-C geometry unit test for the Vulkan PSNR chroma plane dimension
 *  computation.  Does NOT require a Vulkan device — exercises only the
 *  ceiling-division formula from psnr_vulkan.c::init() in isolation.
 *
 *  Rationale (Research-0094; dedup-audit-c-feature-twins-2026-05-16
 *  finding #5): for odd-dimension YUV420 inputs the correct chroma width
 *  is ceil(w/2) = (w + 1) >> 1, not floor(w/2) = w >> 1.  A floor
 *  formula underestimates by 1 pixel, producing a silently wrong sample
 *  count and diverging PSNR scores against the CPU / CUDA twins at
 *  places=4.
 *
 *  This test pins the expected chroma geometry for two odd-dimension
 *  pairs (1921x1081 and 999x540) and one even-dimension control
 *  (1920x1080) to prevent future regressions.
 */

#include "test.h"

/* Mirror the ceiling formula verbatim from psnr_vulkan.c::init():
 *   const unsigned cw = (w + (unsigned)ss_hor) >> ss_hor;
 *   const unsigned ch = (h + (unsigned)ss_ver) >> ss_ver;
 *
 * ss_hor / ss_ver are 0 or 1 depending on pix_fmt:
 *   YUV420: ss_hor=1, ss_ver=1
 *   YUV422: ss_hor=1, ss_ver=0
 *   YUV444: ss_hor=0, ss_ver=0  */
static unsigned chroma_dim_ceil(unsigned full, int ss)
{
    return (full + (unsigned)ss) >> ss;
}

static char *test_odd_yuv420_chroma_dims(void)
{
    /* 1921x1081 YUV420 — canonical odd-dimension case from the audit. */
    {
        const unsigned w = 1921U, h = 1081U;
        const unsigned cw = chroma_dim_ceil(w, 1);
        const unsigned ch = chroma_dim_ceil(h, 1);
        /* ceil(1921/2) = 961; floor would give 960 */
        mu_assert("1921 chroma width: ceiling is 961", cw == 961U);
        /* ceil(1081/2) = 541; floor would give 540 */
        mu_assert("1081 chroma height: ceiling is 541", ch == 541U);
    }

    /* 999x540 YUV420 — second odd-dimension case. */
    {
        const unsigned w = 999U, h = 540U;
        const unsigned cw = chroma_dim_ceil(w, 1);
        const unsigned ch = chroma_dim_ceil(h, 1);
        /* ceil(999/2) = 500; floor would give 499 */
        mu_assert("999 chroma width: ceiling is 500", cw == 500U);
        /* 540 is even: ceil(540/2) == floor(540/2) == 270 */
        mu_assert("540 chroma height: 270", ch == 270U);
    }

    return NULL;
}

static char *test_even_yuv420_chroma_dims(void)
{
    /* 1920x1080 YUV420 — control: ceiling == floor for even inputs. */
    const unsigned w = 1920U, h = 1080U;
    const unsigned cw = chroma_dim_ceil(w, 1);
    const unsigned ch = chroma_dim_ceil(h, 1);
    mu_assert("1920 chroma width: 960", cw == 960U);
    mu_assert("1080 chroma height: 540", ch == 540U);
    return NULL;
}

static char *test_yuv422_chroma_dims(void)
{
    /* YUV422: ss_hor=1, ss_ver=0 — odd-width gets ceiling; height unchanged. */
    const unsigned w = 1921U, h = 1081U;
    const unsigned cw = chroma_dim_ceil(w, 1);
    const unsigned ch = chroma_dim_ceil(h, 0);
    mu_assert("1921 YUV422 chroma width: ceiling is 961", cw == 961U);
    mu_assert("YUV422 chroma height equals luma height", ch == h);
    return NULL;
}

static char *test_yuv444_chroma_dims(void)
{
    /* YUV444: ss_hor=0, ss_ver=0 — chroma == luma for all dimensions. */
    const unsigned w = 1921U, h = 1081U;
    const unsigned cw = chroma_dim_ceil(w, 0);
    const unsigned ch = chroma_dim_ceil(h, 0);
    mu_assert("YUV444 chroma width equals luma width", cw == w);
    mu_assert("YUV444 chroma height equals luma height", ch == h);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_odd_yuv420_chroma_dims);
    mu_run_test(test_even_yuv420_chroma_dims);
    mu_run_test(test_yuv422_chroma_dims);
    mu_run_test(test_yuv444_chroma_dims);
    return NULL;
}
