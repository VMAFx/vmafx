/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The covariance blocks test_sycl_speed_cov_math and test_sycl_speed_cov_chain
 * check against speed.c's compute_cov_kernel_scalar()
 * (T-SPEED-CHROMA-SYCL-COV-1ULP-2026-10-06): flat random blocks, and blocks
 * whose terms cancel to a few parts in 1e7 of their magnitude, MIN_DISCRIMINATING
 * of which a near-exact sum (x86-64 long double, rounded once) stores as
 * another fp32 value than the reference does. build_cases() fills xs, ys,
 * mxs, mys and want (the reference's entries); count_wrong() compares `got`
 * with want bit for bit.
 *
 * One definition for both tests; include it from one translation unit each.
 */

#ifndef VMAF_TEST_SPEED_COV_CASES_H_
#define VMAF_TEST_SPEED_COV_CASES_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "feature/speed_cov.h"
#include "float_bits.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

enum {
    WIDTH = 11,
    HEIGHT = 6,
    STRIDE = 16,
    BLOCK = HEIGHT * STRIDE,
    FLAT_CASES = 256,
    CANCELLING_KEPT = 192,
    MIN_DISCRIMINATING = 64,
    CASES = FLAT_CASES + CANCELLING_KEPT,
    MAX_TRIALS = 3000000,
};

static float xs[(size_t)CASES * BLOCK];
static float ys[(size_t)CASES * BLOCK];
static float mxs[CASES];
static float mys[CASES];
static float want[CASES];
static float got[CASES];
static unsigned discriminating;

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint32_t next_u32(void)
{
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(rng_state >> 33);
}

/* A value in [0, 1) with a full fp32 significand: short significands make
 * every product and every partial sum exact and the two sums equal. */
static double unit_d(void)
{
    return (double)next_u32() / 2147483648.0;
}

static float unit(void)
{
    return (float)unit_d();
}

/* Fills block `c`: random samples around a mean, and term f + 33 cancelling
 * term f up to the fp32 rounding of the sample, so the running sum climbs to
 * ~1e4 over the first half of the block and falls back to ~1e-4 over the
 * second: the reference's fp64 adds round at the size of the large partial
 * sums, which the small total then exposes in its fp32 value. */
static void fill_cancelling(size_t c)
{
    float *x = xs + c * BLOCK;
    float *y = ys + c * BLOCK;
    const float mx = 20.0f + 400.0f * unit();
    const float my = 20.0f + 400.0f * unit();
    mxs[c] = mx;
    mys[c] = my;
    memset(x, 0, BLOCK * sizeof(float));
    memset(y, 0, BLOCK * sizeof(float));
    const size_t half = (size_t)WIDTH * HEIGHT / 2u;
    for (size_t f = 0; f < half; f++) {
        const size_t i0 = (f / WIDTH) * STRIDE + f % WIDTH;
        const size_t g = f + half;
        const size_t i1 = (g / WIDTH) * STRIDE + g % WIDTH;
        const double dx0 = 1.0 + 40.0 * unit_d();
        const double dy0 = 1.0 + 40.0 * unit_d();
        const double dx1 = 1.0 + 40.0 * unit_d();
        x[i0] = (float)((double)mx + dx0);
        y[i0] = (float)((double)my + dy0);
        x[i1] = (float)((double)mx - dx1);
        const double a = (double)x[i0] - (double)mx;
        const double b = (double)y[i0] - (double)my;
        const double d = (double)x[i1] - (double)mx;
        y[i1] = (float)((double)my - a * b / d);
    }
}

static void fill_flat(size_t c)
{
    float *x = xs + c * BLOCK;
    float *y = ys + c * BLOCK;
    mxs[c] = 100.0f * unit();
    mys[c] = 100.0f * unit();
    memset(x, 0, BLOCK * sizeof(float));
    memset(y, 0, BLOCK * sizeof(float));
    for (size_t r = 0; r < HEIGHT; r++) {
        for (size_t j = 0; j < WIDTH; j++) {
            x[r * STRIDE + j] = 255.0f * unit();
            y[r * STRIDE + j] = 255.0f * unit();
        }
    }
}

/* The reference's entry, and the entry of a sum rounded once. */
static float reference_entry(size_t c)
{
    const double sum = compute_cov_kernel_scalar(xs + c * BLOCK, ys + c * BLOCK, STRIDE, HEIGHT,
                                                 WIDTH, (double)mxs[c], (double)mys[c]);
    return (float)(sum / (WIDTH * HEIGHT));
}

static float near_exact_entry(size_t c)
{
    const float *x = xs + c * BLOCK;
    const float *y = ys + c * BLOCK;
    long double sum = 0.0L;
    for (size_t r = 0; r < HEIGHT; r++) {
        for (size_t j = 0; j < WIDTH; j++) {
            const double vx = (double)x[r * STRIDE + j] - (double)mxs[c];
            const double vy = (double)y[r * STRIDE + j] - (double)mys[c];
            const volatile double product = vx * vy;
            sum += (long double)product;
        }
    }
    return (float)(sum / (long double)(WIDTH * HEIGHT));
}

/* Flat blocks first, then cancelling blocks drawn until MIN_DISCRIMINATING of
 * them separate the two sums (a few in a thousand do) and the rest of the
 * CANCELLING_KEPT do not. */
static void build_cases(void)
{
    static int built;
    if (built) {
        return;
    }
    built = 1;
    size_t c = 0;
    for (; c < FLAT_CASES; c++) {
        fill_flat(c);
        want[c] = reference_entry(c);
    }
    unsigned plain = 0u;
    for (unsigned trial = 0u; trial < MAX_TRIALS && c < CASES; trial++) {
        fill_cancelling(c);
        want[c] = reference_entry(c);
        const int separates = want[c] != near_exact_entry(c);
        if (separates && discriminating < MIN_DISCRIMINATING) {
            discriminating++;
            c++;
        } else if (!separates && plain < CANCELLING_KEPT - MIN_DISCRIMINATING) {
            plain++;
            c++;
        }
    }
}

static unsigned count_wrong(const char *where)
{
    unsigned wrong = 0u;
    for (size_t c = 0; c < CASES; c++) {
        if (vmaf_test_bits_f32(got[c]) != vmaf_test_bits_f32(want[c]) &&
            !(got[c] == 0.0f && want[c] == 0.0f)) {
            if (wrong < 4u) {
                (void)fprintf(stderr, "[%s case %zu: got %a want %a] ", where, c, (double)got[c],
                              (double)want[c]);
            }
            wrong++;
        }
    }
    return wrong;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAF_TEST_SPEED_COV_CASES_H_ */
