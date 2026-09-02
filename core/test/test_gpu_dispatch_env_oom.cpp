/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression test for finding R2-9: a transient allocation failure while
 *  snapshotting a *set* GPU-dispatch env variable must NOT permanently cache
 *  the slot as "unset" for the remainder of the process lifetime.
 *
 *  History: the legacy `gpu_dispatch_env.c` committed `row->var_name =
 *  var_name` (marking the slot populated, since the free-slot search keyed on
 *  var_name) BEFORE the unchecked `strdup(val)`.  On a transient strdup OOM
 *  the slot stayed populated with `value == NULL`, indistinguishable from
 *  "variable was unset at snapshot time".  Every later call then hit the
 *  lock-free fast path and returned NULL forever — the dispatch override was
 *  silently dropped for the whole process.
 *
 *  The shipped C++23 implementation (`gpu_dispatch_env.cpp`, ADR-0858 /
 *  ADR-1068) gates publication on a per-slot `ready` atomic flag that is
 *  store-released LAST, after the value has been written.  If the value's
 *  allocation throws std::bad_alloc, `ready` is never set, the slot is left
 *  free, and a subsequent call retries the snapshot.  This test pins that
 *  contract: it forces the value-string allocation to throw on the first
 *  attempt, then verifies a later attempt (with allocation working again)
 *  recovers the real value rather than caching NULL.
 *
 *  The test is load-bearing: re-introducing the R2-9 pattern (publishing the
 *  slot before — or independently of — a successful value allocation) makes
 *  the recovery `get` return NULL and the assertion fails.
 *
 *  Mechanism: a global `operator new` replacement throws std::bad_alloc while
 *  a thread-unsafe toggle is armed.  This is single-threaded test code; the
 *  toggle is reset before any other allocation matters.
 */

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef _WIN32
static int test_setenv(const char *name, const char *value, int overwrite)
{
    (void)overwrite;
    return _putenv_s(name, value);
}
#define setenv(name, value, overwrite) test_setenv(name, value, overwrite)
#endif

extern "C" {
#include "test.h"
}

#include "gpu_dispatch_env.h"

/* Sanitizer detection. The OOM-injection mechanism below replaces the global
 * `operator new` / `operator delete`. The sanitizer runtimes (TSan, ASan, MSan)
 * ALSO interpose their own global `operator new` / `delete`, so a replacement
 * here collides at link time under lld:
 *   ld.lld: error: duplicate symbol: operator new(unsigned long)
 * Under any sanitizer we therefore skip the replacement entirely and turn the
 * OOM test into a no-op skip. The bug it guards (R2-9: a transient allocation
 * failure on the value snapshot must not poison the dispatch-env slot) is a
 * pure control-flow check the non-sanitized suites still exercise on every
 * push, so coverage is not lost — only this one sanitizer configuration opts
 * out of the global-new override that is fundamentally incompatible with the
 * sanitizer allocator interceptors. */
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_MEMORY__)
#define VMAF_OOM_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define VMAF_OOM_TEST_SANITIZED 1
#endif
#endif

#ifndef VMAF_OOM_TEST_SANITIZED
/* When armed, the next operator new call throws std::bad_alloc.  A plain bool
 * suffices (single-threaded test); std::atomic keeps it tidy and avoids any
 * tearing concern under sanitizers. */
static std::atomic<bool> g_fail_next_new{false};

void *operator new(std::size_t n)
{
    if (g_fail_next_new.load(std::memory_order_relaxed)) {
        g_fail_next_new.store(false, std::memory_order_relaxed);
        throw std::bad_alloc{};
    }
    void *p = std::malloc(n ? n : 1U);
    if (!p)
        throw std::bad_alloc{};
    return p;
}

void operator delete(void *p) noexcept
{
    std::free(p);
}

void operator delete(void *p, std::size_t) noexcept
{
    std::free(p);
}
#endif /* !VMAF_OOM_TEST_SANITIZED */

/* R2-9: a transient OOM on the value snapshot must not poison the slot. */
static mu_message_t test_env_oom_does_not_poison_slot(void)
{
#ifdef VMAF_OOM_TEST_SANITIZED
    /* Skip under sanitizers: the global-new override that arms the fault is
     * compiled out (it duplicates the sanitizer allocator symbols), so the
     * injection point does not exist here. Returning NULL signals a pass. */
    return NULL;
#else
    const char *const var = "VMAFX_TEST_DISPATCH_OOM_R2_9";
    const char *const want = "vif:graph,adm:direct";
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — single-thread test setup. */
    (void)setenv(var, want, 1);

    /* Arm the allocation-failure toggle, then attempt the first snapshot.
     * The shipped .cpp constructs a std::string for the value via operator
     * new, which will throw; the helper must propagate (or otherwise fail
     * to publish) WITHOUT marking the slot populated. */
    bool threw = false;
    g_fail_next_new.store(true, std::memory_order_relaxed);
    try {
        const char *poisoned = vmaf_gpu_dispatch_env_get(var);
        /* If the implementation swallowed the OOM and cached, `poisoned`
         * would be NULL here — that is exactly the R2-9 failure shape.  We
         * do not assert on it directly (an impl that returns getenv() on
         * the failing call is also acceptable) — the load-bearing check is
         * the *recovery* get below. */
        (void)poisoned;
    } catch (const std::bad_alloc &) {
        threw = true;
    }
    /* Disarm regardless of which path executed. */
    g_fail_next_new.store(false, std::memory_order_relaxed);
    (void)threw;

    /* Recovery: with allocation working again, the set variable MUST now
     * snapshot to its real value.  Under the R2-9 bug the slot was cached
     * as unset on the first (failing) call, so this returns NULL forever. */
    const char *recovered = vmaf_gpu_dispatch_env_get(var);
    /* test.h's `mu_assert` returns its message as `char *`. The harness is
     * C-first, where a string literal -> `char *` is legal; this is the only
     * test whose body is a C++ TU, and under MSVC /std:c++latest that
     * conversion is a hard error (C2440) that `-Wno-write-strings` cannot
     * silence (it is an error, not a warning, and that flag is GCC/Clang-only).
     * Hold the messages in `static char[]` buffers (mutable arrays decay to
     * `char *` with no conversion, and static storage keeps the pointer valid
     * after the function returns) so the assert compiles identically on every
     * compiler. */
    static char msg_not_cached[] =
        "set var must not be permanently cached as unset after a transient OOM";
    static char msg_value_match[] = "recovered snapshot value matches the env";
    mu_assert(msg_not_cached, recovered != NULL);
    mu_assert(msg_value_match, strcmp(recovered, want) == 0);
    return NULL;
#endif /* VMAF_OOM_TEST_SANITIZED */
}

extern "C" mu_message_t run_tests(void)
{
    mu_run_test(test_env_oom_does_not_poison_slot);
    return NULL;
}
