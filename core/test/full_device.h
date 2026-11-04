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
 * Probe verifies at runtime that /dev/full exists and is a character device.
 * On platforms without /dev/full (Windows, macOS), stat() fails and returns false.
 */

#ifndef VMAF_TEST_FULL_DEVICE_H_
#define VMAF_TEST_FULL_DEVICE_H_

#include <stdbool.h>
#include <sys/stat.h>

#ifndef S_ISCHR
#if defined(S_IFMT) && defined(S_IFCHR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#elif defined(_S_IFMT) && defined(_S_IFCHR)
#define S_ISCHR(m) (((m) & _S_IFMT) == _S_IFCHR)
#endif
#endif

static inline bool vmaf_test_full_device_rejects_writes(void)
{
    struct stat st;
    return stat("/dev/full", &st) == 0 && S_ISCHR(st.st_mode);
}

#endif /* VMAF_TEST_FULL_DEVICE_H_ */
