/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
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
#include <io.h>
#include <windows.h>
#define CLOSE_FD _close
#define WRITE_FD _write
#define READ_FD _read
#else
#include <unistd.h>
#define CLOSE_FD close
#define WRITE_FD write
#define READ_FD read
#endif

#include "compat/path_utf8.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

static int get_temp_directory(char *out, size_t out_sz)
{
    assert(out != NULL);
    assert(out_sz > 0);

#ifdef _WIN32
    wchar_t wdir[1024];
    const DWORD n = GetTempPathW((DWORD)(sizeof(wdir) / sizeof(wdir[0])), wdir);
    if (n == 0 || n >= sizeof(wdir) / sizeof(wdir[0])) {
        return -1;
    }
    const int converted =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wdir, -1, out, (int)out_sz, NULL, NULL);
    if (converted == 0) {
        return -1;
    }
    size_t len = strlen(out);
    if (len > 0 && (out[len - 1] == '/' || out[len - 1] == '\\')) {
        out[len - 1] = '\0';
    }
    return 0;
#else
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup (ADR-0141 / ADR-0278). */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    int n = snprintf(out, out_sz, "%s", tmp);
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return 0;
#endif
}

static int remove_directory_utf8(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[2048];
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, 2048);
    if (converted == 0)
        return -1;
    return RemoveDirectoryW(wpath) ? 0 : -1;
#else
    return rmdir(path);
#endif
}

