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
 * C++11 implementation of the libvmaf logging subsystem.
 *
 * Migration note (ADR-0722 / Research-0732):
 *   - `git mv log.c log.cpp` preserves blame.
 *   - The public C API in log.h is unchanged; both functions retain their
 *     original C signatures and are declared `extern "C"` in the header,
 *     so every C caller links without modification.
 *   - `nullptr` replaces `NULL` throughout this file.
 *   - Level bounds-clamping uses a typed inline helper instead of raw
 *     ternary comparisons, making intent explicit without requiring C++17
 *     std::clamp (the project baseline is C++11; log.cpp must compile
 *     inline in test targets that inherit the project default).
 *   - The level_str / level_str_color tables are `constexpr` arrays of
 *     `const char *` — zero-overhead, type-safe replacements for the
 *     original C designated-initialiser arrays.
 *   - No exceptions cross the C ABI boundary; all error states are
 *     signalled via the existing return-value or silent-discard convention.
 */

#include "log.h"
#include "libvmaf/libvmaf.h"

#include <array>
#include <cstdarg>
#include <cstdio>

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

/* Clamp `v` to [lo, hi].  A local helper avoids a C++17 std::clamp
 * dependency while keeping the intent explicit and compiler-checkable. */
template <typename T> static constexpr T clamp_val(T v, T lo, T hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void vmaf_set_log_level(enum VmafLogLevel level)
{
    vmaf_log_level = clamp_val(level, VMAF_LOG_LEVEL_NONE, VMAF_LOG_LEVEL_DEBUG);
    istty = isatty(fileno(stderr));
}

/* Per-level display name.  Indices match VmafLogLevel values 1–4;
 * element [0] is unused (NONE is a sentinel, guarded in vmaf_log below). */
static constexpr std::array<const char *, 4> level_str = {{
    /* VMAF_LOG_LEVEL_ERROR   */ "ERROR",
    /* VMAF_LOG_LEVEL_WARNING */ "WARNING",
    /* VMAF_LOG_LEVEL_INFO    */ "INFO",
    /* VMAF_LOG_LEVEL_DEBUG   */ "DEBUG",
}};

/* ANSI colour codes for terminal output. */
static constexpr std::array<const char *, 4> level_str_color = {{
    /* VMAF_LOG_LEVEL_ERROR   */ "\x1B[31m",
    /* VMAF_LOG_LEVEL_WARNING */ "\x1B[33m",
    /* VMAF_LOG_LEVEL_INFO    */ "\x1B[32m",
    /* VMAF_LOG_LEVEL_DEBUG   */ "\x1B[34m",
}};

void vmaf_log(enum VmafLogLevel level, const char *fmt, ...)
{
    if (level <= VMAF_LOG_LEVEL_NONE)
        return;
    if (level > vmaf_log_level)
        return;

    /* level is in [1, 4] here; map to zero-based array index. */
    const std::size_t idx = static_cast<std::size_t>(level) - 1u;

    (void)fprintf(stderr, "%slibvmaf%s %s%s%s ", istty ? "\x1B[35m" : "", istty ? "\x1B[0m" : "",
                  istty ? level_str_color[idx] : "", level_str[idx], istty ? "\x1B[0m" : "");

    va_list args;
    va_start(args, fmt);
    (void)vfprintf(stderr, fmt, args);
    va_end(args);
}
