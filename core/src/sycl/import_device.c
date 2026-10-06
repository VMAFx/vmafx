/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * SYCL devices of the VMAFx API (RC4 WP3, ADR-1852 design section 2.7,
 * ADR-2091): enumeration without creating a device, devices opened by index
 * (a Level Zero GPU) or on the context and device of the caller's queue, and
 * the engine state of a context on the device (the successor of
 * vmaf_sycl_state_init() + vmaf_sycl_import_state()).
 *
 * The devices are the Level Zero GPUs: the lane's imports (dma-bufs, native
 * handles) go through Level Zero, so another backend's view of the same GPU
 * is not counted twice.
 */

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "libvmaf/libvmaf_sycl.h"
#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_sycl.h"
#include "vmafx_sycl_internal.h"
#include "vmafx_sycl_rt.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* A failed runtime call, named. */
static VmafxStatus runtime_failed(const VmafxReport *report, int err, const char *subject,
                                  const char *what)
{
    return VMAFX_FAIL(report, err == -ENOMEM ? VMAFX_E_NOMEM : VMAFX_E_DEVICE, err,
                      VMAFX_SUBJECT_DEVICE, subject, "backend sycl: %s failed (%d)", what, err);
}

/* ---- Enumeration -------------------------------------------------------------- */

VmafxStatus vmafx_sycl_device_count(const VmafxReport *report, uint32_t *count)
{
    const int err = vmafx_sycl_rt_count(count);
    return err ? runtime_failed(report, err, "sycl", "device enumeration") : VMAFX_OK;
}

/* What a SYCL device imports: USM pointers (bound for any layout: the
 * extractors copy rows on the device), dma-bufs (linear bound, Intel Y and
 * Tile4 de-tiled on the device), GL textures and LINEAR or DRM format
 * modifier Vulkan memory (as dma-bufs, Linux); it waits on NONE, HOST,
 * SYCL_EVENT, SYNC_FILE and GL_SYNC acquire fences and returns HOST and
 * SYCL_EVENT release fences. */
static VmafxDeviceInfo sycl_info(int32_t index, uint32_t flags, const char *name, uint64_t memory,
                                 const uint32_t pci[4])
{
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    info.backend = VMAFX_BACKEND_SYCL;
    info.index = index;
    info.flags = flags;
    info.memory_kinds = 1u << VMAFX_MEMORY_DEVICE_POINTER;
    info.fence_kinds =
        (1u << VMAFX_FENCE_NONE) | (1u << VMAFX_FENCE_HOST) | (1u << VMAFX_FENCE_SYCL_EVENT);
#ifdef __linux__
    info.memory_kinds |=
        (1u << VMAFX_MEMORY_DMABUF) | (1u << VMAFX_MEMORY_GL_TEXTURE) | (1u << VMAFX_MEMORY_VULKAN);
    info.fence_kinds |= (1u << VMAFX_FENCE_SYNC_FILE) | (1u << VMAFX_FENCE_GL_SYNC);
#endif
    info.total_memory = memory;
    info.name = name;
    memcpy(info.pci, pci, sizeof(info.pci));
    return info;
}

VmafxStatus vmafx_sycl_device_info(const VmafxReport *report, int32_t index, VmafxDeviceInfo *info)
{
    const char *name = NULL;
    uint64_t memory = 0;
    const int32_t at = index == -1 ? 0 : index;
    uint32_t pci[4];
    int err = vmafx_sycl_rt_info(at, &name, &memory);
    if (!err) {
        err = vmafx_sycl_rt_pci(at, pci);
    }
    if (err == -ENOENT) {
        uint32_t n = 0;
        (void)vmafx_sycl_rt_count(&n);
        return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_DEVICE, "index",
                          "backend sycl has %u Level Zero GPU%s, not %d", (unsigned)n,
                          n == 1u ? "" : "s", (int)index);
    }
    if (err) {
        return runtime_failed(report, err, "index", "device query");
    }
    *info = sycl_info(at, 0u, name, memory, pci);
    return VMAFX_OK;
}

/* ---- Opening a device ------------------------------------------------------------ */

/* The flags and handles a SYCL device takes: no profiling (the runtime's
 * profiler is per engine state, VMAF_SYCL_PROFILE), a queue in external[0]
 * and nothing in external[1]. */
