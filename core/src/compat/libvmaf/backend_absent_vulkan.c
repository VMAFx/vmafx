/**
 *  Copyright 2026 luoye-cpu
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * libvmaf's Vulkan functions in a build without the Vulkan backend, on the
 * VMAFx API (ADR-1852 design section 2.11): the library has no Vulkan device,
 * so the calls report what libvmaf's stubs reported (0 devices, -ENOSYS). A
 * build with Vulkan keeps the engine's own functions in libvmafx, a declared
 * exception of core/api/vmafx.toml until the Vulkan lane of RC4 WP3 lands;
 * meson compiles this file only without Vulkan. There is no
 * VMAFX_BACKEND_VULKAN in the VmafxBackend enum yet, so unlike the HIP / Metal
 * stubs these bodies cannot delegate to the vmafx device API — they report the
 * absence directly.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_vulkan.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

int vmaf_vulkan_available(void)
{
    return 0;
}

int vmaf_vulkan_state_init(VmafVulkanState **out, VmafVulkanConfiguration cfg)
{
    (void)cfg;
    if (out != NULL) {
        *out = NULL;
    }
    return -ENOSYS;
}

int vmaf_vulkan_import_state(VmafContext *ctx, VmafVulkanState *state)
{
    (void)ctx;
    (void)state;
    return -ENOSYS;
}

void vmaf_vulkan_state_free(VmafVulkanState **state)
{
    if (state != NULL) {
        *state = NULL;
    }
}
