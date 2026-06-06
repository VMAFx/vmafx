/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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
 * C++23 implementation of the libvmaf logging subsystem.
 *
 * Migration note (ADR-0708 — second C++23 pilot after dict.c → dict.cpp):
 *   - `git mv log.c log.cpp` preserves blame; this PR also wires the .cpp
 *     into the build for the first time (the file was previously inert per
 *     PR #205's "Wave 1 INERT" audit — meson still referenced log.c).
 *   - The public C ABI in log.h is unchanged; both functions retain their
 *     original C signatures, log.h carries `extern "C"` guards, and the
 *     definitions below are wrapped in `extern "C"` so the symbols emit
 *     with C name-mangling under any compiler. `nm libvmaf.so | grep
 *     vmaf_log` returns the same two symbols (`vmaf_log`, `vmaf_set_log_level`)
 *     as the prior log.c build.
 *   - Behaviour is byte-identical to the C version: the same fprintf format
 *     string + va_list pass-through writes to stderr unchanged. std::print /
 *     std::format are *not* used because they would change ANSI escape /
 *     %-format semantics versus the existing %s/%d-driven varargs surface;
 *     preserving the printf-style format string is a deliberate
 *     forward-compatibility decision so existing call sites and any
 *     downstream LD_PRELOAD shims keep working.
 *   - Level bounds-clamping uses `std::clamp` (C++17, mandated by C++23)
 *     instead of the original pair of ternary guards.
 *   - The level_str / level_str_color tables are
 *     `static constexpr std::array<std::string_view, 4>` — the C++23
 *     string_view type gives a zero-overhead, null-safe, type-safe
 *     replacement for the original `const char *` C designated-initialiser
 *     arrays.  Each element is a string literal; no heap allocation occurs.
 *   - The static log-level + isatty state remains a pair of process-global
 *     statics; std::atomic was considered for thread-safety but rejected to
 *     match the original C behaviour bit-for-bit (the previous code raced
 *     identically; introducing an atomic would change observable ordering
 *     under TSan). A future ADR can lift this if call-site auditing shows
 *     concurrent writers exist.
 *   - No exceptions cross the C ABI boundary; all error states are
 *     signalled via the existing return-value or silent-discard convention.
 *   - Supersedes ADR-0722 (PR #42) which used C++11 with a local
 *     `clamp_val<T>` template and `const char *` arrays — real C++23
 *     idioms now used throughout.
 */

#include "log.h"
#include "libvmaf/libvmaf.h"

#include <algorithm> /* std::clamp */
#include <array>
#include <cstdarg>
#include <cassert>
#include <cstdio>
#include <string_view>

#ifdef _WIN32
/* MSVC provides isatty / fileno via <io.h> (named with leading underscores;
 * the non-underscored aliases stay available for POSIX source portability). */
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace
{
/* File-local log state. C++23 anonymous-namespace replaces the C `static`
 * file-scope qualifier (clang-tidy misc-use-anonymous-namespace). */
VmafLogLevel vmaf_log_level = VMAF_LOG_LEVEL_INFO;
int istty = 0;
} /* anonymous namespace */

/* Per-level display names.  Indices 0–3 map to VmafLogLevel values 1–4
 * via `idx = static_cast<std::size_t>(level) - 1u`.
 * std::string_view avoids the implicit decay-to-pointer from a raw array
 * and makes the compile-time-fixed size explicit in the type. */
static constexpr std::array<std::string_view, 4> level_str = {{
    /* VMAF_LOG_LEVEL_ERROR   (1) */ "ERROR",
    /* VMAF_LOG_LEVEL_WARNING (2) */ "WARNING",
    /* VMAF_LOG_LEVEL_INFO    (3) */ "INFO",
    /* VMAF_LOG_LEVEL_DEBUG   (4) */ "DEBUG",
}};

/* ANSI colour escape codes for terminal output.
 * string_view is deliberate: the .data() pointer passed to fprintf is a
 * null-terminated string literal, so it satisfies the %s format requirement. */
static constexpr std::array<std::string_view, 4> level_str_color = {{
    /* VMAF_LOG_LEVEL_ERROR   (1) */ "\x1B[31m",
    /* VMAF_LOG_LEVEL_WARNING (2) */ "\x1B[33m",
    /* VMAF_LOG_LEVEL_INFO    (3) */ "\x1B[32m",
    /* VMAF_LOG_LEVEL_DEBUG   (4) */ "\x1B[34m",
}};

extern "C" {

void vmaf_set_log_level(enum VmafLogLevel level)
{
    /* std::clamp (C++17, mandated by C++23) replaces the pair of ternary
     * guards in the original C implementation. */
    vmaf_log_level = std::clamp(level, VMAF_LOG_LEVEL_NONE, VMAF_LOG_LEVEL_DEBUG);
    istty = isatty(fileno(stderr));
}

void vmaf_log(enum VmafLogLevel level, const char *fmt, ...)
{
    /* Cast to int before comparing: an out-of-range enum value (e.g. the
     * sentinel VMAF_LOG_LEVEL_NONE-1 used by test_log to verify silent discard)
     * is not a representable VmafLogLevel enumerator.  Reading it through the
     * enum type in a comparison expression triggers UBSan's enum-invalid-value
     * check.  Casting to int first is value-preserving for all inputs and makes
     * the guard UBSan-clean without changing observable semantics.
     * See ADR-1080 (UBSan enum-invalid-value in vmaf_log / vmaf_option_set). */
    const int level_int = static_cast<int>(level);
    if (level_int <= static_cast<int>(VMAF_LOG_LEVEL_NONE))
        return;
    if (level_int > static_cast<int>(vmaf_log_level))
        return;

    /* level is in [1, 4] here; map to zero-based array index. */
    const std::size_t idx = static_cast<std::size_t>(level_int) - 1u;

    /* Enforce NUL-termination invariant: string_view::data() is safe to pass
     * to fprintf's %s only when the underlying storage is NUL-terminated.
     * These views are constructed from string literals so the byte at
     * data()[size()] is always '\0'.  The asserts make the contract explicit
     * and auditable (Power of 10 #5; adversarial review 2026-05-28 finding #7). */
    assert(level_str[idx].data()[level_str[idx].size()] == '\0');
    assert(level_str_color[idx].data()[level_str_color[idx].size()] == '\0');
    /* NOLINT justification (bugprone-suspicious-stringview-data-usage): the
     * two asserts above prove the .data() pointer is NUL-terminated. The
     * string_views are constructed from string literals; their backing storage
     * always ends in '\0'. Required for printf-family %s pass-through which is
     * the stable libvmaf log surface (ADR-0708 — preserves byte-identical
     * output). */
    (void)fprintf(
        stderr, "%slibvmaf%s %s%s%s ", istty ? "\x1B[35m" : "", istty ? "\x1B[0m" : "",
        istty ? level_str_color[idx].data() // NOLINT(bugprone-suspicious-stringview-data-usage)
                :
                "",
        level_str[idx].data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
        istty ? "\x1B[0m" : "");

    va_list args;
    va_start(args, fmt);
    (void)vfprintf(stderr, fmt, args);
    va_end(args);
}

} /* extern "C" */
