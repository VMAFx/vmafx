/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * fopen() for tests, including those that link only the public library and so
 * cannot call compat/path_utf8.c's vmaf_fopen_utf8(). On Windows it is
 * _fsopen() with _SH_DENYNO, the sharing fopen() gives: the C runtime declares
 * fopen() deprecated (C4996 under cl.exe, -Wdeprecated-declarations under
 * clang-cl and icx-cl), which fails the Windows legs that build with warnings
 * as errors (ADR-2828). Library code opens named files with vmaf_fopen_utf8().
 * Shared by every test that opens a named file (HISS-19).
 */

#ifndef VMAF_TEST_FOPEN_H_
#define VMAF_TEST_FOPEN_H_

#include <stdio.h>
#ifdef _WIN32
#include <share.h>
#endif

/* `path` opened with `mode`, or NULL with errno set. */
static inline FILE *vmaf_test_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    return _fsopen(path, mode, _SH_DENYNO);
#else
    return fopen(path, mode);
#endif
}

#endif /* VMAF_TEST_FOPEN_H_ */
