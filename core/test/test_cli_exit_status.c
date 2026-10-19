/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#include <errno.h>

#include "test.h"

#include "cli_exit_status.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's C23 feature set
 * has no `nullptr` keyword and the required Windows build compiles this file
 * with cl.exe. ADR-1138. */

static char *test_success_is_zero(void)
{
    mu_assert("0 stays 0", vmaf_cli_exit_status(0) == 0);
    return NULL;
}

static char *test_negative_libvmaf_codes_are_modulo_256(void)
{
    mu_assert("-EINVAL is 234", vmaf_cli_exit_status(-EINVAL) == 234);
    mu_assert("-1 is 255", vmaf_cli_exit_status(-1) == 255);
    mu_assert("-ENOMEM is 244", vmaf_cli_exit_status(-ENOMEM) == 244);
    /* ENOSYS is 38 on Linux, 40 in the Windows CRT and 78 on macOS: the status
     * is the platform's code modulo 256 (218, 216 and 178), not a fixed number
     * (T-CLI-EXIT-STATUS-TEST-LINUX-ERRNO-2026-10-06). */
    mu_assert("-ENOSYS is 256 - ENOSYS", vmaf_cli_exit_status(-ENOSYS) == 256 - ENOSYS);
    return NULL;
}

static char *test_documented_positive_codes_pass_through(void)
{
    mu_assert("1 stays 1", vmaf_cli_exit_status(1) == 1);
    mu_assert("100 stays 100", vmaf_cli_exit_status(100) == 100);
    mu_assert("101 stays 101", vmaf_cli_exit_status(101) == 101);
    mu_assert("102 stays 102", vmaf_cli_exit_status(102) == 102);
    mu_assert("255 stays 255", vmaf_cli_exit_status(255) == 255);
    return NULL;
}

static char *test_status_is_never_negative_and_never_a_false_success(void)
{
    static const int probes[] = {
        -2147483647 - 1, -65536, -257, -256, -255, -22, -1, 1, 255, 256, 257, 65536, 2147483647};
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        const int status = vmaf_cli_exit_status(probes[i]);
        mu_assert("status within [1, 255] for a non-zero code", status >= 1 && status <= 255);
    }
    mu_assert("-256 is not reported as success", vmaf_cli_exit_status(-256) == 1);
    mu_assert("256 is not reported as success", vmaf_cli_exit_status(256) == 1);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_success_is_zero);
    mu_run_test(test_negative_libvmaf_codes_are_modulo_256);
    mu_run_test(test_documented_positive_codes_pass_through);
    mu_run_test(test_status_is_never_negative_and_never_a_false_success);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
