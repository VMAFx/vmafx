/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
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
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#ifdef _WIN32
static void set_utf8_conversion_errno(void)
{
    const DWORD err = GetLastError();
    if (err == ERROR_NO_UNICODE_TRANSLATION) {
        errno = EILSEQ;
    } else if (err == ERROR_INSUFFICIENT_BUFFER) {
        errno = ENAMETOOLONG;
    } else {
        errno = EINVAL;
    }
}

static int utf8_to_wide(const char *src, wchar_t *dst, size_t dst_len)
{
    if (strlen(src) >= UTF8_PATH_MAX || dst_len > INT_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    const int converted =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, (int)dst_len);
    if (converted == 0) {
        set_utf8_conversion_errno();
        return -1;
    }
    return 0;
}

static int wide_to_utf8(const wchar_t *src, char *dst, size_t dst_len)
{
    if (dst_len > INT_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    const int converted =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, dst, (int)dst_len, NULL, NULL);
    if (converted == 0) {
        set_utf8_conversion_errno();
        return -1;
    }
    return 0;
}
#endif

FILE *vmaf_fopen_utf8(const char *path, const char *mode)
{
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }

    assert(path != NULL);
    assert(mode != NULL);

#ifdef _WIN32
    if (strlen(mode) >= UTF8_MODE_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    wchar_t wpath[UTF8_PATH_MAX];
    wchar_t wmode[UTF8_MODE_MAX];

    if (utf8_to_wide(path, wpath, UTF8_PATH_MAX) != 0) {
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
    wchar_t wpath[UTF8_PATH_MAX];
    if (utf8_to_wide(path, wpath, UTF8_PATH_MAX) != 0) {
        return -1;
    }

    return _wopen(wpath, flags, mode);
#else
    return open(path, flags, (mode_t)mode);
#endif
}

char *vmaf_fullpath_utf8(const char *path, char *resolved, size_t resolved_size)
{
    if (!path || !resolved || resolved_size == 0u) {
        errno = EINVAL;
        return NULL;
    }

    assert(path != NULL);
    assert(resolved != NULL);

#ifdef _WIN32
    wchar_t wpath[UTF8_PATH_MAX];
    wchar_t wresolved[UTF8_PATH_MAX];
    if (utf8_to_wide(path, wpath, UTF8_PATH_MAX) != 0)
        return NULL;
    if (_wfullpath(wresolved, wpath, UTF8_PATH_MAX) == NULL)
        return NULL;
    if (wide_to_utf8(wresolved, resolved, resolved_size) != 0)
        return NULL;
    return resolved;
#else
    char *canonical = realpath(path, NULL);
    if (!canonical)
        return NULL;
    const size_t canonical_len = strlen(canonical);
    if (canonical_len >= resolved_size) {
        free(canonical);
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(resolved, canonical, canonical_len + 1u);
    free(canonical);
    return resolved;
#endif
}

int vmaf_path_info_utf8(const char *path, VmafPathInfo *info)
{
    if (!path || !info) {
        errno = EINVAL;
        return -1;
    }

    assert(path != NULL);
    assert(info != NULL);

#ifdef _WIN32
    wchar_t wpath[UTF8_PATH_MAX];
    if (utf8_to_wide(path, wpath, UTF8_PATH_MAX) != 0)
        return -1;
    struct _stat64 st;
    if (_wstat64(wpath, &st) != 0)
        return -1;
    info->size = st.st_size < 0 ? 0u : (uint64_t)st.st_size;
    info->is_regular = (st.st_mode & _S_IFMT) == _S_IFREG;
    info->is_directory = (st.st_mode & _S_IFMT) == _S_IFDIR;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    info->size = st.st_size < 0 ? 0u : (uint64_t)st.st_size;
    info->is_regular = S_ISREG(st.st_mode);
    info->is_directory = S_ISDIR(st.st_mode);
#endif
    return 0;
}

int vmaf_mkdir_utf8(const char *path, mode_t mode)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    assert(path != NULL);

#ifdef _WIN32
    (void)mode;
    wchar_t wpath[UTF8_PATH_MAX];
    if (utf8_to_wide(path, wpath, UTF8_PATH_MAX) != 0)
        return -1;
    return _wmkdir(wpath);
#else
    return mkdir(path, mode);
#endif
}

int vmaf_remove_utf8(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    assert(path != NULL);

#ifdef _WIN32
    wchar_t wpath[UTF8_PATH_MAX];
    if (utf8_to_wide(path, wpath, UTF8_PATH_MAX) != 0)
        return -1;
    return _wremove(wpath);
#else
    return remove(path);
#endif
}

/* NOLINTEND(modernize-use-nullptr) */
