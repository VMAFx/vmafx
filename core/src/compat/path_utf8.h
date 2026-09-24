/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAF_COMPAT_PATH_UTF8_H_
#define VMAF_COMPAT_PATH_UTF8_H_

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

#if defined(_WIN32) && !defined(__MINGW32__)
typedef unsigned int mode_t;
#endif

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
FILE *vmaf_fopen_utf8(const char *path, const char *mode);

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
int vmaf_open_utf8(const char *path, int flags, int mode);

/**
 * Minimal, platform-neutral metadata returned by vmaf_path_info_utf8().
 * Keeping the CRT-specific stat structure private avoids the incompatible
 * struct stat / struct _stat64 layouts used by MinGW and MSVC.
 */
struct VmafPathInfo {
    uint64_t size;
    int is_regular;
    int is_directory;
};

#ifndef __cplusplus
typedef struct VmafPathInfo VmafPathInfo;
#endif

/**
 * Canonicalize an existing UTF-8 encoded path into an absolute path.
 * On Windows, converts through UTF-16 and invokes _wfullpath. On POSIX,
 * delegates to realpath. The returned pointer is @p resolved on success.
 *
 * @param path UTF-8 encoded input path. Must not be NULL.
 * @param resolved Caller-owned destination buffer. Must not be NULL.
 * @param resolved_size Size of @p resolved in bytes. Must be greater than 0.
 * @return @p resolved on success, NULL on failure with errno set.
 */
char *vmaf_fullpath_utf8(const char *path, char *resolved, size_t resolved_size);

/**
 * Read filesystem metadata for a UTF-8 encoded path.
 *
 * @param path UTF-8 encoded file or directory path. Must not be NULL.
 * @param info Caller-owned metadata destination. Must not be NULL.
 * @return 0 on success, -1 on failure with errno set.
 */
int vmaf_path_info_utf8(const char *path, VmafPathInfo *info);

/**
 * Create one directory at a UTF-8 encoded path.
 *
 * @param path UTF-8 encoded directory path. Must not be NULL.
 * @param mode POSIX permission bits; ignored by the Windows CRT.
 * @return 0 on success, -1 on failure with errno set.
 */
int vmaf_mkdir_utf8(const char *path, mode_t mode);

/**
 * Remove a file at a UTF-8 encoded path.
 *
 * @param path UTF-8 encoded file path. Must not be NULL.
 * @return 0 on success, -1 on failure with errno set.
 */
int vmaf_remove_utf8(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* VMAF_COMPAT_PATH_UTF8_H_ */
