/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VmafxDevice skeleton (ADR-1852, RC4 WP2): every frame carries a device from
 * the start. This release creates CPU devices only; the device backends,
 * enumeration and external handles are WP3's (core/src/vmafx/device*.c).
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "error_internal.h"
#include "internal.h"
#include "ref.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The process CPU device: frames created without a device use it. */
static VmafxDevice cpu_device = {.refs = NULL, .backend = VMAFX_BACKEND_CPU, .index = 0};

VmafxDevice *vmafx_device_cpu(void)
{
    return &cpu_device;
}

VmafxStatus vmafx_device_create(const VmafxDeviceDesc *desc, VmafxDevice **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "out",
                          "no place to store the device");
    }
    *out = NULL;
    VmafxDeviceDesc d = VMAFX_DEVICE_DESC_INIT;
    if (desc) {
        const VmafxStatus status =
            vmafx_read_sized(&report, &d, (uint32_t)sizeof(d), desc, VMAFX_MIN_DEVICE_DESC, "desc");
        if (status != VMAFX_OK) {
            return status;
        }
    }
    if (d.backend != VMAFX_BACKEND_CPU) {
        return VMAFX_FAIL(&report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_BACKEND, "desc.backend",
                          "backend %u: this release creates CPU devices only", (unsigned)d.backend);
    }
    if (d.index != 0 && d.index != -1) {
        return VMAFX_FAIL(&report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_DEVICE, "desc.index",
                          "the CPU has device 0 only, not %d", (int)d.index);
    }
    VmafxDevice *const device = malloc(sizeof(*device));
    if (!device) {
        return VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "cannot allocate a device");
    }
    assert(d.backend == VMAFX_BACKEND_CPU && d.struct_size == sizeof(d));
    device->backend = d.backend;
    device->index = 0;
    if (vmaf_ref_init(&device->refs) != 0) {
        free(device);
        return VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "cannot allocate a reference count");
    }
    *out = device;
    return VMAFX_OK;
}

VmafxDevice *vmafx_device_ref(VmafxDevice *device)
{
    if (device && device->refs) {
        vmaf_ref_fetch_increment(device->refs);
    }
    return device;
}

void vmafx_device_unref(VmafxDevice *device)
{
    if (!device || !device->refs) {
        return;
    }
    if (vmaf_ref_fetch_decrement(device->refs) != 1) {
        return;
    }
    (void)vmaf_ref_close(device->refs);
    free(device);
}

uint32_t vmafx_device_backend(const VmafxDevice *device)
{
    return device ? device->backend : (uint32_t)VMAFX_BACKEND_CPU;
}

/* NOLINTEND(modernize-use-nullptr) */
