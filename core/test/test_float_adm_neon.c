/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness for the float-ADM helper kernels
 * (`float_adm_csf_neon`, `float_adm_csf_den_scale_neon`,
 * `float_adm_sum_cube_neon` in core/src/feature/arm64/float_adm_neon.c).
 *
 * Why this file exists
 * --------------------
 * ADR-0873 gap 2 records that these three kernels have no dispatch caller and
 * no unit test; ADR-1057 is what a missing unit test on the fourth kernel in
 * the same file (`float_adm_dwt2_neon`) eventually cost — a dropped filter tap
 * that reached master and drifted the ARM Netflix golden.  This test closes the
 * coverage gap for the remaining three.
 *
 * What the reference asserts, and why it is not circular
 * ------------------------------------------------------
 * Two properties are under test, and both are derived from sources *outside*
 * the NEON translation unit:
 *
 *   1. Per-element precision.  `adm_csf_den_scale_s` / `adm_sum_cube_s`
 *      (core/src/feature/adm_tools.c) and both x86 twins
 *      (`float_adm_{csf_den_scale,sum_cube}_{avx2,avx512}`) compute
 *      `val = fabsf(factor * x)` and `val * val * val` in **float**, then widen
 *      the cube to double for accumulation.  Every element must therefore
 *      contribute the float-precision cube, whether it is handled by a vector
 *      lane or by the scalar tail.
 *
 *   2. Per-row accumulation.  The scalar references reset an inner accumulator
 *      at the top of every row (`accum_inner_h` / `accum_inner`) and fold it
 *      into the outer accumulator at the end of the row; both x86 twins mirror
 *      that with a per-row `row_accum`.  ADR-0873 decision 5 requires the NEON
 *      reduction to follow "the AVX2 path's `_mm256_cvtps_pd` strategy".
 *
 * The one thing the reference does copy from the NEON kernel is the intra-row
 * 4-lane grouping `(lane0 + lane2) + (lane1 + lane3)` produced by
 * `vaddvq_f64(vaddq_f64(v_accum0, v_accum1))`.  That is deliberately *not*
 * claimed as a defect here — a 4-lane NEON reduction cannot reproduce an
 * 8-lane AVX2 tree without restructuring the loop — so it is held fixed on
 * both sides.
 *
 * Property 1 is what this table actually detects.  Property 2 is asserted for
 * documentation value only: the reduction terms are all non-negative, so
 * reordering the *double* accumulation perturbs the sum by at most N * 2^-53
 * relative, which stays far below the 2^-24 rounding step of the `float`
 * return for any N a frame can produce.  No geometry in this table
 * distinguishes a per-row accumulator from one hoisted across rows, and none
 * is expected to.
 *
 * Geometry selection
 * ------------------
 * The reductions return a single `float`, so a per-element error in a 1..3
 * column tail is diluted below the final rounding of a large rectangle.  The
 * table therefore mixes ordinary random geometries with "tail-only" cases:
 * every column covered by the 4-wide vector body is zeroed and only the tail
 * columns carry data, so the return value is exactly `N * cube(v)` and any
 * per-element precision difference survives the cast to float.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"

#if ARCH_AARCH64
#include "feature/arm64/float_adm_neon.h"

/* FLOAT_ONE_BY_30 from adm_tools.c, which does not export it. */
#define TEST_ONE_BY_30 0.0333333351f

/* A float whose cube rounds differently when the multiplications are done in
 * float versus double:
 *   float:  fl(fl(v*v)*v) == 0x1.2431f2p+0
 *   double: (float)((double)v*v*v) == 0x1.2431f0p+0
 * Used by the tail-only geometries to make the divergence survive the final
 * cast to float. */
#define TEST_TAIL_VALUE 0x1.0b898ap+0f

typedef enum {
    DATA_RANDOM = 0,   /* pseudo-random floats everywhere */
    DATA_TAIL_ONLY = 1 /* zeros in the 4-wide vector body, TEST_TAIL_VALUE in the tail */
} data_mode_t;

typedef struct {
    int w, h;
    int left, top, right, bottom;
    data_mode_t mode;
} geom_t;

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Pseudo-random float in (-4, 4), no denormals, no infinities. */
static float rand_float(uint32_t *s)
{
    const uint32_t r = xs32(s);
    const uint32_t bits = (r & 0x007FFFFFu) | 0x3F800000u; /* [1, 2) */
    float f;

    memcpy(&f, &bits, sizeof(f));
    f -= 1.5f; /* [-0.5, 0.5) */
    f *= (float)(1u << (r % 4u));
    return (r & 0x80000000u) ? -f : f;
}

static void fill_plane(float *buf, const geom_t *g, uint32_t seed)
{
    const int vec_end = g->left + (((g->right - g->left) / 4) * 4);

    for (int i = 0; i < g->h; ++i) {
        for (int j = 0; j < g->w; ++j) {
            float v;
            if (g->mode == DATA_TAIL_ONLY) {
                const int in_rect = (i >= g->top && i < g->bottom && j >= g->left && j < g->right);
                v = (in_rect && j >= vec_end) ? TEST_TAIL_VALUE : 0.0f;
            } else {
                v = rand_float(&seed);
            }
            buf[(size_t)i * g->w + j] = v;
        }
    }
}

