/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Bounded libvmaf context close shared by the command-line tools.
 */

#include <errno.h>
#include <stddef.h>

#include "vmaf_close_retry.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

int vmaf_tool_close_context(VmafContext **vmaf)
{
    if (vmaf == NULL)
        return -EINVAL;
    if (*vmaf == NULL)
        return 0;

    int first_err = 0;
    for (unsigned attempt = 0U; attempt < VMAF_TOOL_CLOSE_MAX_ATTEMPTS; attempt++) {
        const int err = vmaf_close(*vmaf);
        if (err == 0) {
            *vmaf = NULL;
            return 0;
        }
        if (first_err == 0)
            first_err = err;
    }
    return first_err;
}

/* NOLINTEND(modernize-use-nullptr) */
