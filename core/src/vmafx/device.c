/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VmafxDevice (ADR-1852): every frame carries a device from the start (WP2
 * skeleton). The RC4 WP3 common lane adds enumeration, information, external
 * handles and profiling for the CPU device; the backend lanes (CUDA, SYCL,
 * HIP, Metal) create their devices behind these functions, and until a lane
 * lands its backend is refused naming it, never replaced by the CPU.
 *
 * VmafxDeviceInfo is size-prefixed and grows at the end: the RC6 / RC7
 * capability tables append each device's format envelope (PR #2185).
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "error_internal.h"
#include "internal.h"
#include "ref.h"
#include "vmafx/vmafx.h"
#include "config.h"
#ifdef HAVE_CUDA
#include "cuda/vmafx_cuda.h"
#endif
#ifdef HAVE_HIP
#include "hip/vmafx_hip.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The process CPU device: frames created without a device use it. */
static VmafxDevice cpu_device = {
    .refs = NULL, .backend = VMAFX_BACKEND_CPU, .index = 0, .flags = 0u};

static const char *const backend_names[] = {"cpu", "cuda", "sycl", "metal", "hip"};

const char *vmafx_backend_name(uint32_t backend)
{
    return backend < sizeof(backend_names) / sizeof(backend_names[0]) ? backend_names[backend] :
                                                                        "unknown";
}

VmafxDevice *vmafx_device_cpu(void)
{
    return &cpu_device;
}

/* A value of VmafxBackend (5 stays reserved for the removed backend). */
static bool is_backend(uint32_t backend)
{
    return backend <= VMAFX_BACKEND_HIP;
}

/* A backend this build creates devices for: the CPU, and CUDA / HIP in a
 * build with that backend. */
static bool built_backend(uint32_t backend)
{
#ifdef HAVE_CUDA
    if (backend == VMAFX_BACKEND_CUDA) {
        return true;
    }
#endif
#ifdef HAVE_HIP
    if (backend == VMAFX_BACKEND_HIP) {
        return true;
    }
#endif
    return backend == VMAFX_BACKEND_CPU;
}

#ifdef HAVE_CUDA
#define VMAFX_BUILT_CUDA ", CUDA"
#else
#define VMAFX_BUILT_CUDA ""
#endif
#ifdef HAVE_HIP
#define VMAFX_BUILT_HIP ", HIP"
#else
#define VMAFX_BUILT_HIP ""
#endif
#define VMAFX_BUILT_BACKENDS "devices of these backends: CPU" VMAFX_BUILT_CUDA VMAFX_BUILT_HIP

/* `backend` is one this build creates devices for. */
static VmafxStatus check_backend(const VmafxReport *report, uint32_t backend, const char *subject)
{
    if (!is_backend(backend)) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_BACKEND, subject,
                          "backend %u is not a VmafxBackend", (unsigned)backend);
    }
    if (!built_backend(backend)) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_BACKEND, subject,
                          "backend %s: this build creates " VMAFX_BUILT_BACKENDS,
                          vmafx_backend_name(backend));
    }
    return VMAFX_OK;
}

/* The CPU device takes no external handles and has no kernel profiler. */
static VmafxStatus check_cpu_desc(const VmafxReport *report, const VmafxDeviceDesc *d)
{
    if (d->external[0] != 0u || d->external[1] != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.external",
                          "the CPU device takes no external handles");
    }
    if (d->flags & ~(uint32_t)VMAFX_DEVICE_PROFILING) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.flags",
                          "unknown flag bits 0x%x",
                          (unsigned)(d->flags & ~(uint32_t)VMAFX_DEVICE_PROFILING));
    }
    if (d->flags & VMAFX_DEVICE_PROFILING) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.flags",
                          "VMAFX_DEVICE_PROFILING: the CPU device has no kernel profiler");
    }
    if (d->index != 0 && d->index != -1) {
        return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_DEVICE, "desc.index",
                          "the CPU has device 0 only, not %d", (int)d->index);
    }
    return VMAFX_OK;
}

/* The descriptor names a device this build can create. */
static VmafxStatus check_desc(const VmafxReport *report, const VmafxDeviceDesc *d)
{
    const VmafxStatus status = check_backend(report, d->backend, "desc.backend");
    if (status != VMAFX_OK || d->backend != VMAFX_BACKEND_CPU) {
        return status; /* a backend lane checks its own descriptor */
    }
    return check_cpu_desc(report, d);
}

/* Open the backend lane's device (nothing for the CPU). */
static VmafxStatus open_lane(const VmafxReport *report, const VmafxDeviceDesc *d,
                             VmafxDevice *device)
{
#ifdef HAVE_CUDA
    if (d->backend == VMAFX_BACKEND_CUDA) {
        return vmafx_cuda_device_open(report, d, device);
    }
#endif
#ifdef HAVE_HIP
    if (d->backend == VMAFX_BACKEND_HIP) {
        return vmafx_hip_device_open(report, d, device);
    }
#endif
    (void)report;
    assert(d->backend == VMAFX_BACKEND_CPU);
    device->index = 0;
    return VMAFX_OK;
}

