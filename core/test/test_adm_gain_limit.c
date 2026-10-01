/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The integer ADM enhancement gain limit without binary64
 * (core/src/feature/adm_gain_limit.h; ADR-1413,
 * T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29).
 *
 * The scalar decouple kernels store MIN(rst * gain, t) or MAX(rst * gain, t),
 * a double, in an integer: what reaches the band is the double product of the
 * sample and `adm_enhn_gain_limit`, truncated toward zero. The SYCL twin has no
 * double on the device and used a Q31 fixed-point product, which floors
 * instead of truncating and does not round the way the double product does:
 * at a limit of 1.2 the double 5 * 1.2 is exactly 6 while the Q31 product is
 * 5, and the double -3 * 1.2 truncates to -3 while the Q31 product floors to
 * -4. adm_gain_limit_product() is the integer form the twin uses now, and
 * these tests hold it to the double product for every sample.
 *
 * Host-only: no GPU, no device runtime.
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "adm_gain_limit.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

/* What the scalar kernels store: the double product, truncated. */
static int64_t reference_product(int32_t rst, double gain)
{
    return (int64_t)((double)rst * gain);
}

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* Number of samples in [lo, hi] whose integer product is not the reference. */
static long mismatches_in_range(double gain, int64_t lo, int64_t hi)
{
    const struct AdmGainLimit g = adm_gain_limit_split(gain);
    long bad = 0;

    for (int64_t r = lo; r <= hi; ++r) {
        if (adm_gain_limit_product((int32_t)r, g) != reference_product((int32_t)r, gain)) {
            ++bad;
        }
    }
    return bad;
}

/* The same over `count` samples drawn from the whole int32 range, half of
 * them shifted down so that every magnitude class is visited. */
static long mismatches_in_random(double gain, uint64_t seed, int count)
{
    const struct AdmGainLimit g = adm_gain_limit_split(gain);
    uint64_t state = seed;
    long bad = 0;

    for (int i = 0; i < count; ++i) {
        const uint64_t bits = xorshift64(&state);
        int32_t r = (int32_t)(uint32_t)bits;
        if ((i & 1) != 0) {
            r /= (int32_t)(1 << ((bits >> 40) % 28u));
        }
        if (adm_gain_limit_product(r, g) != reference_product(r, gain)) {
            ++bad;
        }
    }
    return bad;
}

/* positive: the limits of the shipped models are integral; the split keeps
 * them exact. */
static char *test_integral_limits_split_exactly(void)
{
    const struct AdmGainLimit one = adm_gain_limit_split(1.0);
    const struct AdmGainLimit hundred = adm_gain_limit_split(100.0);

    mu_assert("a limit of 1 is 2^52 * 2^-52",
              one.m_hi == (1u << 20) && one.m_lo == 0u && one.frac_bits == 52);
    mu_assert("a limit of 100 is 100 * 2^46 * 2^-46",
              hundred.m_hi == (100u << 14) && hundred.m_lo == 0u && hundred.frac_bits == 46);
    return NULL;
}

/* positive: with an integral limit the product is the plain integer product. */
static char *test_integral_limits_give_the_integer_product(void)
{
    const struct AdmGainLimit one = adm_gain_limit_split(1.0);
    const struct AdmGainLimit hundred = adm_gain_limit_split(100.0);

    mu_assert("12345 * 1", adm_gain_limit_product(12345, one) == 12345);
    mu_assert("-12345 * 1", adm_gain_limit_product(-12345, one) == -12345);
    mu_assert("12345 * 100", adm_gain_limit_product(12345, hundred) == 1234500);
    mu_assert("-12345 * 100", adm_gain_limit_product(-12345, hundred) == -1234500);
    mu_assert("a limit of 1 over +-300000", mismatches_in_range(1.0, -300000, 300000) == 0);
    mu_assert("a limit of 100 over +-300000", mismatches_in_range(100.0, -300000, 300000) == 0);
    return NULL;
}

/* positive: the two cases a fixed-point product gets wrong, pinned by value. */
static char *test_fractional_limit_rounds_then_truncates(void)
{
    const struct AdmGainLimit g = adm_gain_limit_split(1.2);

    /* The double nearest 1.2 is below 1.2 and 5 times it is below 6, but the
     * double product rounds to 6 exactly. */
    mu_assert("5 * 1.2 is 6 in double", reference_product(5, 1.2) == 6);
    mu_assert("5 * 1.2 must be 6", adm_gain_limit_product(5, g) == 6);
    mu_assert("-5 * 1.2 must be -6", adm_gain_limit_product(-5, g) == -6);
    /* 3 * 1.2 is 3.6: truncation gives 3 and -3, a floor would give -4. */
    mu_assert("3 * 1.2 must be 3", adm_gain_limit_product(3, g) == 3);
    mu_assert("-3 * 1.2 must be -3", adm_gain_limit_product(-3, g) == -3);
    mu_assert("0 * 1.2 must be 0", adm_gain_limit_product(0, g) == 0);
    return NULL;
}

/* negative: no sample may leave the double product, at the limits under test
 * elsewhere and at limits spread over the option's range. */
static char *test_fractional_limits_match_the_double_product(void)
{
    static const double gains[] = {1.2, 1.5, 1.3, 7.0 / 3.0, 99.0 + (1.0 / 3.0)};
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);

    for (size_t k = 0; k < sizeof(gains) / sizeof(gains[0]); ++k) {
        mu_assert("a fractional limit left the double product over +-70000",
                  mismatches_in_range(gains[k], -70000, 70000) == 0);
        mu_assert("a fractional limit left the double product on random samples",
                  mismatches_in_random(gains[k], 0x5eedu + k, 400000) == 0);
    }
    for (int n = 0; n < 400; ++n) {
        /* A limit in [1, 100) with all 53 significand bits in play. */
        const double gain =
            1.0 + ((double)(xorshift64(&state) >> 11) * (99.0 / 9007199254740992.0));
        mu_assert("a random limit left the double product",
                  mismatches_in_random(gain, state, 4000) == 0);
    }
    return NULL;
}

/* boundary: the ends of the int32 range and of the limits the split admits. */
static char *test_range_ends(void)
{
    static const double gains[] = {
        1.0,  1.0000000000000002, 1.2, 63.999999999999993, 64.0, 99.999999999999986, 100.0,
        0.25, 2097151.5,
    };
    static const int32_t samples[] = {INT32_MIN, INT32_MIN + 1, -1, 0, 1, INT32_MAX - 1, INT32_MAX};

    for (size_t k = 0; k < sizeof(gains) / sizeof(gains[0]); ++k) {
        const struct AdmGainLimit g = adm_gain_limit_split(gains[k]);
        mu_assert("the split left the supported range", g.frac_bits >= 32 && g.frac_bits <= 54);
        for (size_t n = 0; n < sizeof(samples) / sizeof(samples[0]); ++n) {
            mu_assert("a range end left the double product",
                      adm_gain_limit_product(samples[n], g) ==
                          reference_product(samples[n], gains[k]));
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integral_limits_split_exactly);
    mu_run_test(test_integral_limits_give_the_integer_product);
    mu_run_test(test_fractional_limit_rounds_then_truncates);
    mu_run_test(test_fractional_limits_match_the_double_product);
    mu_run_test(test_range_ends);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
