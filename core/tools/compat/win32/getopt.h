/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Minimal POSIX/GNU getopt_long replacement for Windows MSVC.
 *
 *  MSVC's UCRT ships no <getopt.h>. MinGW provides its own via mingw-w64-crt,
 *  so this shim is only compiled into the fork's tools + test_cli_parse when
 *  the primary pthread.h-probe-style check_header('getopt.h') fails. Linux
 *  and macOS pull <getopt.h> from libc and never reach this header.
 *
 *  Supported surface (matches the subset exercised by libvmaf/tools and the
 *  test_cli_parse test):
 *    - getopt() / getopt_long()
 *    - struct option { name, has_arg, flag, val }
 *    - no_argument / required_argument / optional_argument
 *    - optarg / optind / opterr / optopt globals
 *
 *  Notes / divergences from GNU:
 *    - No POSIXLY_CORRECT / '+' / '-' permute-mode behaviour. Option
 *      permutation (re-ordering argv so non-options move to the end) is
 *      implemented; the GNU "in-order" mode is not.
 *    - getopt_long_only() is not provided (callers don't use it).
 *    - Globals are single-threaded — this matches the POSIX getopt contract
 *      and libvmaf's CLI argument-parsing path is single-threaded anyway.
 */

#ifndef VMAF_COMPAT_WIN32_GETOPT_H
#define VMAF_COMPAT_WIN32_GETOPT_H

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

#define no_argument 0
#define required_argument 1
#define optional_argument 2

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *optstring);
int getopt_long(int argc, char *const argv[], const char *optstring, const struct option *longopts,
                int *longindex);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* VMAF_COMPAT_WIN32_GETOPT_H */
