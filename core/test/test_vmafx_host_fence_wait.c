/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Host fence waits (core/src/vmafx/fence.c) on the real clock: a signal from
 * another thread wakes the waiter long before its timeout, a wait without a
 * signal returns false no earlier than its timeout, a wait without a limit
 * returns on the signal, every one of several waiters wakes on one signal,
 * and a poll (timeout 0) answers at once. A host fence waits on a condition
 * variable with a timed wait; on the MSVC builds that is the Win32 shim's
 * pthread_cond_timedwait() (core/src/compat/win32/pthread.h), so this test
 * runs on every platform, Windows included.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "mu_table.h"
#include "test.h"
#include "vmafx/engine.h"
#include "vmafx/internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's C has no
 * nullptr (ADR-1138). */

#define FW_MS UINT64_C(1000000)  /* ns per ms */
#define FW_LONG (10000u * FW_MS) /* a timeout no passing wait reaches: 10 s */
#define FW_WAITERS 4
/* A woken waiter returns well inside this. Without the signal's broadcast it
 * would sleep until the end of a 1 s chunk of its timed wait (fence.c). */
#define FW_WAKE_BOUND (500u * FW_MS)

static void fw_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    const struct timespec t = {.tv_sec = (time_t)(ms / 1000u),
                               .tv_nsec = (long)(ms % 1000u) * 1000000L};
    (void)nanosleep(&t, NULL);
#endif
}

typedef struct FwWaiter {
    VmafxHostFence *fence;
    uint64_t timeout_ns;
    bool signalled;
    uint64_t waited_ns;
} FwWaiter;

static void *fw_wait(void *arg)
{
    FwWaiter *w = arg;
    const uint64_t start = vmafx_monotonic_ns();
    w->signalled = vmafx_host_fence_wait(w->fence, w->timeout_ns);
    w->waited_ns = vmafx_monotonic_ns() - start;
    return NULL;
}

static char *test_signal_wakes_the_waiter(void)
{
    VmafxHostFence *fence = vmafx_host_fence_new();
    mu_assert("vmafx_host_fence_new failed", fence != NULL);
    FwWaiter w = {.fence = fence, .timeout_ns = FW_LONG};
    pthread_t thread;
    mu_assert("pthread_create failed", pthread_create(&thread, NULL, fw_wait, &w) == 0);
    fw_sleep_ms(30);
    vmafx_host_fence_signal(fence);
    mu_assert("pthread_join failed", pthread_join(thread, NULL) == 0);
    vmafx_host_fence_unref(fence);
    mu_assert("a signalled wait must return true", w.signalled);
    mu_assert("a signalled wait must return within 0.5 s of the signal",
              w.waited_ns < (30u * FW_MS) + FW_WAKE_BOUND);
    return NULL;
}

static char *test_wait_without_signal_times_out(void)
{
    VmafxHostFence *fence = vmafx_host_fence_new();
    mu_assert("vmafx_host_fence_new failed", fence != NULL);
    FwWaiter w = {.fence = fence, .timeout_ns = 250u * FW_MS};
    (void)fw_wait(&w);
    vmafx_host_fence_unref(fence);
    mu_assert("an unsignalled wait must return false", !w.signalled);
    mu_assert("an unsignalled wait must not return before its timeout",
              w.waited_ns >= 250u * FW_MS);
    mu_assert("an unsignalled wait must return within 5 s of its timeout",
              w.waited_ns < 250u * FW_MS + 5000u * FW_MS);
    return NULL;
}

static char *test_unlimited_wait_returns_on_signal(void)
{
    VmafxHostFence *fence = vmafx_host_fence_new();
    mu_assert("vmafx_host_fence_new failed", fence != NULL);
    FwWaiter w = {.fence = fence, .timeout_ns = UINT64_MAX};
    pthread_t thread;
    mu_assert("pthread_create failed", pthread_create(&thread, NULL, fw_wait, &w) == 0);
    fw_sleep_ms(250); /* past two chunks of the wait */
    vmafx_host_fence_signal(fence);
    mu_assert("pthread_join failed", pthread_join(thread, NULL) == 0);
    vmafx_host_fence_unref(fence);
    mu_assert("a wait without a limit must return true on the signal", w.signalled);
    return NULL;
}

static char *test_one_signal_wakes_every_waiter(void)
{
    VmafxHostFence *fence = vmafx_host_fence_new();
    mu_assert("vmafx_host_fence_new failed", fence != NULL);
    FwWaiter w[FW_WAITERS];
    pthread_t thread[FW_WAITERS];
    int started = 0;
    for (int n = 0; n < FW_WAITERS; ++n) {
        w[n] = (FwWaiter){.fence = fence, .timeout_ns = FW_LONG};
        started += pthread_create(&thread[n], NULL, fw_wait, &w[n]) == 0 ? 1 : 0;
    }
    fw_sleep_ms(30);
    vmafx_host_fence_signal(fence);
    int all = started == FW_WAITERS;
    for (int n = 0; n < started; ++n) {
        all &= pthread_join(thread[n], NULL) == 0 && w[n].signalled &&
               w[n].waited_ns < (30u * FW_MS) + FW_WAKE_BOUND;
    }
    vmafx_host_fence_unref(fence);
    mu_assert("every waiter must wake on one signal", all);
    return NULL;
}

static char *test_poll_answers_at_once(void)
{
    VmafxHostFence *fence = vmafx_host_fence_new();
    mu_assert("vmafx_host_fence_new failed", fence != NULL);
    const bool before = vmafx_host_fence_wait(fence, 0u);
    vmafx_host_fence_signal(fence);
    const bool after = vmafx_host_fence_wait(fence, 0u);
    vmafx_host_fence_unref(fence);
    mu_assert("a poll of an unsignalled fence must return false", !before);
    mu_assert("a poll of a signalled fence must return true", after);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_signal_wakes_the_waiter),
        MU_TEST(test_wait_without_signal_times_out),
        MU_TEST(test_unlimited_wait_returns_on_signal),
        MU_TEST(test_one_signal_wakes_every_waiter),
        MU_TEST(test_poll_answers_at_once),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
