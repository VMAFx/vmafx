/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Regression test for the `cli_parse.c` long-only-error-fix
 * (ADR-0316, follow-up to ADR-0311 / PR #408 fuzzer-parked
 * crash).
 *
 * Bug: invalid `optarg` for `--threads` / `--subsample` /
 * `--cpumask` (e.g. `--threads abc`, or the `--th=foosoxe`
 * abbreviation captured at
 * `libvmaf/test/fuzz/cli_parse_known_crashes/`) used to trip
 * `error()`'s `assert(long_opts[n].name)` because the
 * call-site passed a synthesised short-option char (`'t'` /
 * `'s'` / `'c'`) that does not appear in `long_opts[]`. Fix
 * passes the long-only enum value (e.g. `ARG_THREADS`)
 * instead, so `error()` finds the matching entry and emits a
 * clean usage() line + `exit(1)` rather than `SIGABRT`.
 *
 * Test strategy: fork(); the child invokes `cli_parse` with
 * the abbreviated bad input, captures stderr to a pipe, and
 * the parent asserts the child exited with status 1 (clean
 * usage error) and printed an "Invalid argument" line — NOT
 * died from SIGABRT. POSIX fork/waitpid is required; the test
 * is wired Windows-off in meson.build alongside
 * `test_y4m_411_oob`.
 */

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test.h"

#include "cli_parse.h"

/* In the child half of the fork: redirect stderr to the pipe and call
 * cli_parse with the bad argv.  Never returns — the child exits via
 * cli_parse → usage() → exit(1) for invalid input, or _exit(2) if
 * cli_parse somehow accepted the bad args silently. */
static void child_parse_via_pipe(int pipefd_write, int argc, char **argv)
{
    (void)dup2(pipefd_write, 2);
    (void)close(pipefd_write);

    CLISettings settings;
    memset(&settings, 0, sizeof(settings));
    optind = 1;
    cli_parse(argc, argv, &settings);
    /* Unreachable on invalid input — cli_parse calls usage() → exit(1).
     * _exit(2) distinguishes "parser silently accepted bad args" from the
     * expected exit(1) so the parent can flag it separately. */
    _exit(2);
}

/* Read up to `cap-1` bytes from `fd` into `buf`, NUL-terminate, then
 * drain any remaining bytes (discarding them) so the writer is never
 * blocked or SIGPIPE'd.  Returns the number of bytes stored in buf.
 *
 * Background (ADR-0523): usage() dumps the full ~4 KiB help text after
 * the "Invalid argument" line.  When the CLI grew past 4 KiB the
 * original 4 KiB buf caused the parent to stop reading mid-message,
 * close the read end, and SIGPIPE the child — an intermittent failure
 * unrelated to the assert bug.  Splitting head (captured) from tail
 * (drained) eliminates the race. */