static unsigned long test_process_id(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

typedef struct Utf8PathFixture {
    char directory[2048];
    char file[2048];
} Utf8PathFixture;

static int prepare_utf8_path_fixture(Utf8PathFixture *fixture)
{
    char tmpdir[1024];
    if (get_temp_directory(tmpdir, sizeof(tmpdir)) != 0)
        return -1;

    int length = snprintf(fixture->directory, sizeof(fixture->directory),
                          "%s/vmaf_dir_\xC3\xA9\xE6\x97\xA5_%lu", tmpdir, test_process_id());
    if (length <= 0 || (size_t)length >= sizeof(fixture->directory))
        return -1;

    (void)remove_directory_utf8(fixture->directory);
    if (vmaf_mkdir_utf8(fixture->directory, 0700) != 0)
        return -1;

    length = snprintf(fixture->file, sizeof(fixture->file), "%s/path_info.bin", fixture->directory);
    if (length <= 0 || (size_t)length >= sizeof(fixture->file)) {
        (void)remove_directory_utf8(fixture->directory);
        return -1;
    }
    return 0;
}

static int write_utf8_probe_file(const char *path)
{
    FILE *const file = vmaf_fopen_utf8(path, "wb");
    if (!file)
        return -1;

    const char byte = 'x';
    const size_t written = fwrite(&byte, 1u, 1u, file);
    const int close_rc = fclose(file);
    return written == 1u && close_rc == 0 ? 0 : -1;
}

static int remove_utf8_path_fixture(const Utf8PathFixture *fixture)
{
    const int remove_rc = vmaf_remove_utf8(fixture->file);
    const int rmdir_rc = remove_directory_utf8(fixture->directory);
    return remove_rc == 0 && rmdir_rc == 0 ? 0 : -1;
}

static char *check_utf8_directory_operations(const Utf8PathFixture *fixture)
{
    VmafPathInfo info;
    const int rc = vmaf_path_info_utf8(fixture->directory, &info);
    mu_assert("vmaf_path_info_utf8 failed for directory", rc == 0);
    mu_assert("UTF-8 directory was not classified as a directory", info.is_directory != 0);
    mu_assert("UTF-8 directory was classified as a regular file", info.is_regular == 0);
    return NULL;
}

static char *check_utf8_file_operations(const Utf8PathFixture *fixture)
{
    const int write_rc = write_utf8_probe_file(fixture->file);
    mu_assert("failed to create file inside UTF-8 directory", write_rc == 0);

    VmafPathInfo info;
    const int info_rc = vmaf_path_info_utf8(fixture->file, &info);
    mu_assert("vmaf_path_info_utf8 failed for regular file", info_rc == 0);
    mu_assert("UTF-8 child was not classified as a regular file", info.is_regular != 0);
    mu_assert("UTF-8 child size mismatch", info.size == 1u);

    char resolved[4096];
    const char *const resolved_ret = vmaf_fullpath_utf8(fixture->file, resolved, sizeof(resolved));
    mu_assert("vmaf_fullpath_utf8 failed", resolved_ret == resolved);
    mu_assert("vmaf_fullpath_utf8 returned an empty path", resolved[0] != '\0');
    return NULL;
}

static int make_utf8_temp_path(char *path, size_t path_size, const char *stem, const char *suffix)
{
    char tmpdir[1024];
    if (get_temp_directory(tmpdir, sizeof(tmpdir)) != 0)
        return -1;

    const int length =
        snprintf(path, path_size, "%s/%s_\xC3\xA9\xE6\x97\xA5%s", tmpdir, stem, suffix);
    return length > 0 && (size_t)length < path_size ? 0 : -1;
}

static char *check_fopen_write_result(size_t written, size_t payload_len, int close_rc)
{
    mu_assert("fwrite failed to write full payload", written == payload_len);
    mu_assert("fclose write handle failed", close_rc == 0);
    return NULL;
}

#ifdef _WIN32
static char *check_win32_created_path(int converted, DWORD attrs)
{
    mu_assert("MultiByteToWideChar failed on created path", converted > 0);
    mu_assert("GetFileAttributesW failed on wide UTF-8 path", attrs != INVALID_FILE_ATTRIBUTES);
    return NULL;
}
#endif

static char *check_fopen_read_result(int opened, size_t bytes, size_t payload_len, int close_rc,
                                     int remove_rc, const char *actual, const char *expected)
{
    mu_assert("vmaf_fopen_utf8 failed to open for reading", opened);
    mu_assert("fread failed to read payload", bytes == payload_len);
    mu_assert("fclose read handle failed", close_rc == 0);
    mu_assert("vmaf_remove_utf8 failed after fopen round-trip", remove_rc == 0);
    mu_assert("read payload does not match written payload",
              memcmp(actual, expected, payload_len) == 0);
    return NULL;
}

static char *check_fopen_utf8_roundtrip(const char *filepath)
{
    const char payload[] = "{\"vmaf_version\":\"utf8_test\",\"score\":98.500000}\n";
    const size_t payload_len = strlen(payload);
    assert(payload_len > 0);

    FILE *fout = vmaf_fopen_utf8(filepath, "wb");
    if (!fout)
        return "vmaf_fopen_utf8 failed to open for writing";
    const size_t written = fwrite(payload, 1, payload_len, fout);
    const int close_write_rc = fclose(fout);

#ifdef _WIN32
    wchar_t wpath[2048];
    const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filepath, -1, wpath, 2048);
    const DWORD attrs = wlen > 0 ? GetFileAttributesW(wpath) : INVALID_FILE_ATTRIBUTES;
#endif

    FILE *fin = vmaf_fopen_utf8(filepath, "rb");
    const int read_opened = fin != NULL;
    char read_buf[256];
    memset(read_buf, 0, sizeof(read_buf));
    const size_t read_bytes = fin ? fread(read_buf, 1, sizeof(read_buf) - 1, fin) : 0u;
    const int close_read_rc = fin ? fclose(fin) : -1;
    const int remove_rc = vmaf_remove_utf8(filepath);

    char *failure = check_fopen_write_result(written, payload_len, close_write_rc);
#ifdef _WIN32
    if (!failure)
        failure = check_win32_created_path(wlen, attrs);
#endif
    if (!failure) {
        failure = check_fopen_read_result(read_opened, read_bytes, payload_len, close_read_rc,
                                          remove_rc, read_buf, payload);
    }
    return failure;
}

static char *test_fopen_utf8_roundtrip(void)
{
    char filepath[2048];
    const int path_rc = make_utf8_temp_path(filepath, sizeof(filepath), "vmaf", ".json");
    mu_assert("failed to construct UTF-8 fopen path", path_rc == 0);
    (void)vmaf_remove_utf8(filepath);
    return check_fopen_utf8_roundtrip(filepath);
}

static char *check_open_write_result(long written, size_t payload_len, int close_rc)
{
    mu_assert("write failed", written == (long)payload_len);
    mu_assert("close write fd failed", close_rc == 0);
    return NULL;
}

static char *check_open_read_result(int opened, long bytes, size_t payload_len, int close_rc,
                                    int remove_rc, const char *actual, const char *expected)
{
    mu_assert("vmaf_open_utf8 failed to open for reading", opened);
    mu_assert("read failed", bytes == (long)payload_len);
    mu_assert("close read fd failed", close_rc == 0);
    mu_assert("vmaf_remove_utf8 failed after open round-trip", remove_rc == 0);
    mu_assert("read payload mismatch", memcmp(actual, expected, payload_len) == 0);
    return NULL;
}

