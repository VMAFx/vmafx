/* Copyright 2026 luoye-cpu
 * SPDX-License-Identifier: EUPL-1.2
 *
 * -ENOSYS fallbacks for the public `libvmaf/libvmaf_vulkan.h` surface
 * when libvmaf is built without `-Denable_vulkan=true`. Mirrors the
 * `core/src/hip/stubs.c` pattern: without this TU the entries declared
 * in libvmaf_vulkan.h are not emitted into libvmafx at all (the real
 * implementations live in `vulkan/common.c` + `libvmaf.c` under
 * `#ifdef HAVE_VULKAN`), so any downstream consumer that references the
 * Vulkan entry points fails at link time. The header documents the
 * contract — "every entry point returns -ENOSYS unconditionally" — and
 * these stubs honour it.
 *
 * Meson wires this TU into the engine sources only when
 * `is_vulkan_enabled == false`. When Vulkan is enabled the real
 * implementations in `vulkan/common.c` and `libvmaf.c` take over.
 * `engine_names_gen.h` renames these definitions to `vmaf_engine_*`
 * (VMAFX_ENGINE_EXPORTS_VULKAN is 0 here), the same shape the HIP
 * stubs take.
 */

#include "config.h"

#ifndef HAVE_VULKAN

#include <errno.h>
#include <stddef.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_vulkan.h"
#include "log.h"

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
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "vmaf_vulkan: Vulkan support disabled; rebuild libvmaf with -Denable_vulkan=true\n");
    return -ENOSYS;
}

int vmaf_vulkan_import_state(VmafContext *ctx, VmafVulkanState *state)
{
    (void)ctx;
    (void)state;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "vmaf_vulkan: Vulkan support disabled; rebuild libvmaf with -Denable_vulkan=true\n");
    return -ENOSYS;
}

void vmaf_vulkan_state_free(VmafVulkanState **state)
{
    if (state != NULL) {
        *state = NULL;
    }
}

#endif /* !HAVE_VULKAN */
