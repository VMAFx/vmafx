/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * HIP devices of the VMAFx API (RC4 WP3, ADR-1852 design section 2.7,
 * ADR-2092): enumeration without creating a device, devices opened by index
 * or from the caller's stream (`external[0]`; HIP has no context object, so
 * the device is the stream's), and the engine import of a device into a
 * context (the successor of vmaf_hip_state_init() + vmaf_hip_import_state()).
 *
 * A device owns one library stream (non-blocking, or the caller's): every
 * frame of the device is read on it (ADR-2092, as ADR-2023 item 1 on CUDA).
 */

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "common.h"
#include "libvmaf/libvmaf_hip.h"
#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_hip.h"
#include "vmafx_hip_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#ifdef HAVE_HIPCC
/* The conversion kernels (import_convert.hip, embedded by xxd). */
extern const unsigned char import_convert_hsaco[];
#endif

/* Devices whose names the enumeration keeps for the process (HISS-02). */
#define VMAFX_HIP_MAX_DEVICES 64

/* ---- Runtime ------------------------------------------------------------------ */

VmafxStatus vmafx_hip_status(hipError_t rc)
{
    return rc == hipErrorOutOfMemory ? VMAFX_E_NOMEM : VMAFX_E_DEVICE;
}

VmafxStatus vmafx_hip_failed(const VmafxReport *report, hipError_t rc, uint32_t kind,
                             const char *subject, const char *call)
{
    return VMAFX_FAIL(report, vmafx_hip_status(rc), (int32_t)rc, kind, subject,
                      "backend hip: %s failed: %s (%d)", call, hipGetErrorName(rc), (int)rc);
}

int vmafx_hip_bind(const VmafxHipDevice *dev)
{
    return vmaf_hip_rc_to_errno(hipSetDevice(dev->index));
}

/* ---- Enumeration -------------------------------------------------------------- */

/* Devices the runtime sees, 0 when it reports none; VMAFX_E_DEVICE when it
 * cannot be asked. */
static VmafxStatus visible_devices(const VmafxReport *report, uint32_t *count)
{
    int n = 0;
    const hipError_t rc = hipGetDeviceCount(&n);
    if (rc == hipErrorNoDevice || (rc == hipSuccess && n <= 0)) {
        *count = 0u;
        return VMAFX_OK;
    }
    if (rc != hipSuccess) {
        *count = 0u;
        return vmafx_hip_failed(report, rc, VMAFX_SUBJECT_BACKEND, "hip", "hipGetDeviceCount");
    }
    *count = n > VMAFX_HIP_MAX_DEVICES ? VMAFX_HIP_MAX_DEVICES : (uint32_t)n;
    return VMAFX_OK;
}

VmafxStatus vmafx_hip_device_count(const VmafxReport *report, uint32_t *count)
{
    return visible_devices(report, count);
}

/* Names of the devices, kept for the process (VmafxDeviceInfo.name). */
static char device_names[VMAFX_HIP_MAX_DEVICES][256];
static pthread_mutex_t names_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *device_name(int32_t index)
{
    if (index < 0 || index >= VMAFX_HIP_MAX_DEVICES) {
        return "hip";
    }
    (void)pthread_mutex_lock(&names_lock);
    char *const name = device_names[index];
    if (name[0] == '\0') {
        hipDeviceProp_t prop;
        memset(&prop, 0, sizeof(prop));
        if (hipGetDeviceProperties(&prop, index) == hipSuccess && prop.name[0] != '\0') {
            /* The arch name names the target the device code is built for
             * (gfx1036); the marketing name of an APU names the CPU. */
            (void)snprintf(name, sizeof(device_names[0]), "%.160s (%.64s)", prop.name,
                           prop.gcnArchName);
        } else {
            (void)snprintf(name, sizeof(device_names[0]), "hip device %d", (int)index);
        }
    }
    (void)pthread_mutex_unlock(&names_lock);
    return name;
}

static uint64_t device_memory(int32_t index)
{
    size_t bytes = 0;
    return hipDeviceTotalMem(&bytes, index) == hipSuccess ? (uint64_t)bytes : 0u;
}

/* PCI location of HIP device `index` (UINT32_MAX in each when unknown). */
static void device_pci(int32_t index, uint32_t pci[4])
{
    char bus[32];
    const bool ok = hipDeviceGetPCIBusId(bus, (int)sizeof(bus), index) == hipSuccess;
    vmafx_parse_pci_bus_id(ok ? bus : NULL, pci);
}

/* What a HIP device imports: device pointers and dma-bufs (bound or
 * converted), HIP arrays and GL textures (read out on the device), and
 * LINEAR or DRM format modifier Vulkan memory (as dma-bufs); it waits on
 * NONE, HOST, HIP_EVENT (on its stream), GL_SYNC and SYNC_FILE (on the host)
 * acquire fences and returns HOST and HIP_EVENT release fences. */
static VmafxDeviceInfo hip_info(int32_t index, int32_t reported, uint32_t flags)
{
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    info.backend = VMAFX_BACKEND_HIP;
    info.index = reported;
    info.flags = flags;
    info.memory_kinds = (1u << VMAFX_MEMORY_DEVICE_POINTER) | (1u << VMAFX_MEMORY_DEVICE_ARRAY) |
                        (1u << VMAFX_MEMORY_GL_TEXTURE);
#if defined(__linux__)
    info.memory_kinds |= (1u << VMAFX_MEMORY_DMABUF) | (1u << VMAFX_MEMORY_VULKAN);
#endif
    info.fence_kinds = (1u << VMAFX_FENCE_NONE) | (1u << VMAFX_FENCE_HOST) |
                       (1u << VMAFX_FENCE_HIP_EVENT) | (1u << VMAFX_FENCE_GL_SYNC);
#if defined(__linux__)
    info.fence_kinds |= 1u << VMAFX_FENCE_SYNC_FILE;
#endif
    info.total_memory = device_memory(index);
    info.name = device_name(index);
    device_pci(index, info.pci);
    return info;
}

/* HIP device `index` (0 for -1), or VMAFX_E_NOTFOUND naming it. */
static VmafxStatus device_at(const VmafxReport *report, int32_t index, const char *subject,
                             int32_t *at)
{
    uint32_t n = 0;
    const VmafxStatus status = visible_devices(report, &n);
    *at = index == -1 ? 0 : index;
    if (status == VMAFX_OK && (*at < 0 || (uint32_t)*at >= n)) {
        return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_DEVICE, subject,
                          "backend hip has %u device%s, not %d", (unsigned)n, n == 1u ? "" : "s",
                          (int)index);
    }
    return status;
}

