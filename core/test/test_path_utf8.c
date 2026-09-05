/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Unit tests for Windows UTF-8 path compatibility layer (Netflix#1568, ADR-1182).
 *  Exercises vmaf_fopen_utf8 and vmaf_open_utf8 round-trip write and read-back
 *  with non-ASCII UTF-8 filenames, validation, and error paths.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define CLOSE_FD _close
#define WRITE_FD _write
#define READ_FD _read
#else
#include <unistd.h>
#define CLOSE_FD close
#define WRITE_FD write
#define READ_FD read
#endif

#include "test.h"
#include "compat/path_utf8.h"

static int get_temp_directory(char *out, size_t out_sz)
{
    assert(out != NULL);
    assert(out_sz > 0);

#ifdef _WIN32
    DWORD n = GetTempPathA((DWORD)out_sz, out);
    if (n == 0 || n >= out_sz)
        return -1;
    size_t len = strlen(out);
    if (len > 0 && (out[len - 1] == '/' || out[len - 1] == '\\'))
        out[len - 1] = '\0';
    return 0;
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] == '\0')
        tmp = "/tmp";
    int n = snprintf(out, out_sz, "%s", tmp);
    if (n < 0 || (size_t)n >= out_sz)
        return -1;
    return 0;
#endif
}

static char *test_fopen_utf8_roundtrip(void)
{
    char tmpdir[1024];
    int rc = get_temp_directory(tmpdir, sizeof(tmpdir));
    mu_assert("failed to retrieve temporary directory", rc == 0);

    /* Construct UTF-8 path containing non-ASCII characters: é (\xC3\xA9) and 日 (\xE6\x97\xA5) */
    char filepath[2048];
    rc = snprintf(filepath, sizeof(filepath), "%s/vmaf_\xC3\xA9\xE6\x97\xA5.json", tmpdir);
    mu_assert("snprintf filepath overflow", rc > 0 && (size_t)rc < sizeof(filepath));

    const char payload[] = "{\"vmaf_version\":\"utf8_test\",\"score\":98.500000}\n";
    const size_t payload_len = strlen(payload);
    assert(payload_len > 0);

    /* Write using vmaf_fopen_utf8 */
    FILE *fout = vmaf_fopen_utf8(filepath, "wb");
    mu_assert("vmaf_fopen_utf8 failed to open for writing", fout != NULL);
    size_t written = fwrite(payload, 1, payload_len, fout);
    mu_assert("fwrite failed to write full payload", written == payload_len);
    rc = fclose(fout);
    mu_assert("fclose write handle failed", rc == 0);

#ifdef _WIN32
    /* On Windows, additionally verify with wide Win32 API GetFileAttributesW */
    wchar_t wpath[2048];
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filepath, -1, wpath, 2048);
    mu_assert("MultiByteToWideChar failed on created path", wlen > 0);
    DWORD attrs = GetFileAttributesW(wpath);
    mu_assert("GetFileAttributesW failed on wide UTF-8 path", attrs != INVALID_FILE_ATTRIBUTES);
#endif

    /* Read back using vmaf_fopen_utf8 */
    FILE *fin = vmaf_fopen_utf8(filepath, "rb");
    mu_assert("vmaf_fopen_utf8 failed to open for reading", fin != NULL);
    char read_buf[256];
    memset(read_buf, 0, sizeof(read_buf));
    size_t read_bytes = fread(read_buf, 1, sizeof(read_buf) - 1, fin);
    mu_assert("fread failed to read payload", read_bytes == payload_len);
    rc = fclose(fin);
    mu_assert("fclose read handle failed", rc == 0);

    mu_assert("read payload does not match written payload",
              memcmp(read_buf, payload, payload_len) == 0);

    /* Clean up temporary file */
    rc = remove(filepath);
    mu_assert("remove temp file failed", rc == 0);

    return NULL;
}

