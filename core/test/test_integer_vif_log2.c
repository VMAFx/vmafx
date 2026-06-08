/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 2 — integer_vif.h log2_32 / log2_64 inline helpers.
 *
 *  The compact ADR-0500 log2 LUT has its uint16 entries derived from
 *  log2f((float)(VIF_LOG2_TABLE_OFFSET + i)) * 2048.  log2_32 / log2_64
 *  apply a clz-based normalisation that strips the MSB and recovers
 *  the 15-bit index.  Test the function value for known temp inputs
 *  against the closed-form expectation.
 *
 *  Plugs uncovered lines 146-162 of `core/src/feature/integer_vif.h`
 *  (the inline body is otherwise reached only via the AVX-512 hot
 *  path which CI gates on with parity tests already).
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/integer_vif.h"

/* Build the LUT exactly as `log_generate()` in integer_vif.c does. */
static void build_log2_table(uint16_t *t)
{
    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i) {
        t[i] = (uint16_t)round(log2f((float)(VIF_LOG2_TABLE_OFFSET + i)) * 2048);
    }
}

static char *test_log2_32_value_matches_closed_form(void)
{
    uint16_t *t = (uint16_t *)malloc(sizeof(uint16_t) * VIF_LOG2_TABLE_SIZE);
    mu_assert("alloc log2_table", t != NULL);
    build_log2_table(t);

    /* temp = 65536 = 2^16 → log2(temp) = 16 → integer_vif result = 16*2048
     * because the table value for index 0 (mantissa 32768) is log2(32768)*2048
     * = 15*2048 and k = 1 contributes +2048. Total = 16*2048 = 32768. */
    int32_t r = log2_32(t, 65536u);
    mu_assert("log2_32(65536) must be 16*2048", r == 32768);

    /* temp = 32768 = 2^15 → log2(temp) = 15 → integer_vif result = 15*2048
     * because the table value for index 0 (mantissa 32768) is log2(32768)*2048
     * = 15*2048 and k = 0 contributes 0. Total = 15*2048 = 30720. */
    int32_t r2 = log2_32(t, 32768u);
    mu_assert("log2_32(32768) must be 15*2048", r2 == 30720);

    free(t);
    return NULL;
}

static char *test_log2_64_value_matches_closed_form(void)
{
    uint16_t *t = (uint16_t *)malloc(sizeof(uint16_t) * VIF_LOG2_TABLE_SIZE);
    mu_assert("alloc log2_table", t != NULL);
    build_log2_table(t);

    /* temp = 1<<17 = 131072 → log2(temp) = 17 → result = 17*2048 = 34816 */
    int32_t r = log2_64(t, (uint64_t)1u << 17);
    mu_assert("log2_64(2^17) must be 17*2048", r == 34816);

    /* temp = 1<<32 → log2(temp) = 32 → result = 32*2048 = 65536 */
    int32_t r2 = log2_64(t, (uint64_t)1u << 32);
    mu_assert("log2_64(2^32) must be 32*2048", r2 == 65536);

    /* temp = 1<<40 → log2(temp) = 40 → result = 40*2048 = 81920 */
    int32_t r3 = log2_64(t, (uint64_t)1u << 40);
    mu_assert("log2_64(2^40) must be 40*2048", r3 == 81920);

    free(t);
    return NULL;
}

static char *test_log2_monotonic_increasing(void)
{
    uint16_t *t = (uint16_t *)malloc(sizeof(uint16_t) * VIF_LOG2_TABLE_SIZE);
    mu_assert("alloc log2_table", t != NULL);
    build_log2_table(t);

    /* The integer log2 must be monotonic over a sweep — successive
     * powers-of-two and intermediate values cannot decrease. */
    int32_t prev = log2_32(t, 0x10000u);
    for (uint32_t v = 0x11000u; v < 0x80000u; v += 0x1000u) {
        int32_t cur = log2_32(t, v);
        mu_assert("log2_32 must be monotonically non-decreasing", cur >= prev);
        prev = cur;
    }

    /* Same for log2_64 on 64-bit inputs. */
    int32_t prev64 = log2_64(t, (uint64_t)0x100000u);
    for (uint64_t v = 0x110000u; v < 0x1000000u; v += 0x100000u) {
        int32_t cur = log2_64(t, v);
        mu_assert("log2_64 must be monotonically non-decreasing", cur >= prev64);
        prev64 = cur;
    }

    free(t);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_log2_32_value_matches_closed_form);
    mu_run_test(test_log2_64_value_matches_closed_form);
    mu_run_test(test_log2_monotonic_increasing);
    return NULL;
}
