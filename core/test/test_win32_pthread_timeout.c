/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The deadline arithmetic of the Win32 pthread shim's
 *  pthread_cond_timedwait() (core/src/compat/win32/pthread.h). The shim
 *  itself only compiles on Windows; its conversion of an absolute deadline
 *  into the relative milliseconds SleepConditionVariableSRW() takes lives in
 *  core/src/compat/win32/pthread_timeout.h so that it is tested here on every
 *  host: rounded up (a wait never ends before its deadline), 0 once the
 *  deadline has passed (the shim returns ETIMEDOUT), and capped below
 *  INFINITE without overflow for a deadline centuries away.
 */

#include <stdint.h>

#include "test.h"

#include "compat/win32/pthread_timeout.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's C has no
 * nullptr (ADR-1138). */

#define CAP (UINT32_C(0xFFFFFFFF) - 1u) /* INFINITE - 1 */

static char *test_passed_deadline_is_zero(void)
{
    mu_assert("a deadline equal to now must give 0", vmaf_w32_timeout_ms(5, 0, 5, 0, CAP) == 0u);
    mu_assert("a deadline 1 ns ago must give 0",
              vmaf_w32_timeout_ms(5, 999999999, 6, 0, CAP) == 0u);
    mu_assert("a deadline a second ago must give 0", vmaf_w32_timeout_ms(4, 0, 5, 0, CAP) == 0u);
    return NULL;
}

static char *test_rounds_up(void)
{
    mu_assert("1 ns must wait 1 ms", vmaf_w32_timeout_ms(5, 1, 5, 0, CAP) == 1u);
    mu_assert("1 ms must wait 1 ms", vmaf_w32_timeout_ms(5, 1000000, 5, 0, CAP) == 1u);
    mu_assert("1 ms + 1 ns must wait 2 ms", vmaf_w32_timeout_ms(5, 1000001, 5, 0, CAP) == 2u);
    mu_assert("1 ns across a second boundary must wait 1 ms",
              vmaf_w32_timeout_ms(6, 0, 5, 999999999, CAP) == 1u);
    mu_assert("10 s must wait 10000 ms", vmaf_w32_timeout_ms(15, 250, 5, 250, CAP) == 10000u);
    mu_assert("5 s minus 1 ns must wait 5000 ms",
              vmaf_w32_timeout_ms(9, 999999999, 5, 0, CAP) == 5000u);
    return NULL;
}

static char *test_caps_below_infinite(void)
{
    mu_assert("a wait of exactly the cap must be the cap",
              vmaf_w32_timeout_ms(0, 0, 0, 0, 7u) == 0u &&
                  vmaf_w32_timeout_ms(0, 7000000, 0, 0, 7u) == 7u);
    mu_assert("one ms past the cap must be the cap",
              vmaf_w32_timeout_ms(0, 8000000, 0, 0, 7u) == 7u);
    mu_assert("one ms below the cap must not be capped",
              vmaf_w32_timeout_ms(0, 6000000, 0, 0, 7u) == 6u);
    mu_assert("50 days must be the INFINITE - 1 cap",
              vmaf_w32_timeout_ms(INT64_C(50) * 86400, 0, 0, 0, CAP) == CAP);
    mu_assert("a deadline at INT64_MAX seconds must be the cap, not an overflow",
              vmaf_w32_timeout_ms(INT64_MAX, 999999999, 0, 0, CAP) == CAP);
    mu_assert("49.7 days minus a second must not be capped",
              vmaf_w32_timeout_ms(4294966, 0, 0, 0, CAP) == 4294966000u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_passed_deadline_is_zero);
    mu_run_test(test_rounds_up);
    mu_run_test(test_caps_below_infinite);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
