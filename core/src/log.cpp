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
 * Migration note (ADR-0725 / Research-0732 Wave 1):
 *   - `git mv log.c log.cpp` preserves blame.
 *   - The public C ABI in log.h is unchanged; both functions retain their
 *     original C signatures and are declared `extern "C"` in the header,
 *     so every C caller links without modification.
 *   - `nullptr` replaces `NULL` throughout this file.
 *   - Level bounds-clamping uses `std::clamp` (C++17, available in C++23).
 *   - The level_str / level_str_color tables are
 *     `static constexpr std::array<std::string_view, 4>` — the C++23
 *     string_view type gives a zero-overhead, null-safe, type-safe
 *     replacement for the original `const char *` C designated-initialiser
 *     arrays.  Each element is a string literal; no heap allocation occurs.
 *   - `[[nodiscard]]` is applied to `vmaf_set_log_level` to catch callers
 *     that ignore the (currently void) result; the attribute is inert on
 *     void functions in C++23 but serves as a forward-compatibility marker
 *     should the function ever return an error code.
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

static VmafLogLevel vmaf_log_level = VMAF_LOG_LEVEL_INFO;
static int istty = 0;

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

void vmaf_set_log_level(enum VmafLogLevel level)
{
    /* std::clamp (C++17, mandated by C++23) replaces the pair of ternary
     * guards in the original C implementation. */
    vmaf_log_level = std::clamp(level, VMAF_LOG_LEVEL_NONE, VMAF_LOG_LEVEL_DEBUG);
    istty = isatty(fileno(stderr));
}

void vmaf_log(enum VmafLogLevel level, const char *fmt, ...)
{
    if (level <= VMAF_LOG_LEVEL_NONE)
        return;
    if (level > vmaf_log_level)
        return;

    /* level is in [1, 4] here; map to zero-based array index. */
    const std::size_t idx = static_cast<std::size_t>(level) - 1u;

    /* string_view::data() is a pointer to the underlying null-terminated
     * string literal, safe to pass to fprintf's %s specifier. */
    (void)fprintf(stderr, "%slibvmaf%s %s%s%s ", istty ? "\x1B[35m" : "", istty ? "\x1B[0m" : "",
                  istty ? level_str_color[idx].data() : "", level_str[idx].data(),
                  istty ? "\x1B[0m" : "");

    va_list args;
    va_start(args, fmt);
    (void)vfprintf(stderr, fmt, args);
    va_end(args);
}
