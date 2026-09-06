/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "compat/path_utf8.h"

#define UTF8_PATH_MAX 4096
#define UTF8_MODE_MAX 32

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit where NULL is the canonical null pointer constant, and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

FILE *vmaf_fopen_utf8(const char *path, const char *mode)
{
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }

    assert(path != NULL);
    assert(mode != NULL);

#ifdef _WIN32
    if (strlen(path) >= UTF8_PATH_MAX || strlen(mode) >= UTF8_MODE_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    wchar_t wpath[UTF8_PATH_MAX];
    wchar_t wmode[UTF8_MODE_MAX];

    const int res_path =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, UTF8_PATH_MAX);
    if (res_path == 0) {
        const DWORD err = GetLastError();
        if (err == ERROR_NO_UNICODE_TRANSLATION)
            errno = EILSEQ;
        else if (err == ERROR_INSUFFICIENT_BUFFER)
            errno = ENAMETOOLONG;
        else
            errno = EINVAL;
        return NULL;
    }

    const int res_mode =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, wmode, UTF8_MODE_MAX);
    if (res_mode == 0) {
        errno = EINVAL;
        return NULL;
    }

    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

int vmaf_open_utf8(const char *path, int flags, int mode)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    assert(path != NULL);

#ifdef _WIN32
    if (strlen(path) >= UTF8_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    wchar_t wpath[UTF8_PATH_MAX];
    const int res =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, UTF8_PATH_MAX);
    if (res == 0) {
        const DWORD err = GetLastError();
        if (err == ERROR_NO_UNICODE_TRANSLATION)
            errno = EILSEQ;
        else if (err == ERROR_INSUFFICIENT_BUFFER)
            errno = ENAMETOOLONG;
        else
            errno = EINVAL;
        return -1;
    }

    return _wopen(wpath, flags, mode);
#else
    return open(path, flags, (mode_t)mode);
#endif
}

/* NOLINTEND(modernize-use-nullptr) */
