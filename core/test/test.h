/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#ifndef LIBVMAF_TEST_H
#define LIBVMAF_TEST_H

#include <stdio.h>

// http://www.jera.com/techinfo/jtns/jtn002.html

#define mu_assert(message, test)                                                                   \
    do {                                                                                           \
        if (!(test))                                                                               \
            return message;                                                                        \
    } while (0)

#ifdef __cplusplus
using mu_message_t = const char *;
extern "C" {
extern int mu_tests_run;
extern int mu_skipped;
mu_message_t run_tests(void);
}
#else
typedef char *mu_message_t;
extern int mu_tests_run;
extern int mu_skipped;
mu_message_t run_tests(void);
#endif

/* Propagate a helper's failure message. Mirrors mu_assert for helpers that
 * already return "NULL on success, or the message to fail with", which is how
 * a test body that outgrows the 60-line function budget is split into phase
 * helpers without changing which message a failure reports (ADR-1142). */
#define mu_assert_msg(expr)                                                                        \
    do {                                                                                           \
        mu_message_t mu_helper_msg = (expr);                                                       \
        if (mu_helper_msg)                                                                         \
            return mu_helper_msg;                                                                  \
    } while (0)

/* Reports pass/fail for one test and returns its message (NULL on
 * pass). Lives here as a `static inline` helper so every TU that
 * includes test.h gets one copy and so the `mu_run_test` macro
 * expansion stays short enough to avoid tripping
 * `readability-function-size` on test bodies that run many cases. */
#ifdef __cplusplus
static inline mu_message_t mu_report(const char *name, mu_message_t (*test)())
#else
static inline mu_message_t mu_report(const char *name, mu_message_t (*test)(void))
#endif
{
    (void)fprintf(stderr, "%s: ", name);
    mu_message_t message = test();
    mu_tests_run++;
    (void)fprintf(stderr, message ? "\033[31mfail\033[0m\n" : "\033[32mpass\033[0m\n");
    return message;
}

#define mu_run_test(test)                                                                          \
    do {                                                                                           \
        mu_message_t mu_msg = mu_report(#test, (test));                                            \
        if (mu_msg)                                                                                \
            return mu_msg;                                                                         \
    } while (0)

#endif /* LIBVMAF_TEST_H */
