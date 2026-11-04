/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Shared helper for detecting /dev/full.
 *
 * A /dev/full character device always fails writes with ENOSPC.
 * Probe verifies at runtime that /dev/full exists, is a character device,
 * can be opened for writing, and rejects writes with ENOSPC.
 * On platforms without /dev/full (Windows, macOS) or when write access is
 * denied, the probe returns false so callers can SKIP cleanly.
 */

#ifndef VMAF_TEST_FULL_DEVICE_H_
#define VMAF_TEST_FULL_DEVICE_H_

#include <stdbool.h>

#ifndef _WIN32
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include "test_fopen.h"

#ifndef S_ISCHR
#if defined(S_IFMT) && defined(S_IFCHR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#elif defined(_S_IFMT) && defined(_S_IFCHR)
#define S_ISCHR(m) (((m) & _S_IFMT) == _S_IFCHR)
#endif
#endif
#endif /* !_WIN32 */

static inline bool vmaf_test_full_device_rejects_writes(void)
{
#ifdef _WIN32
    return false;
#else
    struct stat st;
    if (stat("/dev/full", &st) != 0 || !S_ISCHR(st.st_mode)) {
        return false;
    }

    FILE *const f = vmaf_test_fopen("/dev/full", "wb");
    if (!f) {
        return false;
    }

    /* fflush() forces the buffered byte out; /dev/full fails it with ENOSPC. */
    errno = 0;
    const int put_res = fputc(0, f);
    const int flush_res = fflush(f);
    const int write_errno = errno;
    if (fclose(f) != 0) {
        return false;
    }

    const bool failed = (put_res == EOF) || (flush_res == EOF);
    return failed && write_errno == ENOSPC;
#endif
}

#endif /* VMAF_TEST_FULL_DEVICE_H_ */
