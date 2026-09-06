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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit where NULL is the canonical null pointer constant, and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

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
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test setup (ADR-1143). */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] == '\0')
        tmp = "/tmp";
    int n = snprintf(out, out_sz, "%s", tmp);
    if (n < 0 || (size_t)n >= out_sz)
        return -1;
    return 0;
#endif
}

/* Build "<tempdir>/<basename>" into @p out. Returns a failure message like a
 * test body does, so callers propagate it with `if (msg) return msg;`. */
static char *build_utf8_temp_path(char *out, size_t out_sz, const char *basename)
{
    char tmpdir[1024];
    const int rc = get_temp_directory(tmpdir, sizeof(tmpdir));
    mu_assert("failed to retrieve temporary directory", rc == 0);

    const int n = snprintf(out, out_sz, "%s/%s", tmpdir, basename);
    mu_assert("snprintf filepath overflow", n > 0 && (size_t)n < out_sz);
    return NULL;
}

#ifdef _WIN32
/* Cross-check the created file through the wide Win32 API: this is what proves
 * the shim really produced a UTF-16 name rather than an ACP transliteration. */
static char *assert_wide_path_exists(const char *filepath)
{
    wchar_t wpath[2048];
    const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filepath, -1, wpath, 2048);
    mu_assert("MultiByteToWideChar failed on created path", wlen > 0);
    const DWORD attrs = GetFileAttributesW(wpath);
    mu_assert("GetFileAttributesW failed on wide UTF-8 path", attrs != INVALID_FILE_ATTRIBUTES);
    return NULL;
}
#else
/* POSIX paths are byte sequences; there is no wide API to cross-check against. */
static char *assert_wide_path_exists(const char *filepath)
{
    (void)filepath;
    return NULL;
}
#endif

static char *fopen_utf8_write(const char *filepath, const char *payload, size_t payload_len)
{
    FILE *fout = vmaf_fopen_utf8(filepath, "wb");
    mu_assert("vmaf_fopen_utf8 failed to open for writing", fout != NULL);

    /* Close before asserting on the write so a short write cannot leak the
     * stream out of the test body. */
    const size_t written = fwrite(payload, 1, payload_len, fout);
    const int rc = fclose(fout);
    mu_assert("fwrite failed to write full payload", written == payload_len);
    mu_assert("fclose write handle failed", rc == 0);
    return NULL;
}

static char *fopen_utf8_read_back(const char *filepath, const char *payload, size_t payload_len)
{
    FILE *fin = vmaf_fopen_utf8(filepath, "rb");
    mu_assert("vmaf_fopen_utf8 failed to open for reading", fin != NULL);

    char read_buf[256];
    memset(read_buf, 0, sizeof(read_buf));
    const size_t read_bytes = fread(read_buf, 1, sizeof(read_buf) - 1, fin);
    const int rc = fclose(fin);
    mu_assert("fread failed to read payload", read_bytes == payload_len);
    mu_assert("fclose read handle failed", rc == 0);
    mu_assert("read payload does not match written payload",
              memcmp(read_buf, payload, payload_len) == 0);
    return NULL;
}

static char *test_fopen_utf8_roundtrip(void)
{
    /* UTF-8 path containing non-ASCII characters: é (\xC3\xA9) and 日 (\xE6\x97\xA5) */
    char filepath[2048];
    char *msg = build_utf8_temp_path(filepath, sizeof(filepath), "vmaf_\xC3\xA9\xE6\x97\xA5.json");
    if (msg)
        return msg;

    const char payload[] = "{\"vmaf_version\":\"utf8_test\",\"score\":98.500000}\n";
    const size_t payload_len = strlen(payload);
    assert(payload_len > 0);

    msg = fopen_utf8_write(filepath, payload, payload_len);
    if (msg)
        return msg;
    msg = assert_wide_path_exists(filepath);
    if (msg)
        return msg;
    msg = fopen_utf8_read_back(filepath, payload, payload_len);
    if (msg)
        return msg;

    mu_assert("remove temp file failed", remove(filepath) == 0);
    return NULL;
}

static char *open_utf8_write(const char *filepath, const char *payload, size_t payload_len)
{
    const int wfd = vmaf_open_utf8(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    mu_assert("vmaf_open_utf8 failed to open for writing", wfd >= 0);

    const long n_written = WRITE_FD(wfd, payload, (unsigned int)payload_len);
    const int rc = CLOSE_FD(wfd);
    mu_assert("write failed", n_written == (long)payload_len);
    mu_assert("close write fd failed", rc == 0);
    return NULL;
}

static char *open_utf8_read_back(const char *filepath, const char *payload, size_t payload_len)
{
    const int rfd = vmaf_open_utf8(filepath, O_RDONLY, 0);
    mu_assert("vmaf_open_utf8 failed to open for reading", rfd >= 0);

    char read_buf[256];
    memset(read_buf, 0, sizeof(read_buf));
    const long n_read = READ_FD(rfd, read_buf, sizeof(read_buf) - 1);
    const int rc = CLOSE_FD(rfd);
    mu_assert("read failed", n_read == (long)payload_len);
    mu_assert("close read fd failed", rc == 0);
    mu_assert("read payload mismatch", memcmp(read_buf, payload, payload_len) == 0);
    return NULL;
}

static char *test_open_utf8_roundtrip(void)
{
    char filepath[2048];
    char *msg =
        build_utf8_temp_path(filepath, sizeof(filepath), "vmaf_open_\xC3\xA9\xE6\x97\xA5.bin");
    if (msg)
        return msg;

    const char payload[] = "vmaf_open_utf8 binary roundtrip payload\n";
    const size_t payload_len = strlen(payload);
    assert(payload_len > 0);

    msg = open_utf8_write(filepath, payload, payload_len);
    if (msg)
        return msg;
    msg = assert_wide_path_exists(filepath);
    if (msg)
        return msg;
    msg = open_utf8_read_back(filepath, payload, payload_len);
    if (msg)
        return msg;

    mu_assert("remove temp file failed", remove(filepath) == 0);
    return NULL;
}

/* NULL path / NULL mode must be rejected with errno == EINVAL on every platform. */
static char *test_utf8_null_argument_errors(void)
{
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
    return NULL;
}

/* A well-formed but absent path must surface the OS errno unchanged (ENOENT),
 * i.e. the shim must not swallow it into its own EINVAL. */
static char *test_utf8_missing_path_errors(void)
{
    static const char missing[] = "/path/that/definitely/does/not/exist/vmaf_12345.xyz";

    errno = 0;
    FILE *f3 = vmaf_fopen_utf8(missing, "rb");
    mu_assert("vmaf_fopen_utf8 nonexistent path must return NULL", f3 == NULL);
    mu_assert("vmaf_fopen_utf8 nonexistent path errno must be set", errno == ENOENT);

    errno = 0;
    int fd2 = vmaf_open_utf8(missing, O_RDONLY, 0);
    mu_assert("vmaf_open_utf8 nonexistent path must return -1", fd2 == -1);
    mu_assert("vmaf_open_utf8 nonexistent path errno must be set", errno == ENOENT);
    return NULL;
}

/* Invalid UTF-8 maps to EILSEQ on Windows, where the shim decodes the path
 * itself. POSIX has nothing to decode, so the case does not exist there. */
static char *test_utf8_invalid_sequence_errors(void)
{
#ifdef _WIN32
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
    mu_run_test(test_utf8_null_argument_errors);
    mu_run_test(test_utf8_missing_path_errors);
    mu_run_test(test_utf8_invalid_sequence_errors);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
