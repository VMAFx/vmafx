/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Shared helper for detecting /dev/full on Linux.
 *
 * A Linux /dev/full character device always fails writes with ENOSPC.
 * Probe verifies that /dev/full exists and is a character device (S_ISCHR).
 * On non-Linux platforms, returns false.
 */

#ifndef VMAF_TEST_FULL_DEVICE_H_
#define VMAF_TEST_FULL_DEVICE_H_

#include <stdbool.h>
#if defined(__linux__)
#include <sys/stat.h>
#endif

static inline bool vmaf_test_full_device_rejects_writes(void)
{
#if !defined(__linux__)
    return false;
#else
    struct stat st;
    return stat("/dev/full", &st) == 0 && S_ISCHR(st.st_mode);
#endif
}

#endif /* VMAF_TEST_FULL_DEVICE_H_ */
