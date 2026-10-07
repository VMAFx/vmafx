/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAF_COMPAT_CRT_PORTABLE_H_
#define VMAF_COMPAT_CRT_PORTABLE_H_

/*
 * The C runtime calls the Windows CRT deprecates (strdup, close, sscanf,
 * getenv) under the name the Windows CRT itself recommends, and the plain POSIX
 * name everywhere else. Using the CRT's own replacement keeps the Windows build
 * free of C4996 without _CRT_SECURE_NO_WARNINGS or a pragma.
 *
 *   VMAF_STRDUP(s)        strdup(); _strdup() on Windows
 *   VMAF_CLOSE(fd)        close(); _close() on Windows
 *   VMAF_FDOPEN(fd, m)    fdopen(); _fdopen() on Windows
 *   VMAF_SSCANF(...)      sscanf(); sscanf_s() under MSVC. Only for formats
 *                         without %s, %c or %[ (sscanf_s takes a size for those).
 *   vmaf_getenv_portable  getenv(); getenv_s() into a per-thread buffer under
 *                         MSVC. The result is valid until the next call on the
 *                         same thread; NULL when the variable is unset or does
 *                         not fit the 4096-byte buffer.
 *   vmaf_tmpfile_portable tmpfile(); tmpfile_s() under MSVC. NULL on failure.
 *
 * "Under MSVC" means _MSC_VER, which clang-cl and icx-cl define as well: their
 * -Wdeprecated-declarations reads the same CRT deprecations as cl.exe's C4996.
 * A named file is opened with vmaf_fopen_utf8() (compat/path_utf8.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define VMAF_STRDUP _strdup
#define VMAF_CLOSE _close
#define VMAF_FDOPEN _fdopen
#else
#include <unistd.h>
#define VMAF_STRDUP strdup
#define VMAF_CLOSE close
#define VMAF_FDOPEN fdopen
#endif

#if defined(_MSC_VER)
#define VMAF_SSCANF sscanf_s
#else
#define VMAF_SSCANF sscanf
#endif

#ifdef __cplusplus
#define VMAF_CRT_THREAD_LOCAL thread_local
#else
#define VMAF_CRT_THREAD_LOCAL _Thread_local
#endif

#define VMAF_GETENV_BUF 4096

static inline const char *vmaf_getenv_portable(const char *name)
{
#if defined(_MSC_VER)
    static VMAF_CRT_THREAD_LOCAL char buf[VMAF_GETENV_BUF];
    size_t required = 0;
    if (getenv_s(&required, buf, sizeof(buf), name) != 0 || required == 0) {
        return NULL;
    }
    return buf;
#else
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — callers keep their own ADR-0488 / ADR-1155 contract. */
    return getenv(name);
#endif
}

static inline FILE *vmaf_tmpfile_portable(void)
{
#if defined(_MSC_VER)
    FILE *file = NULL;
    return tmpfile_s(&file) == 0 ? file : NULL;
#else
    return tmpfile();
#endif
}

#endif /* VMAF_COMPAT_CRT_PORTABLE_H_ */
