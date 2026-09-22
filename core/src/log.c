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

#include "libvmaf/libvmaf.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#ifdef _WIN32
/* MSVC provides isatty / fileno via <io.h> (named with leading underscores;
 * the non-underscored aliases stay available for POSIX source portability). */
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

static enum VmafLogLevel vmaf_log_level = VMAF_LOG_LEVEL_INFO;
static int istty = 0;

void vmaf_set_log_level(enum VmafLogLevel level)
{
    level = level < VMAF_LOG_LEVEL_NONE ? VMAF_LOG_LEVEL_NONE : level;
    level = level > VMAF_LOG_LEVEL_DEBUG ? VMAF_LOG_LEVEL_DEBUG : level;
    vmaf_log_level = level;
    istty = isatty(fileno(stderr));
}

static const char *level_str[] = {
    [VMAF_LOG_LEVEL_ERROR] = "ERROR",
    [VMAF_LOG_LEVEL_WARNING] = "WARNING",
    [VMAF_LOG_LEVEL_INFO] = "INFO",
    [VMAF_LOG_LEVEL_DEBUG] = "DEBUG",
};

static const char *level_str_color[] = {
    [VMAF_LOG_LEVEL_ERROR] = "\x1B[31m",
    [VMAF_LOG_LEVEL_WARNING] = "\x1B[33m",
    [VMAF_LOG_LEVEL_INFO] = "\x1B[32m",
    [VMAF_LOG_LEVEL_DEBUG] = "\x1B[34m",
};

void vmaf_log(enum VmafLogLevel level, const char *fmt, ...)
{
    if (level <= VMAF_LOG_LEVEL_NONE)
        return;
    if (level > vmaf_log_level)
        return;

    va_list args;
#if defined(__clang__) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    /* Clang 21 lowers the C23 va_start macro to __builtin_c23_va_start,
     * which its VAList analyzer does not model yet. The traditional builtin
     * has identical initialization semantics and remains available in C23. */
    __builtin_va_start(args, fmt);
#else
    va_start(args, fmt);
#endif
    (void)fprintf(stderr, "%slibvmaf%s %s%s%s ", istty ? "\x1B[35m" : "", istty ? "\x1B[0m" : "",
                  istty ? level_str_color[level] : "", level_str[level], istty ? "\x1B[0m" : "");
    (void)vfprintf(stderr, fmt, args);
    va_end(args);
}
