/**
 *
 *  Copyright 2026 Lusoris
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

/*
 * Coverage-targeted unit tests for the vendored IQA helpers
 * (libvmaf_feature/iqa) — math_utils, decimate, the three KBND_*
 * border-handling functions in convolve.c, iqa_filter_pixel /
 * iqa_img_filter, and an end-to-end iqa_ssim drive that exercises
 * the scalar fallback paths of ssim_tools.c (precompute, variance,
 * accumulate).
 *
 * The vendored IQA tree is BSD-licensed code from tdistler.com plus a
 * 2016 Netflix update. This test file does NOT modify any vendored
 * source — every assertion is observation-only on the existing public
 * API (math_utils.h, convolve.h, decimate.h, ssim_tools.h). See
 * docs/adr/0889-libsvm-vendored-audit.md for the same observation-only
 * pattern applied to svm.cpp.
 *
 * The existing test_iqa_convolve.c is conditional on SIMD support and
 * only exercises iqa_convolve under IQA_CONVOLVE_1D bit-exactness vs.
 * AVX2 / AVX-512 / NEON. This file complements it by driving the
 * scalar-only helpers + the iqa_ssim entry point, which is unreachable
 * from the SIMD-vs-scalar diff. Combined, the two files lift
 * libvmaf/feature/iqa coverage from baseline 16.7% lines / 15.4%
 * functions to a substantially higher floor.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "feature/iqa/convolve.h"
#include "feature/iqa/decimate.h"
#include "feature/iqa/math_utils.h"
#include "feature/iqa/ssim_tools.h"
#include "test.h"

/* ------------------------------------------------------------------ */
/*  math_utils.c                                                       */
/* ------------------------------------------------------------------ */

static char *test_math_round_positive_half(void)
{
    mu_assert("round(0.5) == 1", _round(0.5f) == 1);
    mu_assert("round(1.5) == 2", _round(1.5f) == 2);
    mu_assert("round(0.49) == 0", _round(0.49f) == 0);
    mu_assert("round(0.0) == 0", _round(0.0f) == 0);
    return NULL;
}

static char *test_math_round_negative(void)
{
    /* The vendored _round() is "truncate toward zero, then add sign
     * when |frac| >= 0.5". For negative inputs that means:
     *   -0.5 -> trunc=0, (0 - 0 = 0) < 0.5 -> 0
     *   -1.5 -> trunc=-1, (-1.5 - -1 = -0.5) < 0.5 -> -1
     *   -1.7 -> trunc=-1, (-1.7 - -1 = -0.7) < 0.5 -> -1
     * The function is asymmetric — positive .5 rounds up, negative .5
     * rounds toward zero. This test locks the observed behaviour in
     * so any accidental rewrite of the rounding rule surfaces. */
    mu_assert("round(-0.5) == 0", _round(-0.5f) == 0);
    mu_assert("round(-1.5) == -1", _round(-1.5f) == -1);
    mu_assert("round(-0.49) == 0", _round(-0.49f) == 0);
    mu_assert("round(-1.7) == -1", _round(-1.7f) == -1);
    return NULL;
}

static char *test_math_min_max(void)
{
    mu_assert("max(3, 5) == 5", _max(3, 5) == 5);
    mu_assert("max(5, 3) == 5", _max(5, 3) == 5);
    mu_assert("max(-1, -2) == -1", _max(-1, -2) == -1);
    mu_assert("min(3, 5) == 3", _min(3, 5) == 3);
    mu_assert("min(5, 3) == 3", _min(5, 3) == 3);
    mu_assert("min(-1, -2) == -2", _min(-1, -2) == -2);
    return NULL;
}

static char *test_math_cmp_float(void)
{
    /* _cmp_float scales by 10^digits, then applies the same
     * asymmetric "trunc + sign-add when frac>=0.5" rounding as
     * _round() above — and compares the two integer results. So
     * 1.2 vs 1.21 at digits=1 -> both scale to 12.x with frac<0.5
     * -> both truncate to 12, equal. */
    mu_assert("1.2 == 1.21 @ digits=1", _cmp_float(1.2f, 1.21f, 1) == 0);
    /* But 1.2 vs 1.3 at digits=1 -> 12 vs 13, differ. */
    mu_assert("1.2 != 1.3 @ digits=1", _cmp_float(1.2f, 1.3f, 1) == 1);
    /* And the bumped-by-rounding case: 1.234 vs 1.235 at digits=2
     * -> 123.4 (trunc 123) vs 123.5 (trunc 123 + sign 1 = 124). */
    mu_assert("1.234 != 1.235 @ digits=2", _cmp_float(1.234f, 1.235f, 2) == 1);
    /* Negative vs positive — small magnitudes collapse to 0. */
    mu_assert("-0.001 == 0.001 @ digits=1", _cmp_float(-0.001f, 0.001f, 1) == 0);
    /* Floating-point matrix equality at zero precision (everything maps to 0). */
    mu_assert("0.1 == 0.4 @ digits=0", _cmp_float(0.1f, 0.4f, 0) == 0);
    return NULL;
}