/*
 * Reference for float_adm_csf_neon, transcribed from the inner loop of
 * adm_csf_s() in adm_tools.c.  Purely element-wise: bit-exact by construction,
 * no reduction-order freedom.
 */
static void ref_csf(const float *src, float *dst, float *flt, int w, int h, int src_px_stride,
                    int dst_px_stride, float factor, float one_by_30)
{
    for (int i = 0; i < h; ++i) {
        const int src_off = i * src_px_stride;
        const int dst_off = i * dst_px_stride;

        for (int j = 0; j < w; ++j) {
            const float dst_val = factor * src[src_off + j];
            dst[dst_off + j] = dst_val;
            flt[dst_off + j] = one_by_30 * fabsf(dst_val);
        }
    }
}

/*
 * Reference for float_adm_csf_den_scale_neon / float_adm_sum_cube_neon.
 * `factor` is applied when `apply_factor` is set, matching the two entry
 * points.  Per-row accumulation and float-precision cubes come from the scalar
 * reference and the x86 twins; the 4-lane grouping matches the NEON reduction.
 */
static float ref_cube_reduce(const float *src, int px_stride, int left, int top, int right,
                             int bottom, float factor, int apply_factor)
{
    double accum = 0.0;

    for (int i = top; i < bottom; ++i) {
        const float *row = src + (size_t)i * px_stride;
        double lane[4] = {0.0, 0.0, 0.0, 0.0};
        double row_accum = 0.0;
        int j = left;

        for (; j + 3 < right; j += 4) {
            for (int t = 0; t < 4; ++t) {
                const float v = apply_factor ? fabsf(factor * row[j + t]) : fabsf(row[j + t]);
                lane[t] += (double)(v * v * v);
            }
        }
        row_accum += (lane[0] + lane[2]) + (lane[1] + lane[3]);

        for (; j < right; ++j) {
            const float v = apply_factor ? fabsf(factor * row[j]) : fabsf(row[j]);
            row_accum += (double)(v * v * v);
        }

        accum += row_accum;
    }

    return (float)accum;
}

static const geom_t geoms[] = {
    /* Random data: widths that are and are not multiples of the 4-wide stride,
     * odd heights, and non-zero left/top offsets. */
    {20, 8, 0, 0, 20, 8, DATA_RANDOM},
    {21, 8, 0, 0, 21, 8, DATA_RANDOM},
    {22, 7, 0, 0, 22, 7, DATA_RANDOM},
    {23, 5, 0, 0, 23, 5, DATA_RANDOM},
    {17, 3, 2, 1, 17, 3, DATA_RANDOM},
    {16, 1, 0, 0, 16, 1, DATA_RANDOM},
    {9, 9, 1, 1, 8, 8, DATA_RANDOM},
    {5, 4, 0, 0, 5, 4, DATA_RANDOM},
    {4, 4, 0, 0, 4, 4, DATA_RANDOM},
    {3, 1, 0, 0, 3, 1, DATA_RANDOM},
    {2, 1, 0, 0, 2, 1, DATA_RANDOM},
    {1, 1, 0, 0, 1, 1, DATA_RANDOM},
    /* Tail-only: the vector body sees zeros, so the result is exactly
     * N * cube(TEST_TAIL_VALUE) and a tail-precision divergence is visible in
     * the returned float. */
    {1, 8, 0, 0, 1, 8, DATA_TAIL_ONLY},
    {5, 8, 0, 0, 5, 8, DATA_TAIL_ONLY},
    {6, 8, 0, 0, 6, 8, DATA_TAIL_ONLY},
    {7, 8, 0, 0, 7, 8, DATA_TAIL_ONLY},
};

static const float factors[] = {1.0f, 0.10133042f, 3.7071068f};
#endif /* ARCH_AARCH64 */