/* Close the backend lane's device. */
static void close_lane(VmafxDevice *device)
{
#ifdef HAVE_CUDA
    if (device->backend == VMAFX_BACKEND_CUDA) {
        vmafx_cuda_device_close(device);
    }
#endif
#ifdef HAVE_HIP
    if (device->backend == VMAFX_BACKEND_HIP) {
        vmafx_hip_device_close(device);
    }
#endif
    device->lane = NULL;
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
    VmafxStatus status = VMAFX_OK;
    if (desc) {
        status =
            vmafx_read_sized(&report, &d, (uint32_t)sizeof(d), desc, VMAFX_MIN_DEVICE_DESC, "desc");
    }
    if (status == VMAFX_OK) {
        status = check_desc(&report, &d);
    }
    VmafxDevice *const device = status == VMAFX_OK ? calloc(1, sizeof(*device)) : NULL;
    if (status != VMAFX_OK || !device) {
        return status != VMAFX_OK ? status :
                                    VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_DEVICE,
                                               "device", "cannot allocate a device");
    }
    assert(d.struct_size == sizeof(d));
    device->backend = d.backend;
    device->flags = d.flags;
    status = open_lane(&report, &d, device);
    if (status == VMAFX_OK && vmaf_ref_init(&device->refs) != 0) {
        close_lane(device);
        status = VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_DEVICE, "device",
                            "cannot allocate a reference count");
    }
    if (status != VMAFX_OK) {
        free(device);
        return status;
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
    close_lane(device);
    (void)vmaf_ref_close(device->refs);
    free(device);
}

uint32_t vmafx_device_backend(const VmafxDevice *device)
{
    return device ? device->backend : (uint32_t)VMAFX_BACKEND_CPU;
}

/* ---- Enumeration and information (RC4 WP3) ----------------------------------------- */

VmafxStatus vmafx_device_count(uint32_t backend, uint32_t *count, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!count) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "count",
                          "NULL argument");
    }
    *count = 0u;
    const VmafxStatus status = check_backend(&report, backend, "backend");
#ifdef HAVE_CUDA
    if (status == VMAFX_OK && backend == VMAFX_BACKEND_CUDA) {
        return vmafx_cuda_device_count(&report, count);
    }
#endif
#ifdef HAVE_HIP
    if (status == VMAFX_OK && backend == VMAFX_BACKEND_HIP) {
        return vmafx_hip_device_count(&report, count);
    }
#endif
    /* Every other backend this build has was answered by its lane above. */
    assert(status != VMAFX_OK || backend == VMAFX_BACKEND_CPU);
    if (status == VMAFX_OK) {
        *count = 1u;
    }
    return status;
}

/* What the CPU device is and imports. */
static VmafxDeviceInfo cpu_info(uint32_t flags)
{
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    info.backend = VMAFX_BACKEND_CPU;
    info.index = 0;
    info.flags = flags;
    info.memory_kinds = 1u << VMAFX_MEMORY_HOST;
    info.fence_kinds = (1u << VMAFX_FENCE_NONE) | (1u << VMAFX_FENCE_HOST);
    info.total_memory = 0u;
    info.name = "cpu";
    return info;
}

/* What device `index` of a backend lane this build has is
 * (check_backend() accepted `backend`). */
static VmafxStatus lane_info(const VmafxReport *report, uint32_t backend, int32_t index,
                             VmafxDeviceInfo *info)
{
#ifdef HAVE_CUDA
    if (backend == VMAFX_BACKEND_CUDA) {
        return vmafx_cuda_device_info(report, index, info);
    }
#endif
#ifdef HAVE_HIP
    if (backend == VMAFX_BACKEND_HIP) {
        return vmafx_hip_device_info(report, index, info);
    }
#endif
    (void)index;
    (void)info;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_BACKEND, "backend",
                      "backend %s: this build creates " VMAFX_BUILT_BACKENDS,
                      vmafx_backend_name(backend));
}

VmafxStatus vmafx_device_info(uint32_t backend, int32_t index, VmafxDeviceInfo *out,
                              VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "out",
                          "NULL argument");
    }
    VmafxStatus status = check_backend(&report, backend, "backend");
    if (status != VMAFX_OK) {
        return status;
    }
    if (backend != VMAFX_BACKEND_CPU) {
        VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
        status = lane_info(&report, backend, index, &info);
        return status == VMAFX_OK ?
                   vmafx_write_sized(&report, out, &info, (uint32_t)sizeof(info), "out") :
                   status;
    }
    if (index != 0) {
        return VMAFX_FAIL(&report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_DEVICE, "index",
                          "the CPU has device 0 only, not %d", (int)index);
    }
    const VmafxDeviceInfo info = cpu_info(0u);
    return vmafx_write_sized(&report, out, &info, (uint32_t)sizeof(info), "out");
}

VmafxStatus vmafx_device_describe(const VmafxDevice *device, VmafxDeviceInfo *out,
                                  VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!device || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !device ? "device" : "out", "NULL argument");
    }
    VmafxDeviceInfo info = cpu_info(device->flags);
#ifdef HAVE_CUDA
    if (device->backend == VMAFX_BACKEND_CUDA) {
        vmafx_cuda_device_describe(device, &info);
    }
#endif
#ifdef HAVE_HIP
    if (device->backend == VMAFX_BACKEND_HIP) {
        vmafx_hip_device_describe(device, &info);
    }
#endif
    assert(info.backend == device->backend);
    return vmafx_write_sized(&report, out, &info, (uint32_t)sizeof(info), "out");
}

VmafxStatus vmafx_device_profile(VmafxDevice *device, const char **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!device || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !device ? "device" : "out", "NULL argument");
    }
    *out = NULL;
    return VMAFX_FAIL(&report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_DEVICE, "device",
                      "backend %s device %d was not created with VMAFX_DEVICE_PROFILING",
                      vmafx_backend_name(device->backend), (int)device->index);
}

/* NOLINTEND(modernize-use-nullptr) */