static char *check_open_utf8_roundtrip(const char *filepath)
{
    const char payload[] = "vmaf_open_utf8 binary roundtrip payload\n";
    const size_t payload_len = strlen(payload);
    assert(payload_len > 0);

    const int wfd = vmaf_open_utf8(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0)
        return "vmaf_open_utf8 failed to open for writing";
    const long n_written = WRITE_FD(wfd, payload, (unsigned int)payload_len);
    const int close_write_rc = CLOSE_FD(wfd);

#ifdef _WIN32
    wchar_t wpath[2048];
    const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filepath, -1, wpath, 2048);
    const DWORD attrs = wlen > 0 ? GetFileAttributesW(wpath) : INVALID_FILE_ATTRIBUTES;
#endif

    const int rfd = vmaf_open_utf8(filepath, O_RDONLY, 0);
    const int read_opened = rfd >= 0;
    char read_buf[256];
    memset(read_buf, 0, sizeof(read_buf));
    const long n_read = rfd >= 0 ? READ_FD(rfd, read_buf, sizeof(read_buf) - 1) : -1;
    const int close_read_rc = rfd >= 0 ? CLOSE_FD(rfd) : -1;
    const int remove_rc = vmaf_remove_utf8(filepath);

    char *failure = check_open_write_result(n_written, payload_len, close_write_rc);
#ifdef _WIN32
    if (!failure)
        failure = check_win32_created_path(wlen, attrs);
#endif
    if (!failure) {
        failure = check_open_read_result(read_opened, n_read, payload_len, close_read_rc, remove_rc,
                                         read_buf, payload);
    }
    return failure;
}

static char *test_open_utf8_roundtrip(void)
{
    char filepath[2048];
    const int path_rc = make_utf8_temp_path(filepath, sizeof(filepath), "vmaf_open", ".bin");
    mu_assert("failed to construct UTF-8 descriptor path", path_rc == 0);
    (void)vmaf_remove_utf8(filepath);
    return check_open_utf8_roundtrip(filepath);
}

/* Canonicalization, metadata, and directory creation must use the same UTF-8
 * contract as the openers. This catches partial Windows fixes where _wfopen
 * succeeds but _fullpath/stat/_mkdir still receive UTF-8 through the ANSI CRT. */
static char *test_utf8_path_operations(void)
{
    Utf8PathFixture fixture;
    const int fixture_rc = prepare_utf8_path_fixture(&fixture);
    mu_assert("failed to create UTF-8 path fixture", fixture_rc == 0);

    char *failure = check_utf8_directory_operations(&fixture);
    if (!failure)
        failure = check_utf8_file_operations(&fixture);
    const int cleanup_rc = remove_utf8_path_fixture(&fixture);
    if (failure)
        return failure;
    mu_assert("UTF-8 path fixture cleanup failed", cleanup_rc == 0);
    return NULL;
}

static char *check_utf8_null_stream_errors(void)
{
    errno = 0;
    const FILE *const f1 = vmaf_fopen_utf8(NULL, "rb");
    mu_assert("vmaf_fopen_utf8 with NULL path must return NULL", f1 == NULL);
    mu_assert("vmaf_fopen_utf8 with NULL path must set errno=EINVAL", errno == EINVAL);

    errno = 0;
    const FILE *const f2 = vmaf_fopen_utf8("some_path.txt", NULL);
    mu_assert("vmaf_fopen_utf8 with NULL mode must return NULL", f2 == NULL);
    mu_assert("vmaf_fopen_utf8 with NULL mode must set errno=EINVAL", errno == EINVAL);
    return NULL;
}

static char *check_utf8_null_descriptor_error(void)
{
    errno = 0;
    const int fd1 = vmaf_open_utf8(NULL, O_RDONLY, 0);
    mu_assert("vmaf_open_utf8 with NULL path must return -1", fd1 == -1);
    mu_assert("vmaf_open_utf8 with NULL path must set errno=EINVAL", errno == EINVAL);
    return NULL;
}