static char *test_math_matrix_cmp(void)
{
    const float a[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const float b_same[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const float b_diff[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.5f, 6.0f};
    mu_assert("matrix_cmp identical -> 0", _matrix_cmp(a, b_same, 3, 2, 4) == 0);
    mu_assert("matrix_cmp differs at idx 4 -> 1", _matrix_cmp(a, b_diff, 3, 2, 1) == 1);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  convolve.c — KBND_* border handlers                                */
/* ------------------------------------------------------------------ */

/* 4x3 image:
 *   00 01 02 03
 *   10 11 12 13
 *   20 21 22 23
 */
static const float kbnd_img[12] = {0.0f,  1.0f,  2.0f,  3.0f,  10.0f, 11.0f,
                                   12.0f, 13.0f, 20.0f, 21.0f, 22.0f, 23.0f};

static char *test_kbnd_symmetric_in_bounds(void)
{
    /* In-bounds lookup returns the raw pixel. */
    mu_assert("symm in (2,1)", KBND_SYMMETRIC(kbnd_img, 4, 3, 2, 1, 0.0f) == 12.0f);
    mu_assert("symm in (0,0)", KBND_SYMMETRIC(kbnd_img, 4, 3, 0, 0, 0.0f) == 0.0f);
    return NULL;
}

static char *test_kbnd_symmetric_negative_reflect(void)
{
    /* x=-1 reflects to x=0 (period-based symmetric extension).
     * y=-1 reflects to y=0. So (-1, -1) -> (0, 0) = 0.0f. */
    mu_assert("symm (-1,-1)", KBND_SYMMETRIC(kbnd_img, 4, 3, -1, -1, 0.0f) == 0.0f);
    /* x=-2 -> 1 (px=8, rx=-2%8 + 8 = 6 -> px-6-1=1).  y=0 -> 0.  So (-2,0)=01=1.0 */
    mu_assert("symm (-2,0)", KBND_SYMMETRIC(kbnd_img, 4, 3, -2, 0, 0.0f) == 1.0f);
    return NULL;
}

static char *test_kbnd_symmetric_positive_reflect(void)
{
    /* x=w(4) reflects: rx=4%8=4, rx>=w -> rx=px-rx-1=8-4-1=3. y=0. -> img[0*4+3]=3 */
    mu_assert("symm (4,0)", KBND_SYMMETRIC(kbnd_img, 4, 3, 4, 0, 0.0f) == 3.0f);
    /* y=h(3): py=6, ry=3%6=3, ry>=h -> ry=6-3-1=2. x=0 -> img[2*4+0]=20 */
    mu_assert("symm (0,3)", KBND_SYMMETRIC(kbnd_img, 4, 3, 0, 3, 0.0f) == 20.0f);
    return NULL;
}

static char *test_kbnd_replicate(void)
{
    mu_assert("repl in (2,1)", KBND_REPLICATE(kbnd_img, 4, 3, 2, 1, 0.0f) == 12.0f);
    mu_assert("repl (-1,-1) -> (0,0)", KBND_REPLICATE(kbnd_img, 4, 3, -1, -1, 0.0f) == 0.0f);
    mu_assert("repl (5,1) -> (3,1)=13", KBND_REPLICATE(kbnd_img, 4, 3, 5, 1, 0.0f) == 13.0f);
    mu_assert("repl (1,5) -> (1,2)=21", KBND_REPLICATE(kbnd_img, 4, 3, 1, 5, 0.0f) == 21.0f);
    return NULL;
}

static char *test_kbnd_constant(void)
{
    mu_assert("const in (2,1)", KBND_CONSTANT(kbnd_img, 4, 3, 2, 1, 99.0f) == 12.0f);
    /* x<0 clamps to 0; y<0 clamps to 0; then in-bounds returns img[0]=0. */
    mu_assert("const (-1,-1) -> (0,0)=0", KBND_CONSTANT(kbnd_img, 4, 3, -1, -1, 99.0f) == 0.0f);
    /* x>=w returns bnd_const. */
    mu_assert("const (4,1) -> bnd", KBND_CONSTANT(kbnd_img, 4, 3, 4, 1, 99.0f) == 99.0f);
    mu_assert("const (1,3) -> bnd", KBND_CONSTANT(kbnd_img, 4, 3, 1, 3, 99.0f) == 99.0f);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  convolve.c — iqa_filter_pixel + iqa_img_filter                     */
/* ------------------------------------------------------------------ */

/* A 3x3 box-blur kernel (1/9 each) — applied to a 4x4 image. */
static float kernel3x3_data[9] = {1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f,
                                  1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f, 1.0f / 9.0f};

static char *test_iqa_filter_pixel_null_kernel(void)
{
    /* A NULL kernel returns the raw pixel value. */
    float v = iqa_filter_pixel(kbnd_img, 4, 3, 2, 1, NULL, 1.0f);
    mu_assert("null kernel passes pixel through", v == 12.0f);
    return NULL;
}

static char *test_iqa_filter_pixel_interior(void)
{
    struct iqa_kernel k = {0};
    k.kernel = kernel3x3_data;
    k.w = 3;
    k.h = 3;
    k.normalized = 1;
    k.bnd_opt = KBND_REPLICATE;
    /* Interior pixel (1,1): average of the 3x3 box centred there. */
    const float img4x4[16] = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,  8.0f,
                              9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};
    float v = iqa_filter_pixel(img4x4, 4, 4, 1, 1, &k, 1.0f);
    /* Mean of {1,2,3,5,6,7,9,10,11} = 54/9 = 6.0 */
    mu_assert("interior 3x3 box -> 6.0", fabsf(v - 6.0f) < 1e-5f);
    return NULL;
}

static char *test_iqa_filter_pixel_edge_replicate(void)
{
    struct iqa_kernel k = {0};
    k.kernel = kernel3x3_data;
    k.w = 3;
    k.h = 3;
    k.normalized = 1;
    k.bnd_opt = KBND_REPLICATE;
    /* Corner pixel (0,0) — uses bnd_opt for the off-edge taps. */
    const float img4x4[16] = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,  8.0f,
                              9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};
    float v = iqa_filter_pixel(img4x4, 4, 4, 0, 0, &k, 1.0f);
    /* With REPLICATE the 3x3 box around (0,0) reads pixels:
     *   (-1,-1)=(0,0)=1  (0,-1)=(0,0)=1  (1,-1)=(1,0)=2
     *   (-1, 0)=(0,0)=1  (0, 0)=1        (1, 0)=2
     *   (-1, 1)=(0,1)=5  (0, 1)=5        (1, 1)=6
     * Mean = (1+1+2+1+1+2+5+5+6)/9 = 24/9 ≈ 2.6667 */
    mu_assert("corner replicate -> 24/9", fabsf(v - (24.0f / 9.0f)) < 1e-5f);
    return NULL;
}

static char *test_iqa_img_filter_writes_result(void)
{
    struct iqa_kernel k = {0};
    k.kernel = kernel3x3_data;
    k.w = 3;
    k.h = 3;
    k.normalized = 1;
    k.bnd_opt = KBND_REPLICATE;
    const float img4x4[16] = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,  8.0f,
                              9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};
    float dst[16] = {0};
    int rc = iqa_img_filter((float *)img4x4, 4, 4, &k, dst);
    mu_assert("iqa_img_filter returns 0 on success", rc == 0);
    /* Interior pixel (1,1) -> 6.0 as in test_iqa_filter_pixel_interior. */
    mu_assert("dst(1,1) == 6.0", fabsf(dst[1 * 4 + 1] - 6.0f) < 1e-5f);
    return NULL;
}

static char *test_iqa_img_filter_reject_no_bnd_opt(void)
{
    /* k->bnd_opt == NULL must be rejected with rc=1 — guards against
     * the NULL-dereference that iqa_filter_pixel would otherwise hit
     * at the first edge pixel. */
    struct iqa_kernel k = {0};
    k.kernel = kernel3x3_data;
    k.w = 3;
    k.h = 3;
    k.normalized = 1;
    k.bnd_opt = NULL;
    float img[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float dst[4] = {0};
    int rc = iqa_img_filter(img, 2, 2, &k, dst);
    mu_assert("no bnd_opt -> rc=1", rc == 1);
    /* And a NULL kernel triggers the same rc=1 path. */
    mu_assert("NULL kernel -> rc=1", iqa_img_filter(img, 2, 2, NULL, dst) == 1);
    return NULL;
}

static char *test_iqa_img_filter_inplace(void)
{
    /* When result==NULL the result is copied back into `img`. */
    struct iqa_kernel k = {0};
    k.kernel = kernel3x3_data;
    k.w = 3;
    k.h = 3;
    k.normalized = 1;
    k.bnd_opt = KBND_REPLICATE;
    float img[16] = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,  8.0f,
                     9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};
    int rc = iqa_img_filter(img, 4, 4, &k, NULL);
    mu_assert("inplace returns 0", rc == 0);
    /* Interior pixel (1,1) -> 6.0; verified copied back into img. */
    mu_assert("inplace dst(1,1) == 6.0", fabsf(img[1 * 4 + 1] - 6.0f) < 1e-5f);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  decimate.c                                                         */
/* ------------------------------------------------------------------ */

static char *test_decimate_factor2_no_kernel(void)
{
    /* 4x4 -> 2x2 with factor=2, no smoothing kernel (k==NULL means
     * iqa_filter_pixel returns the raw pixel at (x*factor, y*factor)). */
    float img[16] = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,  8.0f,
                     9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};
    float dst[4] = {0};
    int rw = 0;
    int rh = 0;
    int rc = iqa_decimate(img, 4, 4, 2, NULL, dst, &rw, &rh);
    mu_assert("decimate returns 0", rc == 0);
    mu_assert("rw=2", rw == 2);
    mu_assert("rh=2", rh == 2);
    mu_assert("dst[0]=img(0,0)=1", dst[0] == 1.0f);
    mu_assert("dst[1]=img(2,0)=3", dst[1] == 3.0f);
    mu_assert("dst[2]=img(0,2)=9", dst[2] == 9.0f);
    mu_assert("dst[3]=img(2,2)=11", dst[3] == 11.0f);
    return NULL;
}

static char *test_decimate_inplace(void)
{
    /* result==NULL means write back into img. */
    float img[16] = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,  8.0f,
                     9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};
    int rc = iqa_decimate(img, 4, 4, 2, NULL, NULL, NULL, NULL);
    mu_assert("decimate inplace returns 0", rc == 0);
    mu_assert("img[0] preserved (0,0)=1", img[0] == 1.0f);
    mu_assert("img[1] now img(2,0)=3", img[1] == 3.0f);
    return NULL;
}

