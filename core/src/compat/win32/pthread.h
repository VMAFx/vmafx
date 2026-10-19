/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Minimal pthread shim for Windows MSVC builds.
 *
 * Maps the pthread subset used by libvmaf (mutex / cond, timed wait included /
 * once / thread create+join+detach) onto Win32 SRWLOCK + CONDITION_VARIABLE +
 * INIT_ONCE + _beginthreadex. Activated
 * by libvmaf/meson.build when cc.check_header('pthread.h') fails — i.e. on
 * MSVC / clang-cl, where the platform ships no pthread.h. MinGW provides its
 * own pthread.h (winpthreads) and resolves it ahead of this shim.
 *
 * Scope: exactly the API surface in use across libvmaf. Adding more later is
 * fine; do not add what is not yet called from the tree.
 *
 * Semantics: SRWLOCK and CONDITION_VARIABLE are Windows Vista+. The Windows
 * GPU build-only legs run on windows-2022, well above that floor. No spurious-
 * wake or recursive-mutex contracts beyond what plain pthread guarantees.
 */

#ifndef VMAF_COMPAT_WIN32_PTHREAD_H_
#define VMAF_COMPAT_WIN32_PTHREAD_H_

#ifndef _WIN32
#error "win32/pthread.h shim included on a non-Windows target"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "pthread_timeout.h"

typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef void *pthread_attr_t;
typedef void *pthread_mutexattr_t;
typedef void *pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

/* The SYCL device pass compiles this header for a SPIR-V target, which has no calling
 * conventions: clang ignores __stdcall there and warns (-Wignored-attributes). The host pass
 * keeps the Win32 convention the OS entry points need. */
#if defined(__SYCL_DEVICE_ONLY__)
#define VMAF_W32_CALLBACK
#define VMAF_W32_STDCALL
#else
#define VMAF_W32_CALLBACK CALLBACK
#define VMAF_W32_STDCALL __stdcall
#endif

/* pthread_once — maps to Win32 INIT_ONCE / InitOnceExecuteOnce.
 * Used by iqa/ssim_tools.c (ADR-0871), float_ssim.c / float_ms_ssim.c,
 * cuda/dispatch_strategy.c (ADR-0181), and feature/integer_adm.h.
 * The POSIX contract: the callback runs exactly once across all threads;
 * losing threads block until it completes, then observe the full store-
 * barrier before proceeding.  InitOnceExecuteOnce provides the same
 * guarantee via the Windows kernel executive. */
typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

typedef struct vmaf_w32_pthread_once_ctx {
    void (*fn)(void);
} vmaf_w32_pthread_once_ctx_t;

static BOOL VMAF_W32_CALLBACK vmaf_w32_pthread_once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)ctx;
    ((vmaf_w32_pthread_once_ctx_t *)param)->fn();
    return TRUE;
}

static inline int pthread_once(pthread_once_t *once, void (*init_routine)(void))
{
    vmaf_w32_pthread_once_ctx_t ctx = {init_routine};
    return InitOnceExecuteOnce(once, vmaf_w32_pthread_once_cb, &ctx, NULL) ? 0 : EINVAL;
}

typedef struct vmaf_w32_pthread_trampoline {
    void *(*start)(void *);
    void *arg;
} vmaf_w32_pthread_trampoline_t;

static unsigned VMAF_W32_STDCALL vmaf_w32_pthread_runner(void *raw)
{
    vmaf_w32_pthread_trampoline_t local = *(vmaf_w32_pthread_trampoline_t *)raw;
    free(raw);
    (void)local.start(local.arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void *(*start_routine)(void *), void *arg)
{
    (void)attr;
    if (!thread || !start_routine)
        return EINVAL;
    vmaf_w32_pthread_trampoline_t *tramp = (vmaf_w32_pthread_trampoline_t *)malloc(sizeof(*tramp));
    if (!tramp)
        return ENOMEM;
    tramp->start = start_routine;
    tramp->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, vmaf_w32_pthread_runner, tramp, 0, NULL);
    if (h == 0) {
        int err = errno ? errno : EAGAIN;
        free(tramp);
        return err;
    }
    *thread = (HANDLE)h;
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval)
{
    if (retval)
        *retval = NULL;
    DWORD r = WaitForSingleObject(thread, INFINITE);
    if (r == WAIT_FAILED)
        return EINVAL;
    CloseHandle(thread);
    return 0;
}

static inline int pthread_detach(pthread_t thread)
{
    return CloseHandle(thread) ? 0 : EINVAL;
}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    if (!mutex)
        return EINVAL;
    InitializeSRWLock(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    AcquireSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    (void)attr;
    if (!cond)
        return EINVAL;
    InitializeConditionVariable(cond);
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    return SleepConditionVariableSRW(cond, mutex, INFINITE, 0) ? 0 : EINVAL;
}

/* The current time of the clock a condition variable without attributes
 * measures its deadline against: CLOCK_REALTIME, which is TIME_UTC. */
static inline int vmaf_w32_realtime(struct timespec *now)
{
    return (timespec_get(now, TIME_UTC) == TIME_UTC) ? 0 : EINVAL;
}

/*
 * POSIX timed wait over SleepConditionVariableSRW(). `abstime` is absolute on
 * CLOCK_REALTIME, the clock of a condition variable without attributes (the
 * only kind this shim creates); it is turned into a relative timeout rounded
 * up to milliseconds. Returns 0 on a wake, spurious ones included, as POSIX
 * allows: the caller re-checks its predicate and waits again. Returns
 * ETIMEDOUT only once the clock has passed `abstime`; a wait that Win32 ends
 * early (or that the cap of vmaf_w32_timeout_ms() cut short) returns 0. The
 * mutex is held again on every return but EINVAL for bad arguments.
 */
static inline int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                         const struct timespec *abstime)
{
    struct timespec now;
    if (!cond || !mutex || !abstime || abstime->tv_nsec < 0 ||
        abstime->tv_nsec >= VMAF_W32_NS_PER_S || vmaf_w32_realtime(&now) != 0) {
        return EINVAL;
    }
    const uint32_t ms = vmaf_w32_timeout_ms(abstime->tv_sec, abstime->tv_nsec, now.tv_sec,
                                            now.tv_nsec, INFINITE - 1u);
    if (ms == 0u) {
        return ETIMEDOUT;
    }
    if (SleepConditionVariableSRW(cond, mutex, (DWORD)ms, 0)) {
        return 0;
    }
    if (GetLastError() != ERROR_TIMEOUT || vmaf_w32_realtime(&now) != 0) {
        return EINVAL;
    }
    return (vmaf_w32_timeout_ms(abstime->tv_sec, abstime->tv_nsec, now.tv_sec, now.tv_nsec,
                                INFINITE - 1u) == 0u) ?
               ETIMEDOUT :
               0;
}

static inline int pthread_cond_signal(pthread_cond_t *cond)
{
    WakeConditionVariable(cond);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *cond)
{
    WakeAllConditionVariable(cond);
    return 0;
}

#endif /* VMAF_COMPAT_WIN32_PTHREAD_H_ */