static size_t read_head_drain_tail(int fd, char *buf, size_t cap)
{
    size_t total = 0;

    /* Head: capture up to cap-1 bytes for needle search. */
    while (total < cap - 1u) {
        const ssize_t n = read(fd, buf + total, cap - 1u - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    buf[total] = '\0';

    /* Tail: discard remainder so the child can finish writing. */
    char drain[256];
    while (read(fd, drain, sizeof(drain)) > 0)
        ; /* discard */

    return total;
}

/* Capture-and-replay test: fork(), in the child run
 * cli_parse(argv) with stderr redirected into a pipe, in the
 * parent read the captured bytes and waitpid() for the exit
 * status. Returns 0 on a clean usage() error (status == 1 +
 * stderr contains `needle`); returns -1 on assert/abort
 * (signal exit) or any other unexpected outcome. */
static int run_parse_expect_usage_error(int argc, char **argv, const char *needle)
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;

    const pid_t pid = fork();
    if (pid < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        (void)close(pipefd[0]);
        child_parse_via_pipe(pipefd[1], argc, argv);
        /* child_parse_via_pipe never returns */
    }

    (void)close(pipefd[1]);

    /* 512 B is enough for the "Invalid argument …" line which always
     * precedes the usage block. */
    char buf[512];
    read_head_drain_tail(pipefd[0], buf, sizeof(buf));
    (void)close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;

    /* The bug shape we're guarding against is SIGABRT from
     * the assert; reject any signal-termination outcome. */
    if (!WIFEXITED(status))
        return -1;
    if (WEXITSTATUS(status) != 1)
        return -1;
    if (strstr(buf, needle) == NULL)
        return -1;
    return 0;
}

static char *test_threads_invalid_optarg_does_not_assert()
{
    char *argv[] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--threads", "abc"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const int rc = run_parse_expect_usage_error(argc, argv, "Invalid argument");
    mu_assert("cli_parse: --threads abc must exit(1) with usage error, not SIGABRT", rc == 0);
    return NULL;
}

static char *test_subsample_invalid_optarg_does_not_assert()
{
    char *argv[] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--subsample", "xyz"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const int rc = run_parse_expect_usage_error(argc, argv, "Invalid argument");
    mu_assert("cli_parse: --subsample xyz must exit(1) with usage error, not SIGABRT", rc == 0);
    return NULL;
}

static char *test_cpumask_invalid_optarg_does_not_assert()
{
    char *argv[] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--cpumask", "qqq"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const int rc = run_parse_expect_usage_error(argc, argv, "Invalid argument");
    mu_assert("cli_parse: --cpumask qqq must exit(1) with usage error, not SIGABRT", rc == 0);
    return NULL;
}

/* Mirrors the parked fuzzer reproducer at
 * libvmaf/test/fuzz/cli_parse_corpus/cli_threads_abbrev_assert.argv:
 * `--th=foosoxe` (getopt unique-prefix abbreviation of
 * `--threads`). This is the exact shape PR #408's fuzzer
 * surfaced; promoting the file to the corpus protects the
 * fuzzer path, this case protects the C unit-test path. */
static char *test_threads_abbrev_does_not_assert()
{
    char *argv[] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--th=foosoxe"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const int rc = run_parse_expect_usage_error(argc, argv, "Invalid argument");
    mu_assert("cli_parse: --th=foosoxe abbrev must exit(1) with usage error, not SIGABRT", rc == 0);
    return NULL;
}

/* ADR-1088: parse_unsigned must reject negative strings.
 * Before the fix, strtoul("-1") returned ULONG_MAX (unsigned wrapping) and the
 * '*end == 0' check passed, so --frame_cnt -1 silently set frame_cnt = UINT_MAX.
 * The fix adds an explicit leading-'-' guard before strtoul. */
static char *test_frame_cnt_negative_is_rejected()
{
    char *argv[] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--frame_cnt", "-1"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const int rc = run_parse_expect_usage_error(argc, argv, "Invalid argument");
    mu_assert("ADR-1088: --frame_cnt -1 must exit(1) with usage error (was silently UINT_MAX)",
              rc == 0);
    return NULL;
}

static char *test_frame_skip_ref_negative_is_rejected()
{
    char *argv[] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--frame_skip_ref", "-5"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const int rc = run_parse_expect_usage_error(argc, argv, "Invalid argument");
    mu_assert("ADR-1088: --frame_skip_ref -5 must exit(1) with usage error", rc == 0);
    return NULL;
}

static char *test_frame_skip_dist_negative_is_rejected()
{
    char *argv[] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--frame_skip_dist", "-1"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const int rc = run_parse_expect_usage_error(argc, argv, "Invalid argument");
    mu_assert("ADR-1088: --frame_skip_dist -1 must exit(1) with usage error", rc == 0);
    return NULL;
}

/* ADR-1088: parse_unsigned must reject values that overflow uint32.
 * On 64-bit hosts, strtoul("5000000000") = 5000000000 which is > UINT_MAX;
 * the overflow check ul > UINT_MAX catches it and emits a usage error. */
static char *test_frame_cnt_overflow_is_rejected()
{
    /* 5 * 10^9 exceeds UINT_MAX (4294967295) on any platform. */
    char *argv[] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--frame_cnt", "5000000000"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const int rc = run_parse_expect_usage_error(argc, argv, "Invalid argument");
    mu_assert("ADR-1088: --frame_cnt 5000000000 must exit(1) (overflows uint32)", rc == 0);
    return NULL;
}

static char *test_threads_negative_is_rejected()
{
    char *argv[] = {"vmaf", "-r", "ref.y4m", "-d", "dis.y4m", "--threads", "-1"};
    const int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    const int rc = run_parse_expect_usage_error(argc, argv, "Invalid argument");
    mu_assert("ADR-1088: --threads -1 must exit(1) with usage error", rc == 0);
    return NULL;
}

char *run_tests()
{
    mu_run_test(test_threads_invalid_optarg_does_not_assert);
    mu_run_test(test_subsample_invalid_optarg_does_not_assert);
    mu_run_test(test_cpumask_invalid_optarg_does_not_assert);
    mu_run_test(test_threads_abbrev_does_not_assert);
    mu_run_test(test_frame_cnt_negative_is_rejected);
    mu_run_test(test_frame_skip_ref_negative_is_rejected);
    mu_run_test(test_frame_skip_dist_negative_is_rejected);
    mu_run_test(test_frame_cnt_overflow_is_rejected);
    mu_run_test(test_threads_negative_is_rejected);
    return NULL;
}
