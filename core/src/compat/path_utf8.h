/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef VMAF_COMPAT_PATH_UTF8_H_
#define VMAF_COMPAT_PATH_UTF8_H_

#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>

#include "libvmaf/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open a file with a UTF-8 encoded file path.
 * On Windows, converts the UTF-8 path and mode to wide character strings (UTF-16)
 * and invokes _wfopen.
 * On POSIX platforms, delegates directly to fopen.
 *
 * @param path UTF-8 encoded file path. Must not be NULL.
 * @param mode Standard fopen file access mode string. Must not be NULL.
 * @return FILE pointer on success, NULL on failure with errno set.
 */
VMAF_EXPORT FILE *vmaf_fopen_utf8(const char *path, const char *mode);

/**
 * Open a file descriptor with a UTF-8 encoded file path.
 * On Windows, converts the UTF-8 path to a wide character string (UTF-16)
 * and invokes _wopen.
 * On POSIX platforms, delegates directly to open.
 *
 * @param path UTF-8 encoded file path. Must not be NULL.
 * @param flags POSIX/Windows open flags (O_RDONLY, O_WRONLY, O_CREAT, etc.).
 * @param mode File permission mode for created files (e.g. 0644).
 * @return File descriptor >= 0 on success, -1 on failure with errno set.
 */
VMAF_EXPORT int vmaf_open_utf8(const char *path, int flags, int mode);

#ifdef __cplusplus
}
#endif

#endif /* VMAF_COMPAT_PATH_UTF8_H_ */