static char *test_decimate_odd_dimension(void)
{
    /* 5x5 with factor=2: sw = 5/2 + (5&1) = 2 + 1 = 3. */
    float img[25];
    for (int i = 0; i < 25; ++i) {
        img[i] = (float)i;
    }
    float dst[9] = {0};
    int rw = 0;
    int rh = 0;
    int rc = iqa_decimate(img, 5, 5, 2, NULL, dst, &rw, &rh);
    mu_assert("decimate odd returns 0", rc == 0);
    mu_assert("rw=3", rw == 3);
    mu_assert("rh=3", rh == 3);
    /* dst(0,0)=img(0,0)=0; dst(1,0)=img(2,0)=2; dst(2,0)=img(4,0)=4 */
    mu_assert("dst[0]=0", dst[0] == 0.0f);
    mu_assert("dst[2]=4", dst[2] == 4.0f);
    /* dst(0,2)=img(0,4)=20 */
    mu_assert("dst[6]=20", dst[6] == 20.0f);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  ssim_tools.c — exercises the scalar precompute / variance /        */
/*  accumulate fallbacks via the iqa_ssim entry point.                 */
/* ------------------------------------------------------------------ */

/* 11-tap Gaussian — matches g_gaussian_window_{h,v} in ssim_tools.h. */
static float kernel_gauss11[11] = {0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
                                   0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f};

/* NOLINTBEGIN(clang-analyzer-unix.Malloc) — ADR-0138 / ADR-0141 / ADR-0278
 * The malloc-failure mu_assert path returns the error string without
 * freeing the partial allocation. The analyzer can't see that
 * mu_assert-on-fail terminates the test process via the runner's
 * top-level return — the leak is bounded to that exit path. Same
 * pattern as test_iqa_convolve.c (file-level suppression for the same
 * reason). */
static char *test_iqa_ssim_identical_frames(void)
{
    /* ref == cmp should give ssim ≈ 1.0. Use a 16x16 deterministic
     * gradient — enough for the 11-tap window plus a few interior
     * pixels (dst region is 6x6). */
    const int w = 16;
    const int h = 16;
    float *ref = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    float *cmp = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    mu_assert("malloc ok", ref && cmp);
    for (int i = 0; i < w * h; ++i) {
        ref[i] = (float)(i % 64);
        cmp[i] = ref[i];
    }
    struct iqa_kernel k = {0};
    k.kernel_h = kernel_gauss11;
    k.kernel_v = kernel_gauss11;
    k.w = 11;
    k.h = 11;
    k.normalized = 1;
    float l = 0.0f;
    float c = 0.0f;
    float s = 0.0f;
    float ssim = iqa_ssim(ref, cmp, w, h, &k, NULL, NULL, &l, &c, &s);
    free(ref);
    free(cmp);
    mu_assert("ssim(identical) ~ 1.0", fabsf(ssim - 1.0f) < 1e-3f);
    mu_assert("l ~ 1.0", fabsf(l - 1.0f) < 1e-3f);
    mu_assert("c ~ 1.0", fabsf(c - 1.0f) < 1e-3f);
    mu_assert("s ~ 1.0", fabsf(s - 1.0f) < 1e-3f);
    return NULL;
}

static char *test_iqa_ssim_different_frames(void)
{
    /* A patterned ref vs. its negation -> low ssim, but the assertion
     * is just that the call returns a finite value and exercises the
     * accumulate path with non-trivial sigma. */
    const int w = 16;
    const int h = 16;
    float *ref = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    float *cmp = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    mu_assert("malloc ok", ref && cmp);
    uint32_t state = 0xDEADBEEFu;
    for (int i = 0; i < w * h; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        ref[i] = (float)((int)state & 0xFF);
        cmp[i] = (float)((int)(state >> 8) & 0xFF);
    }
    struct iqa_kernel k = {0};
    k.kernel_h = kernel_gauss11;
    k.kernel_v = kernel_gauss11;
    k.w = 11;
    k.h = 11;
    k.normalized = 1;
    float l = 0.0f;
    float c = 0.0f;
    float s = 0.0f;
    float ssim = iqa_ssim(ref, cmp, w, h, &k, NULL, NULL, &l, &c, &s);
    free(ref);
    free(cmp);
    mu_assert("ssim is finite", isfinite(ssim));
    mu_assert("l is finite", isfinite(l));
    mu_assert("c is finite", isfinite(c));
    mu_assert("s is finite", isfinite(s));
    /* Differing random fills should yield ssim well below 1.0. */
    mu_assert("ssim(random) < 0.99", ssim < 0.99f);
    return NULL;
}
/* NOLINTEND(clang-analyzer-unix.Malloc) */

/* ------------------------------------------------------------------ */
/*  Runner                                                             */
/* ------------------------------------------------------------------ */

static char *run_math_tests(void)
{
    mu_run_test(test_math_round_positive_half);
    mu_run_test(test_math_round_negative);
    mu_run_test(test_math_min_max);
    mu_run_test(test_math_cmp_float);
    mu_run_test(test_math_matrix_cmp);
    return NULL;
}

static char *run_kbnd_tests(void)
{
    mu_run_test(test_kbnd_symmetric_in_bounds);
    mu_run_test(test_kbnd_symmetric_negative_reflect);
    mu_run_test(test_kbnd_symmetric_positive_reflect);
    mu_run_test(test_kbnd_replicate);
    mu_run_test(test_kbnd_constant);
    return NULL;
}

static char *run_filter_tests(void)
{
    mu_run_test(test_iqa_filter_pixel_null_kernel);
    mu_run_test(test_iqa_filter_pixel_interior);
    mu_run_test(test_iqa_filter_pixel_edge_replicate);
    mu_run_test(test_iqa_img_filter_writes_result);
    mu_run_test(test_iqa_img_filter_reject_no_bnd_opt);
    mu_run_test(test_iqa_img_filter_inplace);
    return NULL;
}

static char *run_decimate_tests(void)
{
    mu_run_test(test_decimate_factor2_no_kernel);
    mu_run_test(test_decimate_inplace);
    mu_run_test(test_decimate_odd_dimension);
    return NULL;
}

static char *run_ssim_tests(void)
{
    mu_run_test(test_iqa_ssim_identical_frames);
    mu_run_test(test_iqa_ssim_different_frames);
    return NULL;
}

char *run_tests(void)
{
    char *msg = run_math_tests();
    if (msg)
        return msg;
    msg = run_kbnd_tests();
    if (msg)
        return msg;
    msg = run_filter_tests();
    if (msg)
        return msg;
    msg = run_decimate_tests();
    if (msg)
        return msg;
    return run_ssim_tests();
}
