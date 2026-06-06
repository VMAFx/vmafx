/**
 *
 *  Copyright 2026 Lusoris
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

/*
 * Minimal pthread shim for Windows MSVC builds.
 *
 * Maps the pthread subset used by libvmaf (mutex / cond / thread create+join+
 * detach) onto Win32 SRWLOCK + CONDITION_VARIABLE + _beginthreadex. Activated
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
#include <stdlib.h>

typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef void *pthread_attr_t;
typedef void *pthread_mutexattr_t;
typedef void *pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

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

static BOOL CALLBACK vmaf_w32_pthread_once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
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

static unsigned __stdcall vmaf_w32_pthread_runner(void *raw)
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