static char *test_float_adm_csf_neon_matches_scalar(void)
{
#if !ARCH_AARCH64
    return NULL; /* NEON kernel is aarch64-only. */
#else
    for (size_t g = 0; g < sizeof(geoms) / sizeof(geoms[0]); ++g) {
        const geom_t *geo = &geoms[g];
        const int w = geo->w, h = geo->h;
        const size_t n = (size_t)w * h;

        float *src = calloc(n, sizeof(float));
        float *ref_dst = calloc(n, sizeof(float));
        float *ref_flt = calloc(n, sizeof(float));
        float *neon_dst = calloc(n, sizeof(float));
        float *neon_flt = calloc(n, sizeof(float));
        int mismatches = 0;

        mu_assert("calloc failed", src && ref_dst && ref_flt && neon_dst && neon_flt);
        fill_plane(src, geo, 0x51ed0000u ^ (uint32_t)g);

        for (size_t f = 0; f < sizeof(factors) / sizeof(factors[0]); ++f) {
            const float factor = factors[f];

            ref_csf(src, ref_dst, ref_flt, w, h, w, w, factor, TEST_ONE_BY_30);
            float_adm_csf_neon(src, neon_dst, neon_flt, w, h, (int)(w * sizeof(float)),
                               (int)(w * sizeof(float)), factor, TEST_ONE_BY_30);

            for (size_t i = 0; i < n; ++i) {
                if (memcmp(&ref_dst[i], &neon_dst[i], sizeof(float)) != 0 ||
                    memcmp(&ref_flt[i], &neon_flt[i], sizeof(float)) != 0) {
                    ++mismatches;
                    if (mismatches <= 6) {
                        (void)fprintf(stderr,
                                      "  csf %dx%d factor=%.9g idx %zu: dst %.9g vs %.9g, "
                                      "flt %.9g vs %.9g\n",
                                      w, h, (double)factor, i, (double)ref_dst[i],
                                      (double)neon_dst[i], (double)ref_flt[i], (double)neon_flt[i]);
                    }
                }
            }
        }

        free(src);
        free(ref_dst);
        free(ref_flt);
        free(neon_dst);
        free(neon_flt);

        mu_assert("float_adm_csf_neon diverges from the scalar reference", mismatches == 0);
    }
    return NULL;
#endif
}

static char *test_float_adm_csf_den_scale_neon_matches_scalar(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    int failures = 0;

    for (size_t g = 0; g < sizeof(geoms) / sizeof(geoms[0]); ++g) {
        const geom_t *geo = &geoms[g];
        const size_t n = (size_t)geo->w * geo->h;
        float *src = calloc(n, sizeof(float));

        mu_assert("calloc failed", src != NULL);
        fill_plane(src, geo, 0xc0ffee01u ^ (uint32_t)g);

        for (size_t f = 0; f < sizeof(factors) / sizeof(factors[0]); ++f) {
            const float factor = factors[f];
            const float expected = ref_cube_reduce(src, geo->w, geo->left, geo->top, geo->right,
                                                   geo->bottom, factor, 1);
            const float got =
                float_adm_csf_den_scale_neon(src, geo->w, geo->h, (int)(geo->w * sizeof(float)),
                                             geo->left, geo->top, geo->right, geo->bottom, factor);

            if (memcmp(&expected, &got, sizeof(float)) != 0) {
                ++failures;
                (void)fprintf(stderr,
                              "  den_scale %dx%d rect[%d,%d)x[%d,%d) width=%d tail=%d "
                              "mode=%d factor=%.9g: scalar %.9g (%a) != neon %.9g (%a)\n",
                              geo->w, geo->h, geo->left, geo->right, geo->top, geo->bottom,
                              geo->right - geo->left, (geo->right - geo->left) % 4, (int)geo->mode,
                              (double)factor, (double)expected, (double)expected, (double)got,
                              (double)got);
            }
        }
        free(src);
    }

    mu_assert("float_adm_csf_den_scale_neon diverges from the scalar reference", failures == 0);
    return NULL;
#endif
}

static char *test_float_adm_sum_cube_neon_matches_scalar(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    int failures = 0;

    for (size_t g = 0; g < sizeof(geoms) / sizeof(geoms[0]); ++g) {
        const geom_t *geo = &geoms[g];
        const size_t n = (size_t)geo->w * geo->h;
        float *src = calloc(n, sizeof(float));

        mu_assert("calloc failed", src != NULL);
        fill_plane(src, geo, 0xfeed0002u ^ (uint32_t)g);

        {
            const float expected =
                ref_cube_reduce(src, geo->w, geo->left, geo->top, geo->right, geo->bottom, 1.0f, 0);
            const float got =
                float_adm_sum_cube_neon(src, geo->w, geo->h, (int)(geo->w * sizeof(float)),
                                        geo->left, geo->top, geo->right, geo->bottom);

            if (memcmp(&expected, &got, sizeof(float)) != 0) {
                ++failures;
                (void)fprintf(stderr,
                              "  sum_cube %dx%d rect[%d,%d)x[%d,%d) width=%d tail=%d mode=%d: "
                              "scalar %.9g (%a) != neon %.9g (%a)\n",
                              geo->w, geo->h, geo->left, geo->right, geo->top, geo->bottom,
                              geo->right - geo->left, (geo->right - geo->left) % 4, (int)geo->mode,
                              (double)expected, (double)expected, (double)got, (double)got);
            }
        }
        free(src);
    }

    mu_assert("float_adm_sum_cube_neon diverges from the scalar reference", failures == 0);
    return NULL;
#endif
}

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_float_adm_csf_neon_matches_scalar);
    mu_run_test(test_float_adm_csf_den_scale_neon_matches_scalar);
    mu_run_test(test_float_adm_sum_cube_neon_matches_scalar);
#else
    (void)fprintf(stderr, "skipping: non-aarch64 arch\n");
    (void)test_float_adm_csf_neon_matches_scalar;
    (void)test_float_adm_csf_den_scale_neon_matches_scalar;
    (void)test_float_adm_sum_cube_neon_matches_scalar;
#endif
    return NULL;
}
