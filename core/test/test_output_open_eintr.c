/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "test.h"
#include "libvmaf/libvmaf.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr`. This
 * Linux-only linker-interposition test still follows the cross-platform test
 * source convention. ADR-1138. */

static const char *eintr_path;
static unsigned target_open_calls;

/* GNU ld --wrap ABI names are fixed. ADR-0141 / ADR-0278. */
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage) */
int __real_open64(const char *path, int flags, ...);

/* GNU ld --wrap ABI names are fixed. ADR-0141 / ADR-0278. */
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage) */
int __wrap_open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (((unsigned)flags & (unsigned)O_CREAT) != 0U) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }

    if (eintr_path && strcmp(path, eintr_path) == 0) {
        target_open_calls++;
        if (target_open_calls == 1) {
            errno = EINTR;
            return -1;
        }
    }

    if (((unsigned)flags & (unsigned)O_CREAT) != 0U)
        return __real_open64(path, flags, mode);
    return __real_open64(path, flags);
}

static int make_output_path(char *path, size_t path_size)
{
    const int length = snprintf(path, path_size, "vmafx_output_eintr_XXXXXX");
    if (length <= 0 || (size_t)length >= path_size)
        return -1;

    const int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    const int close_err = close(fd);
    const int remove_err = remove(path);
    return close_err != 0 || remove_err != 0 ? -1 : 0;
}

static char *test_output_open_retries_eintr()
{
    char path[512];
    int err = make_output_path(path, sizeof(path));
    mu_assert("could not create output path", err == 0);

    VmafContext *vmaf = NULL;
    const VmafConfiguration cfg = {0};
    err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", err == 0);

    eintr_path = path;
    target_open_calls = 0;
    const int write_err = vmaf_write_output(vmaf, path, VMAF_OUTPUT_FORMAT_JSON);
    eintr_path = NULL;
    const unsigned calls = target_open_calls;
    const int close_err = vmaf_close(vmaf);
    const int remove_err = remove(path);

    mu_assert("output open did not recover from EINTR", write_err == 0);
    mu_assert("output open was not attempted exactly twice", calls == 2);
    mu_assert("vmaf_close failed", close_err == 0);
    mu_assert("output file was not created", remove_err == 0);
    return NULL;
}

char *run_tests()
{
    mu_run_test(test_output_open_retries_eintr);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