static char *test_open_utf8_roundtrip(void)
{
    char tmpdir[1024];
    int rc = get_temp_directory(tmpdir, sizeof(tmpdir));
    mu_assert("failed to retrieve temporary directory", rc == 0);

    char filepath[2048];
    rc = snprintf(filepath, sizeof(filepath), "%s/vmaf_open_\xC3\xA9\xE6\x97\xA5.bin", tmpdir);
    mu_assert("snprintf filepath overflow", rc > 0 && (size_t)rc < sizeof(filepath));

    const char payload[] = "vmaf_open_utf8 binary roundtrip payload\n";
    const size_t payload_len = strlen(payload);
    assert(payload_len > 0);

    /* Open for writing with O_CREAT */
    int wfd = vmaf_open_utf8(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    mu_assert("vmaf_open_utf8 failed to open for writing", wfd >= 0);
    long n_written = WRITE_FD(wfd, payload, (unsigned int)payload_len);
    mu_assert("write failed", n_written == (long)payload_len);
    rc = CLOSE_FD(wfd);
    mu_assert("close write fd failed", rc == 0);

#ifdef _WIN32
    wchar_t wpath[2048];
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filepath, -1, wpath, 2048);
    mu_assert("MultiByteToWideChar failed on created path", wlen > 0);
    DWORD attrs = GetFileAttributesW(wpath);
    mu_assert("GetFileAttributesW failed on wide UTF-8 path", attrs != INVALID_FILE_ATTRIBUTES);
#endif

    /* Read back with vmaf_open_utf8 */
    int rfd = vmaf_open_utf8(filepath, O_RDONLY, 0);
    mu_assert("vmaf_open_utf8 failed to open for reading", rfd >= 0);
    char read_buf[256];
    memset(read_buf, 0, sizeof(read_buf));
    long n_read = READ_FD(rfd, read_buf, sizeof(read_buf) - 1);
    mu_assert("read failed", n_read == (long)payload_len);
    rc = CLOSE_FD(rfd);
    mu_assert("close read fd failed", rc == 0);

    mu_assert("read payload mismatch", memcmp(read_buf, payload, payload_len) == 0);

    rc = remove(filepath);
    mu_assert("remove temp file failed", rc == 0);

    return NULL;
}

static char *test_utf8_error_paths(void)
{
    /* NULL path returns NULL / -1 with errno == EINVAL */
    errno = 0;
    FILE *f1 = vmaf_fopen_utf8(NULL, "rb");
    mu_assert("vmaf_fopen_utf8 with NULL path must return NULL", f1 == NULL);
    mu_assert("vmaf_fopen_utf8 with NULL path must set errno=EINVAL", errno == EINVAL);

    errno = 0;
    FILE *f2 = vmaf_fopen_utf8("some_path.txt", NULL);
    mu_assert("vmaf_fopen_utf8 with NULL mode must return NULL", f2 == NULL);
    mu_assert("vmaf_fopen_utf8 with NULL mode must set errno=EINVAL", errno == EINVAL);

    errno = 0;
    int fd1 = vmaf_open_utf8(NULL, O_RDONLY, 0);
    mu_assert("vmaf_open_utf8 with NULL path must return -1", fd1 == -1);
    mu_assert("vmaf_open_utf8 with NULL path must set errno=EINVAL", errno == EINVAL);

    /* Nonexistent file returns ENOENT */
    errno = 0;
    FILE *f3 = vmaf_fopen_utf8("/path/that/definitely/does/not/exist/vmaf_12345.xyz", "rb");
    mu_assert("vmaf_fopen_utf8 nonexistent path must return NULL", f3 == NULL);
    mu_assert("vmaf_fopen_utf8 nonexistent path errno must be set", errno == ENOENT);

    errno = 0;
    int fd2 = vmaf_open_utf8("/path/that/definitely/does/not/exist/vmaf_12345.xyz", O_RDONLY, 0);
    mu_assert("vmaf_open_utf8 nonexistent path must return -1", fd2 == -1);
    mu_assert("vmaf_open_utf8 nonexistent path errno must be set", errno == ENOENT);

#ifdef _WIN32
    /* Invalid UTF-8 sequence should map to EILSEQ on Windows */
    errno = 0;
    FILE *f_bad = vmaf_fopen_utf8("\xFF\xFE\xFD", "rb");
    mu_assert("vmaf_fopen_utf8 with invalid UTF-8 must return NULL", f_bad == NULL);
    mu_assert("vmaf_fopen_utf8 with invalid UTF-8 must set errno=EILSEQ", errno == EILSEQ);

    errno = 0;
    int fd_bad = vmaf_open_utf8("\xFF\xFE\xFD", O_RDONLY, 0);
    mu_assert("vmaf_open_utf8 with invalid UTF-8 must return -1", fd_bad == -1);
    mu_assert("vmaf_open_utf8 with invalid UTF-8 must set errno=EILSEQ", errno == EILSEQ);
#endif

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_fopen_utf8_roundtrip);
    mu_run_test(test_open_utf8_roundtrip);
    mu_run_test(test_utf8_error_paths);
    return NULL;
}