static char *check_utf8_null_path_utility_errors(void)
{
    VmafPathInfo info;
    errno = 0;
    int info_rc = vmaf_path_info_utf8(NULL, &info);
    mu_assert("vmaf_path_info_utf8 with NULL path must fail", info_rc == -1);
    mu_assert("vmaf_path_info_utf8 with NULL path must set errno=EINVAL", errno == EINVAL);

    errno = 0;
    int mkdir_rc = vmaf_mkdir_utf8(NULL, 0700);
    mu_assert("vmaf_mkdir_utf8 with NULL path must fail", mkdir_rc == -1);
    mu_assert("vmaf_mkdir_utf8 with NULL path must set errno=EINVAL", errno == EINVAL);

    errno = 0;
    int remove_rc = vmaf_remove_utf8(NULL);
    mu_assert("vmaf_remove_utf8 with NULL path must fail", remove_rc == -1);
    mu_assert("vmaf_remove_utf8 with NULL path must set errno=EINVAL", errno == EINVAL);
    return NULL;
}

static char *check_utf8_null_fullpath_errors(void)
{
    char resolved[128];
    errno = 0;
    const char *result = vmaf_fullpath_utf8(NULL, resolved, sizeof(resolved));
    mu_assert("vmaf_fullpath_utf8 with NULL path must fail", result == NULL);
    mu_assert("vmaf_fullpath_utf8 with NULL path must set errno=EINVAL", errno == EINVAL);

    errno = 0;
    result = vmaf_fullpath_utf8("unused", NULL, sizeof(resolved));
    mu_assert("vmaf_fullpath_utf8 with NULL destination must fail", result == NULL);
    mu_assert("vmaf_fullpath_utf8 with NULL destination must set errno=EINVAL", errno == EINVAL);

    errno = 0;
    result = vmaf_fullpath_utf8("unused", resolved, 0u);
    mu_assert("vmaf_fullpath_utf8 with zero-size destination must fail", result == NULL);
    mu_assert("vmaf_fullpath_utf8 with zero-size destination must set errno=EINVAL",
              errno == EINVAL);
    return NULL;
}

static char *check_utf8_null_error_paths(void)
{
    char *failure = check_utf8_null_stream_errors();
    if (!failure)
        failure = check_utf8_null_descriptor_error();
    if (!failure)
        failure = check_utf8_null_path_utility_errors();
    if (!failure)
        failure = check_utf8_null_fullpath_errors();
    return failure;
}

static char *check_utf8_nonexistent_error_paths(void)
{
    errno = 0;
    const FILE *const f3 =
        vmaf_fopen_utf8("/path/that/definitely/does/not/exist/vmaf_12345.xyz", "rb");
    mu_assert("vmaf_fopen_utf8 nonexistent path must return NULL", f3 == NULL);
    mu_assert("vmaf_fopen_utf8 nonexistent path errno must be set", errno == ENOENT);

    errno = 0;
    int fd2 = vmaf_open_utf8("/path/that/definitely/does/not/exist/vmaf_12345.xyz", O_RDONLY, 0);
    mu_assert("vmaf_open_utf8 nonexistent path must return -1", fd2 == -1);
    mu_assert("vmaf_open_utf8 nonexistent path errno must be set", errno == ENOENT);
    return NULL;
}

#ifdef _WIN32
static char *check_invalid_utf8_error_paths(void)
{
    errno = 0;
    FILE *f_bad = vmaf_fopen_utf8("\xFF\xFE\xFD", "rb");
    mu_assert("vmaf_fopen_utf8 with invalid UTF-8 must return NULL", f_bad == NULL);
    mu_assert("vmaf_fopen_utf8 with invalid UTF-8 must set errno=EILSEQ", errno == EILSEQ);

    errno = 0;
    int fd_bad = vmaf_open_utf8("\xFF\xFE\xFD", O_RDONLY, 0);
    mu_assert("vmaf_open_utf8 with invalid UTF-8 must return -1", fd_bad == -1);
    mu_assert("vmaf_open_utf8 with invalid UTF-8 must set errno=EILSEQ", errno == EILSEQ);

    errno = 0;
    f_bad = vmaf_fopen_utf8("unused", "\xFF\xFE\xFD");
    mu_assert("vmaf_fopen_utf8 with invalid UTF-8 mode must return NULL", f_bad == NULL);
    mu_assert("vmaf_fopen_utf8 with invalid UTF-8 mode must set errno=EILSEQ", errno == EILSEQ);
    return NULL;
}
#endif

static char *test_utf8_error_paths(void)
{
    mu_assert_msg(check_utf8_null_error_paths());
    mu_assert_msg(check_utf8_nonexistent_error_paths());
#ifdef _WIN32
    mu_assert_msg(check_invalid_utf8_error_paths());
#endif
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_fopen_utf8_roundtrip);
    mu_run_test(test_open_utf8_roundtrip);
    mu_run_test(test_utf8_path_operations);
    mu_run_test(test_utf8_error_paths);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
