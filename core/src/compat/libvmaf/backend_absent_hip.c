/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * libvmaf's HIP functions in a build without the HIP backend, on the VMAFx
 * API (ADR-1852 design section 2.11): the library has no HIP device, so the
 * calls report what libvmaf's stubs reported (0 devices, -ENOSYS). A build
 * with HIP keeps the engine's own functions in libvmafx, a declared exception
 * of core/api/vmafx.toml until the HIP lane of RC4 WP3 lands; meson compiles
 * this file only without HIP.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

int vmaf_hip_available(void)
{
    uint32_t count = 0;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_device_count(VMAFX_BACKEND_HIP, &count, &error);
    vmafx_error_free(error);
    return status == VMAFX_OK && count > 0u;
}

/* The library refuses the HIP device (its error is logged, as libvmaf's stub
 * logged one); a device it made anyway is released. */
int vmaf_hip_state_init(VmafHipState **out, VmafHipConfiguration cfg)
{
    if (out) {
        *out = NULL;
    }
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_HIP;
    desc.index = cfg.device_index;
    VmafxDevice *device = NULL;
    if (vmafx_device_create(&desc, &device, NULL) == VMAFX_OK) {
        vmafx_device_unref(device);
    }
    return -ENOSYS;
}

int vmaf_hip_import_state(VmafContext *ctx, VmafHipState *state)
{
    (void)ctx;
    (void)state;
    return -ENOSYS;
}

void vmaf_hip_state_free(VmafHipState **state)
{
    if (state) {
        *state = NULL;
    }
}

int vmaf_hip_list_devices(void)
{
    uint32_t count = 0;
    (void)vmafx_device_count(VMAFX_BACKEND_HIP, &count, NULL);
    return -ENOSYS;
}

/* NOLINTEND(modernize-use-nullptr) */