VmafxStatus vmafx_hip_device_info(const VmafxReport *report, int32_t index, VmafxDeviceInfo *info)
{
    int32_t at = 0;
    const VmafxStatus status = device_at(report, index, "index", &at);
    if (status == VMAFX_OK) {
        *info = hip_info(at, at, 0u);
    }
    return status;
}

/* ---- Opening a device ------------------------------------------------------------ */

/* The flags and handles a HIP device takes: no profiling flag (no kernel
 * profiler in this build), the caller's stream in external[0] and nothing
 * in external[1]. */
static VmafxStatus check_hip_desc(const VmafxReport *report, const VmafxDeviceDesc *desc)
{
    if (desc->flags & ~(uint32_t)VMAFX_DEVICE_PROFILING) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.flags",
                          "unknown flag bits 0x%x",
                          (unsigned)(desc->flags & ~(uint32_t)VMAFX_DEVICE_PROFILING));
    }
    if (desc->flags & VMAFX_DEVICE_PROFILING) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.flags",
                          "VMAFX_DEVICE_PROFILING: HIP devices have no kernel profiler in this "
                          "build; use the vendor profiler (docs/api/vmafx/index.md)");
    }
    if (desc->external[1] != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.external[1]",
                          "a HIP device takes one external handle, the caller's hipStream_t in "
                          "external[0] (HIP has no context object)");
    }
    return VMAFX_OK;
}

/* The device and the library stream: the caller's stream (external[0]) and
 * its device, or a new non-blocking stream on the indexed device. */
static VmafxStatus open_stream(const VmafxReport *report, const VmafxDeviceDesc *desc,
                               VmafxHipDevice *dev)
{
    if (desc->external[0] != 0u) {
        /* NOLINTNEXTLINE(performance-no-int-to-ptr): the caller's hipStream_t crosses the ABI as uintptr_t (VmafxDeviceDesc.external, ADR-1929). */
        dev->str = (hipStream_t)desc->external[0];
        dev->by_external = true;
        hipDevice_t device = 0;
        const hipError_t rc = hipStreamGetDevice(dev->str, &device);
        dev->index = (int32_t)device;
        return rc == hipSuccess ? VMAFX_OK :
                                  vmafx_hip_failed(report, rc, VMAFX_SUBJECT_DEVICE,
                                                   "desc.external[0]", "hipStreamGetDevice");
    }
    VmafxStatus status = device_at(report, desc->index, "desc.index", &dev->index);
    hipError_t rc = status == VMAFX_OK ? hipSetDevice(dev->index) : hipSuccess;
    if (status == VMAFX_OK && rc == hipSuccess) {
        rc = hipStreamCreateWithFlags(&dev->str, hipStreamNonBlocking);
        dev->own_stream = rc == hipSuccess;
    }
    if (status == VMAFX_OK && rc != hipSuccess) {
        status = vmafx_hip_failed(report, rc, VMAFX_SUBJECT_DEVICE, "device",
                                  "hipStreamCreateWithFlags");
    }
    return status;
}

/* The conversion kernels on the device (none in a build without device
 * code: semi-planar imports are then refused). */