static VmafxStatus check_sycl_desc(const VmafxReport *report, const VmafxDeviceDesc *desc)
{
    if (desc->flags & ~(uint32_t)VMAFX_DEVICE_PROFILING) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.flags",
                          "unknown flag bits 0x%x",
                          (unsigned)(desc->flags & ~(uint32_t)VMAFX_DEVICE_PROFILING));
    }
    if (desc->flags & VMAFX_DEVICE_PROFILING) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.flags",
                          "VMAFX_DEVICE_PROFILING: SYCL devices have no kernel profiler in this "
                          "API; use VMAF_SYCL_PROFILE=1 or the vendor profiler "
                          "(docs/api/vmafx/index.md)");
    }
    if (desc->external[1] != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.external[1]",
                          "a SYCL device takes the caller's queue in external[0] and nothing in "
                          "external[1]");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_sycl_device_open(const VmafxReport *report, const VmafxDeviceDesc *desc,
                                   VmafxDevice *device)
{
    VmafxStatus status = check_sycl_desc(report, desc);
    VmafxSyclDevice *const dev = status == VMAFX_OK ? calloc(1, sizeof(*dev)) : NULL;
    if (status != VMAFX_OK || !dev) {
        return status != VMAFX_OK ? status :
                                    VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_DEVICE,
                                               "device", "cannot allocate a SYCL device");
    }
    atomic_init(&dev->copy_logged, 0u);
    const int err = vmafx_sycl_rt_open(desc->index, desc->external[0], &dev->rt);
    if (err == -ENOENT) {
        uint32_t n = 0;
        (void)vmafx_sycl_rt_count(&n);
        status = VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_DEVICE, "desc.index",
                            "backend sycl has %u Level Zero GPU%s, not %d", (unsigned)n,
                            n == 1u ? "" : "s", (int)desc->index);
    } else if (err) {
        status = runtime_failed(report, err, desc->external[0] ? "desc.external[0]" : "desc.index",
                                "opening the device");
    }
    if (status != VMAFX_OK) {
        free(dev);
        return status;
    }
    assert(dev->rt != NULL);
    device->lane = dev;
    device->index = vmafx_sycl_rt_index(dev->rt);
    return VMAFX_OK;
}

void vmafx_sycl_device_close(VmafxDevice *device)
{
    if (device && device->lane) {
        VmafxSyclDevice *const dev = vmafx_sycl_dev(device);
        vmafx_sycl_rt_close(dev->rt);
        free(dev);
        device->lane = NULL;
    }
}

void vmafx_sycl_device_describe(const VmafxDevice *device, VmafxDeviceInfo *info)
{
    const VmafxSyclDevice *const dev = vmafx_sycl_dev(device);
    uint32_t pci[4];
    vmafx_sycl_rt_device_pci(dev->rt, pci);
    *info = sycl_info(vmafx_sycl_rt_index(dev->rt), device->flags, vmafx_sycl_rt_name(dev->rt),
                      vmafx_sycl_rt_memory(dev->rt), pci);
}

/* ---- Contexts ------------------------------------------------------------------- */

VmafxStatus vmafx_sycl_context_attach(const VmafxReport *report, VmafxContext *context,
                                      VmafxDevice *device)
{
    VmafxSyclDevice *const dev = vmafx_sycl_dev(device);
    VmafSyclState *state = NULL;
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    int err = vmafx_sycl_rt_engine_state(dev->rt, &state);
    if (!err) {
        err = vmaf_sycl_import_state(vmafx_context_engine(context), state);
        if (err) {
            vmaf_sycl_state_free(&state);
        }
    }
    vmafx_engine_leave(previous);
    if (err) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, err, VMAFX_SUBJECT_DEVICE, "device",
                          "backend sycl device %d: the engine cannot score on it (%d)",
                          (int)vmafx_sycl_rt_index(dev->rt), err);
    }
    context->lane_state = state;
    return VMAFX_OK;
}

void vmafx_sycl_context_detach(VmafxContext *context)
{
    if (context->lane_state) {
        VmafSyclState *state = context->lane_state;
        vmaf_sycl_state_free(&state);
        context->lane_state = NULL;
    }
}

/* NOLINTEND(modernize-use-nullptr) */
