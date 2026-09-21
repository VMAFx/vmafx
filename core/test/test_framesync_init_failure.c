/**
 *
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>

#include "framesync.h"
#include "test.h"
#include "test_framesync_init_failure.h"

static unsigned init_calls;
static unsigned fail_init_at;
static unsigned mutex_destroys;
static unsigned condition_destroys;

int vmafx_test_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
    init_calls++;
    if (init_calls == fail_init_at)
        return EAGAIN;
    return pthread_mutex_init(mutex, attributes);
}

int vmafx_test_pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
    init_calls++;
    if (init_calls == fail_init_at)
        return ENOMEM;
    return pthread_cond_init(condition, attributes);
}

int vmafx_test_pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    mutex_destroys++;
    return pthread_mutex_destroy(mutex);
}

int vmafx_test_pthread_cond_destroy(pthread_cond_t *condition)
{
    condition_destroys++;
    return pthread_cond_destroy(condition);
}

static char *test_init_rejects_null_output(void)
{
    mu_assert("framesync init must reject a null output pointer",
              vmaf_framesync_init((VmafFrameSyncContext **)0) == -EINVAL);
    return (char *)0;
}

static char *test_destroy_accepts_null_context(void)
{
    mu_assert("framesync destroy must accept a null context",
              vmaf_framesync_destroy((VmafFrameSyncContext *)0) == 0);
    return (char *)0;
}

static char *test_init_failure_unwinds_initialized_primitives(void)
{
    const int expected_errors[] = {-EAGAIN, -EAGAIN, -ENOMEM};
    const unsigned expected_mutex_destroys[] = {0, 1, 2};
    for (unsigned failure = 1; failure <= 3; failure++) {
        init_calls = 0;
        mutex_destroys = 0;
        condition_destroys = 0;
        fail_init_at = failure;
        unsigned char sentinel = 0;
        VmafFrameSyncContext *context = (VmafFrameSyncContext *)&sentinel;
        const int error = vmaf_framesync_init(&context);
        fail_init_at = 0;
        const bool clean = error == expected_errors[failure - 1] &&
                           context == (VmafFrameSyncContext *)0 && init_calls == failure &&
                           mutex_destroys == expected_mutex_destroys[failure - 1] &&
                           condition_destroys == 0;
        mu_assert("framesync init failure must unwind only initialized primitives", clean);
    }
    return (char *)0;
}

char *run_tests(void)
{
    mu_run_test(test_init_rejects_null_output);
    mu_run_test(test_destroy_accepts_null_context);
    mu_run_test(test_init_failure_unwinds_initialized_primitives);
    return (char *)0;
}
