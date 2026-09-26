/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <errno.h>
#include <stddef.h>

#include "test.h"
#include "vmaf_close_retry.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

static unsigned close_calls;
static int close_statuses[2];

static void script_close(int first, int second)
{
    close_calls = 0U;
    close_statuses[0] = first;
    close_statuses[1] = second;
}

int vmaf_close(VmafContext *vmaf)
{
    if (vmaf == NULL)
        return -EINVAL;
    const unsigned index = close_calls < 2U ? close_calls : 1U;
    close_calls += 1U;
    return close_statuses[index];
}

static char *test_fail_once_is_retried_and_invalidated(void)
{
    unsigned char context_token = 0U;
    VmafContext *vmaf = (VmafContext *)&context_token;
    script_close(-EIO, 0);

    const int err = vmaf_tool_close_context(&vmaf);

    mu_assert("fail-once close did not recover", err == 0);
    mu_assert("fail-once close was not retried exactly once", close_calls == 2U);
    mu_assert("successful close did not invalidate caller handle", vmaf == NULL);
    return NULL;
}

static char *test_persistent_failure_retains_handle_and_first_error(void)
{
    unsigned char context_token = 0U;
    VmafContext *const expected = (VmafContext *)&context_token;
    VmafContext *vmaf = expected;
    script_close(-EBUSY, -EIO);

    const int err = vmaf_tool_close_context(&vmaf);

    mu_assert("persistent close did not preserve first error", err == -EBUSY);
    mu_assert("persistent close did not stop at retry bound", close_calls == 2U);
    mu_assert("failed close discarded retryable handle", vmaf == expected);
    return NULL;
}

static char *test_empty_handle_is_noop(void)
{
    VmafContext *vmaf = NULL;
    script_close(-EIO, 0);

    const int err = vmaf_tool_close_context(&vmaf);

    mu_assert("empty close was not a no-op", err == 0);
    mu_assert("empty close invoked callback", close_calls == 0U);
    return NULL;
}

static char *test_positive_status_retains_handle(void)
{
    unsigned char context_token = 0U;
    VmafContext *const expected = (VmafContext *)&context_token;
    VmafContext *vmaf = expected;
    script_close(1, 1);

    const int err = vmaf_tool_close_context(&vmaf);

    mu_assert("positive close status was mistaken for success", err == 1);
    mu_assert("positive close status did not exhaust the retry bound", close_calls == 2U);
    mu_assert("positive close status discarded the retained handle", vmaf == expected);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_fail_once_is_retried_and_invalidated);
    mu_run_test(test_persistent_failure_retains_handle_and_first_error);
    mu_run_test(test_empty_handle_is_noop);
    mu_run_test(test_positive_status_retains_handle);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
