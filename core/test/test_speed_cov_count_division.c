/**
 *  Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The device SpEED twins divide every covariance sum by the element count of
 * the submatrix, sub_w * sub_h. speed.c (compute_covariance_row()) divides its
 * double sum by the exact size_t count. The twins carry the sum as an fp32
 * pair and divided it by `(float)(sub_w * sub_h)`, which is the count only up
 * to 2^24: above, an odd count has no fp32 value and the quotient moves. The
 * count reaches 2^24 with speed_prescale above 2 on pictures wider than 16K
 * (8181 x 8181 = 66,928,761 at the 32768 x 32768 cap with prescale 4).
 *
 * This test compiles the HIP twin's device header for the host (with
 * -ffp-contract=off, like the kernel build) and holds:
 *
 * - speed_hd_count_ff() is the count exactly: hi + lo == count up to 2^32;
 * - speed_hd_covariance_store() returns speed.c's
 *   (float)(sum / (double)count) for sums of fp32 pairs at counts above 2^24
 *   that fp32 cannot hold, and at every count a 16K picture reaches;
 * - up to 2^24 the division is the one-float division the twins had before,
 *   bit for bit, so nothing a picture up to 16K produces changes.
 *
 * The CUDA (speed_score.cu) and SYCL (sycl_exact_fp.h) twins carry the same
 * division; test_speed_cov_count_contract.py holds their source to it.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"
#include "float_bits.h"

#include "feature/hip/speed/speed_hip_device.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define TWO_POW_24 16777216u
#define SAMPLES_PER_COUNT 4096u

/* xorshift64: the same sums on every host. */
static uint64_t next_random(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* A covariance sum as the twins carry it: hi any fp32 of either sign over a
 * wide range of binades, lo at most half an ulp of hi, so hi + lo is a
 * double exactly. */
static SpeedHdFf random_sum(uint64_t *state)
{
    const uint64_t r = next_random(state);
    const float mantissa = (float)(r & 0xffffffu) / 16777216.0f + 0.5f;
    float hi = ldexpf(mantissa, (int)((r >> 24) % 60u) - 10);
    if ((r >> 40) & 1u)
        hi = -hi;
    const float ulp = nextafterf(fabsf(hi), INFINITY) - fabsf(hi);
    const int32_t step = (int32_t)((r >> 41) % 2001u) - 1000;
    return speed_hd_ff(hi, (float)step / 2000.0f * ulp);
}

/* speed.c: `float covariance = sums[k] / (submatrix_width * submatrix_height)`. */
static float cpu_covariance(SpeedHdFf sum, uint32_t count)
{
    const double total = (double)sum.hi + (double)sum.lo;
    return (float)(total / (double)count);
}

/* The division the twins had before: the count rounded to one fp32. */
static float one_float_division(SpeedHdFf sum, float divisor)
{
    const float quotient = sum.hi / divisor;
    const float remainder = fmaf(-quotient, divisor, sum.hi);
    const float correction = (remainder + sum.lo) / divisor;
    return quotient + correction;
}

/* The twin's store: the covariance entry (x, y) of channel 0. */
static float twin_covariance(SpeedHdFf sum, uint32_t sub_w, uint32_t sub_h)
{
    static float cov[SPEED_HIP_MATRIX];
    SpeedHipParams params;
    memset(&params, 0, sizeof(params));
    params.geometry.sub_w = sub_w;
    params.geometry.sub_h = sub_h;
    params.cov = cov;
    speed_hd_covariance_store(&params, 0u, 3u, 7u, sum);
    return cov[3u * SPEED_HIP_N + 7u];
}

static char *test_count_pair_is_exact(void)
{
    static const uint32_t counts[] = {1u,          TWO_POW_24 - 1u, TWO_POW_24, TWO_POW_24 + 1u,
                                      66928761u,   67043344u,       268435457u, 2147483647u,
                                      4294967295u, 3000000001u};
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        const SpeedHdFf pair = speed_hd_count_ff(counts[i]);
        const int64_t rebuilt = (int64_t)pair.hi + (int64_t)pair.lo;
        mu_assert("count pair hi + lo is the count", rebuilt == (int64_t)counts[i]);
        mu_assert("count pair lo is 0 while fp32 holds the count",
                  counts[i] > TWO_POW_24 || pair.lo == 0.0f);
    }
    return NULL;
}

/* Submatrix sizes past 2^24 elements: 8181 x 8181 (prescale 4 at the
 * 32768 x 32768 cap), an odd product just past 2^24, and a wide strip. */
static char *test_counts_above_two_pow_24_match_the_cpu(void)
{
    static const uint32_t dims[][2] = {{8181u, 8181u}, {4097u, 4097u}, {65535u, 1023u}};
    uint64_t state = 0x9e3779b97f4a7c15ull;
    unsigned before_fix = 0;
    for (size_t d = 0; d < sizeof(dims) / sizeof(dims[0]); d++) {
        const uint32_t count = dims[d][0] * dims[d][1];
        mu_assert("the fixture count has no fp32 value", (uint32_t)(float)count != count);
        for (unsigned i = 0; i < SAMPLES_PER_COUNT; i++) {
            const SpeedHdFf sum = random_sum(&state);
            const float cpu = cpu_covariance(sum, count);
            mu_assert("covariance above 2^24 equals speed.c's",
                      vmaf_test_identical_f32(twin_covariance(sum, dims[d][0], dims[d][1]), cpu));
            before_fix +=
                vmaf_test_identical_f32(one_float_division(sum, (float)count), cpu) ? 0u : 1u;
        }
    }
    /* The one-float division misses about a quarter of these. */
    mu_assert("the fixture separates the two divisions", before_fix > SAMPLES_PER_COUNT / 4u);
    return NULL;
}

/* Up to 2^24 the pair divisor has lo == 0 and the division is the old one:
 * the largest submatrix at 16K with prescale 4 is 3836 x 2156. */
static char *test_counts_up_to_two_pow_24_are_unchanged(void)
{
    static const uint32_t dims[][2] = {
        {3836u, 2156u}, {1916u, 1076u}, {476u, 266u}, {4096u, 4096u}, {31u, 17u}};
    uint64_t state = 0x2545f4914f6cdd1dull;
    for (size_t d = 0; d < sizeof(dims) / sizeof(dims[0]); d++) {
        const uint32_t count = dims[d][0] * dims[d][1];
        for (unsigned i = 0; i < SAMPLES_PER_COUNT; i++) {
            const SpeedHdFf sum = random_sum(&state);
            const float before = one_float_division(sum, (float)count);
            const float after = twin_covariance(sum, dims[d][0], dims[d][1]);
            mu_assert("division up to 2^24 is unchanged", vmaf_test_identical_f32(before, after));
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_count_pair_is_exact);
    mu_run_test(test_counts_above_two_pow_24_match_the_cpu);
    mu_run_test(test_counts_up_to_two_pow_24_are_unchanged);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
