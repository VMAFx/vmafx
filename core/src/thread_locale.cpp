/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
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
 * C++23 implementation of thread-local locale push/pop (ADR-0735).
 *
 * Migration note (ADR-0735 / Wave 5):
 *   - `git mv thread_locale.c thread_locale.cpp` preserves blame.
 *   - The public C API in thread_locale.h is unchanged; both functions
 *     retain their original C signatures and are declared `extern "C"`
 *     via the header guards so every C caller links without modification.
 *   - `VmafThreadLocaleState` is now a proper C++ class with a destructor
 *     that encapsulates the platform-specific teardown logic, replacing
 *     the platform-`#ifdef` ladder in the original `vmaf_thread_locale_pop`.
 *   - `std::unique_ptr<VmafThreadLocaleState>` in `vmaf_thread_locale_push_c`
 *     guards the allocation so that early-return error paths cannot leak the
 *     state object (previously required explicit `free(state)` on each path).
 *   - `release()` transfers ownership to the returned raw pointer so the
 *     caller's existing lifetime model (pass the pointer to `_pop`) is
 *     preserved without any API change.
 *   - `std::array<char, 256>` replaces the C `char old_locale[256]` on
 *     Windows to get bounds-safe `.fill(0)` initialisation and `std::string_view`
 *     readability on the restore path.
 */

#include "thread_locale.h"
#include "config.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>

#ifdef HAVE_XLOCALE_H
#include <xlocale.h>
#endif

#include <locale.h>

/* Platform-specific locale state — now a C++ class so teardown is
 * encapsulated in the destructor rather than spread across a `#ifdef`
 * ladder in `vmaf_thread_locale_pop`. */
struct VmafThreadLocaleState {
#if defined(HAVE_USELOCALE)
    locale_t c_locale{(locale_t)0};
    locale_t old_locale{(locale_t)0};
#elif defined(_WIN32)
    int old_per_thread_mode{-1};
    std::array<char, 256> old_locale{};
#endif

    VmafThreadLocaleState() = default;

    /* Non-copyable, non-movable: the locale handles are not reference-
     * counted and should not be aliased. */
    VmafThreadLocaleState(const VmafThreadLocaleState &) = delete;
    VmafThreadLocaleState &operator=(const VmafThreadLocaleState &) = delete;

    ~VmafThreadLocaleState()
    {
#if defined(HAVE_USELOCALE)
        if (c_locale != (locale_t)0) {
            uselocale(old_locale);
            freelocale(c_locale);
        }
#elif defined(_WIN32)
        if (old_locale[0] != '\0') {
            setlocale(LC_ALL, old_locale.data());
        }
        if (old_per_thread_mode != -1) {
            _configthreadlocale(old_per_thread_mode);
        }
#endif
    }
};

VmafThreadLocaleState *vmaf_thread_locale_push_c(void)
{
#if defined(HAVE_USELOCALE)
    /* POSIX.1-2008: thread-local locale (Linux, macOS, BSD).
     *
     * macOS note: avoid newlocale(..., NULL). Apple libc allocates an
     * internal _xlocale object for the requested categories; with allocator
     * poisoning enabled, partially initialised category slots can contain
     * poisoned pointer values that uselocale()/fprintf() later dereference.
     * Start from a fully initialised duplicate of the process-global locale
     * and override only numeric formatting, which is the writer requirement.
     * POSIX forbids passing LC_GLOBAL_LOCALE directly as the base locale. */
    locale_t base = duplocale(LC_GLOBAL_LOCALE);
    if (base == (locale_t)0)
        return nullptr;

    auto state = std::make_unique<VmafThreadLocaleState>();
    state->c_locale = newlocale(LC_NUMERIC_MASK, "C", base);
    if (state->c_locale == (locale_t)0) {
        freelocale(base);
        return nullptr;
    }
    state->old_locale = uselocale(state->c_locale);
    return state.release();

#elif defined(_WIN32)
    /* Windows: enable per-thread locale, then set to "C".
     * Use LC_ALL for complete locale isolation.
     *
     * On MinGW64 linked against msvcrt.dll, _configthreadlocale may
     * return -1 because per-thread locale mode is not implemented in
     * that CRT. In that case we fall back to a process-global
     * setlocale — the caller still gets C-locale numeric formatting
     * for the duration of the push/pop window, but without per-thread
     * isolation. We keep old_per_thread_mode at -1 so the destructor
     * skips the restore call (the guard is already in place below). */
    auto state = std::make_unique<VmafThreadLocaleState>();
    state->old_per_thread_mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);

    const char *old = setlocale(LC_ALL, nullptr);
    if (old) {
        /* std::array fill + strncpy: bounds-safe and null-terminates. */
        state->old_locale.fill('\0');
        strncpy(state->old_locale.data(), old, state->old_locale.size() - 1U);
    }

    setlocale(LC_ALL, "C");
    return state.release();

#else
    /* No thread-safe locale support available on this platform. */
    return nullptr;
#endif
}

void vmaf_thread_locale_pop(VmafThreadLocaleState *state)
{
    /* Adopt into unique_ptr so the destructor runs the platform teardown
     * and then frees the allocation — equivalent to the original
     * `if (!state) return; ... free(state)` pattern but leak-safe on
     * future early-return paths. */
    std::unique_ptr<VmafThreadLocaleState> guard(state);
    (void)guard;
}
