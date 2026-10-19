/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The deadline arithmetic of the Win32 pthread shim's timed condition wait
 * (core/src/compat/win32/pthread.h), kept apart from <windows.h> so that every
 * host can test it (core/test/test_win32_pthread_timeout.c).
 */

#ifndef VMAF_COMPAT_WIN32_PTHREAD_TIMEOUT_H_
#define VMAF_COMPAT_WIN32_PTHREAD_TIMEOUT_H_

#include <stdint.h>

/* Nanoseconds per second and per millisecond. */
#define VMAF_W32_NS_PER_S INT64_C(1000000000)
#define VMAF_W32_NS_PER_MS INT64_C(1000000)

/*
 * The relative timeout, in milliseconds rounded up, from `now` to `deadline`,
 * both (seconds, nanoseconds) of one clock with the nanoseconds in
 * [0, 10^9). 0 when the deadline is not after `now`. Capped at `cap`, which a
 * caller sets below the value its wait reads as "no timeout" (INFINITE for
 * SleepConditionVariableSRW()); a capped wait returns early and its caller
 * waits again. Rounding up keeps a wait from returning before its deadline.
 */
static inline uint32_t vmaf_w32_timeout_ms(int64_t deadline_s, int64_t deadline_ns, int64_t now_s,
                                           int64_t now_ns, uint32_t cap)
{
    if (deadline_s < now_s || (deadline_s == now_s && deadline_ns <= now_ns)) {
        return 0;
    }
    /* Past cap / 1000 + 1 seconds the cap applies; below it the product
     * cannot overflow. */
    const int64_t seconds = deadline_s - now_s;
    if (seconds > ((int64_t)cap / 1000) + 1) {
        return cap;
    }
    const int64_t ns = (seconds * VMAF_W32_NS_PER_S) + (deadline_ns - now_ns);
    const int64_t ms = (ns + VMAF_W32_NS_PER_MS - 1) / VMAF_W32_NS_PER_MS;
    return (ms >= (int64_t)cap) ? cap : (uint32_t)ms;
}

#endif /* VMAF_COMPAT_WIN32_PTHREAD_TIMEOUT_H_ */
