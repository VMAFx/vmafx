/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Private bounded-close helper shared by libvmaf tool binaries.
 */

#ifndef LIBVMAF_TOOLS_VMAF_CLOSE_RETRY_H_
#define LIBVMAF_TOOLS_VMAF_CLOSE_RETRY_H_

#include "libvmaf/libvmaf.h"

#define VMAF_TOOL_CLOSE_MAX_ATTEMPTS 2U

#ifdef __cplusplus
extern "C" {
#endif

int vmaf_tool_close_context(VmafContext **vmaf);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_TOOLS_VMAF_CLOSE_RETRY_H_ */
