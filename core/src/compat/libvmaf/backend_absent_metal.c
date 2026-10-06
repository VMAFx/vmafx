/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * libvmaf's Metal functions in a build without the Metal backend, on the
 * VMAFx API (ADR-1852 design section 2.11): the library has no Metal device,
 * so the calls report what libvmaf's stubs reported (0 devices, -ENOSYS). A
 * build with Metal keeps the engine's own functions in libvmafx, a declared
 * exception of core/api/vmafx.toml until the Metal lane of RC4 WP3 lands;
 * meson compiles this file only without Metal.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* -ENOSYS once the library confirmed it has no Metal device; a device it made
 * anyway is released. */
static int metal_absent(int32_t index)
{
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_METAL;
    desc.index = index;
    VmafxDevice *device = NULL;
    VmafxError *error = NULL;
    if (vmafx_device_create(&desc, &device, &error) == VMAFX_OK) {
        vmafx_device_unref(device);
    }
    vmafx_error_free(error);
    return -ENOSYS;
}

int vmaf_metal_available(void)
{
    uint32_t count = 0;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_device_count(VMAFX_BACKEND_METAL, &count, &error);
    vmafx_error_free(error);
    return status == VMAFX_OK && count > 0u;
}

int vmaf_metal_state_init(VmafMetalState **out, VmafMetalConfiguration cfg)
{
    if (out) {
        *out = NULL;
    }
    return metal_absent((int32_t)cfg.device_index);
}

int vmaf_metal_import_state(VmafContext *ctx, VmafMetalState *state)
{
    (void)ctx;
    (void)state;
    return -ENOSYS;
}

void vmaf_metal_state_free(VmafMetalState **state)
{
    if (state) {
        *state = NULL;
    }
}

int vmaf_metal_list_devices(void)
{
    uint32_t count = 0;
    VmafxError *error = NULL;
    (void)vmafx_device_count(VMAFX_BACKEND_METAL, &count, &error);
    vmafx_error_free(error);
    return -ENOSYS;
}

int vmaf_metal_state_init_external(VmafMetalState **out, VmafMetalExternalHandles handles)
{
    (void)handles;
    if (out) {
        *out = NULL;
    }
    return metal_absent(-1);
}

int vmaf_metal_picture_import(VmafMetalState *state, uintptr_t iosurface, unsigned plane,
                              unsigned w, unsigned h, unsigned bpc, int is_ref, unsigned index)
{
    (void)state;
    (void)iosurface;
    (void)plane;
    (void)w;
    (void)h;
    (void)bpc;
    (void)is_ref;
    (void)index;
    return -ENOSYS;
}

int vmaf_metal_wait_compute(VmafMetalState *state)
{
    (void)state;
    return -ENOSYS;
}

int vmaf_metal_read_imported_pictures(VmafContext *ctx, unsigned index)
{
    (void)ctx;
    (void)index;
    return -ENOSYS;
}

/* NOLINTEND(modernize-use-nullptr) */