static VmafxStatus open_kernels(const VmafxReport *report, VmafxHipDevice *dev)
{
#ifdef HAVE_HIPCC
    VmafxHipKernels *const k = &dev->kernels;
    hipError_t rc = hipSetDevice(dev->index);
    if (rc == hipSuccess) {
        rc = hipModuleLoadData(&k->module, import_convert_hsaco);
    }
    if (rc == hipSuccess) {
        rc = hipModuleGetFunction(&k->deint_8, k->module, "vmafx_import_deint_8");
    }
    if (rc == hipSuccess) {
        rc = hipModuleGetFunction(&k->deint_16, k->module, "vmafx_import_deint_16");
    }
    if (rc == hipSuccess) {
        rc = hipModuleGetFunction(&k->shift_16, k->module, "vmafx_import_shift_16");
    }
    if (rc == hipSuccess) {
        rc = hipModuleGetFunction(&k->gather, k->module, "vmafx_import_gather");
    }
    return rc == hipSuccess ?
               VMAFX_OK :
               vmafx_hip_failed(report, rc, VMAFX_SUBJECT_DEVICE, "device", "hipModuleLoadData");
#else
    (void)report;
    (void)dev;
    return VMAFX_OK;
#endif
}

/* Free what open_stream() / open_kernels() made (any subset). */
static void free_device(VmafxHipDevice *dev)
{
    if (dev->str && hipSetDevice(dev->index) == hipSuccess) {
        /* Every frame's work and release runs on the library stream; the
         * caller's other streams are not waited for. */
        (void)hipStreamSynchronize(dev->str);
        vmafx_hip_graves_reap(dev, true);
        if (dev->kernels.module) {
            (void)hipModuleUnload(dev->kernels.module);
        }
        if (dev->own_stream) {
            (void)hipStreamDestroy(dev->str);
        }
    }
    (void)pthread_mutex_destroy(&dev->graves_lock);
    free(dev);
}

static VmafxHipDevice *new_device(void)
{
    VmafxHipDevice *const dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return NULL;
    }
    if (pthread_mutex_init(&dev->graves_lock, NULL) != 0) {
        free(dev);
        return NULL;
    }
    atomic_init(&dev->copy_logged, 0u);
    return dev;
}

VmafxStatus vmafx_hip_device_open(const VmafxReport *report, const VmafxDeviceDesc *desc,
                                  VmafxDevice *device)
{
    VmafxStatus status = check_hip_desc(report, desc);
    VmafxHipDevice *const dev = status == VMAFX_OK ? new_device() : NULL;
    if (status != VMAFX_OK || !dev) {
        return status != VMAFX_OK ? status :
                                    VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_DEVICE,
                                               "device", "cannot allocate a HIP device");
    }
    status = open_stream(report, desc, dev);
    if (status == VMAFX_OK) {
        status = open_kernels(report, dev);
    }
    if (status == VMAFX_OK && hipDeviceGetPCIBusId(dev->pci_bus_id, (int)sizeof(dev->pci_bus_id),
                                                   dev->index) != hipSuccess) {
        dev->pci_bus_id[0] = '\0';
    }
    if (status != VMAFX_OK) {
        free_device(dev);
        return status;
    }
    assert(dev->str != NULL);
    dev->total_memory = device_memory(dev->index);
    (void)snprintf(dev->name, sizeof(dev->name), "%s", device_name(dev->index));
    device->lane = dev;
    device->index = dev->by_external ? -1 : dev->index;
    return VMAFX_OK;
}

void vmafx_hip_device_close(VmafxDevice *device)
{
    if (device && device->lane) {
        free_device(vmafx_hip_dev(device));
        device->lane = NULL;
    }
}

void vmafx_hip_device_describe(const VmafxDevice *device, VmafxDeviceInfo *info)
{
    const VmafxHipDevice *const dev = vmafx_hip_dev(device);
    *info = hip_info(dev->index, dev->by_external ? -1 : dev->index, device->flags);
}

/* ---- Contexts ------------------------------------------------------------------- */

VmafxStatus vmafx_hip_context_attach(const VmafxReport *report, VmafxContext *context,
                                     VmafxDevice *device)
{
    const VmafxHipDevice *const dev = vmafx_hip_dev(device);
    VmafHipConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.device_index = dev->index;
    VmafHipState *state = NULL;
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    int err = vmaf_hip_state_init(&state, cfg);
    if (!err) {
        err = vmaf_hip_import_state(vmafx_context_engine(context), state);
        if (err) {
            vmaf_hip_state_free(&state);
        }
    }
    vmafx_engine_leave(context, previous);
    if (err) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, err, VMAFX_SUBJECT_DEVICE, "device",
                          "backend hip device %d: the engine cannot score on it (%d)",
                          (int)dev->index, err);
    }
    context->lane_state = state;
    return VMAFX_OK;
}

void vmafx_hip_context_detach(VmafxContext *context)
{
    if (context->lane_state) {
        VmafHipState *state = context->lane_state;
        vmaf_hip_state_free(&state);
        context->lane_state = NULL;
    }
}

/* NOLINTEND(modernize-use-nullptr) */
