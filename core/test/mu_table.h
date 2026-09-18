/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Table-driven runner for the minunit harness in test.h.
 *
 * Each mu_run_test() expands to a branch, so a run_tests() body with more
 * than fifteen tests breaks the readability-function-size branch budget
 * (ADR-0141). Splitting such a body into hand-picked groups works but has to
 * be rebalanced whenever a test is added. A table has no branches however
 * many rows it holds, and adding a test stays a one-line edit:
 *
 *     static const MuTest tests[] = {
 *         MU_TEST(test_first),
 *         MU_TEST(test_second),
 *     };
 *     return mu_run_table(tests, MU_TABLE_LEN(tests));
 *
 * Output and early-exit behaviour are identical to a sequence of
 * mu_run_test() calls: each test is reported through mu_report() and the
 * first failure message is returned without running the rest.
 */

#ifndef VMAF_TEST_MU_TABLE_H_
#define VMAF_TEST_MU_TABLE_H_

#include <stddef.h>

#include "test.h"

typedef struct MuTest {
    const char *name;
    mu_message_t (*fn)(void);
} MuTest;

#define MU_TEST(test) {#test, (test)}
#define MU_TABLE_LEN(table) (sizeof(table) / sizeof((table)[0]))

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23, where
 * clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr` while the required
 * Windows build compiles the tests including this header with cl.exe.
 * ADR-1138. */
static inline mu_message_t mu_run_table(const MuTest *tests, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        mu_message_t msg = mu_report(tests[i].name, tests[i].fn);
        if (msg)
            return msg;
    }
    return NULL;
}
/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAF_TEST_MU_TABLE_H_ */
