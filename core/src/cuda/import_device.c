/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * CUDA devices of the VMAFx API (RC4 WP3, ADR-1852 design section 2.7,
 * ADR-2023): enumeration without creating a device, devices opened by index
 * (the primary context, retained) or from the caller's context and stream,
 * and the engine import of a device into a context (the successor of
 * vmaf_cuda_state_init() + vmaf_cuda_import_state()).
 *
 * The driver library is loaded and initialised once per process and never
 * unloaded: fences of the CUDA kinds are waited on and destroyed without a
 * device (vmafx_fence_wait() / vmafx_fence_destroy() take only the fence).
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "libvmaf/libvmaf_cuda.h"
#include "log.h"
#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_cuda.h"
#include "vmafx_cuda_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The conversion kernels (import_convert.cu, embedded by bin2c). */
extern const unsigned char import_convert_ptx[];

/* Devices whose names the enumeration keeps for the process (HISS-02). */
#define VMAFX_CUDA_MAX_DEVICES 64

/* ---- Driver ------------------------------------------------------------------- */

static VmafxCudaDriver driver;
static pthread_once_t driver_once = PTHREAD_ONCE_INIT;

/* The driver's entry point `name` (dlsym / GetProcAddress hand a function
 * back as an object pointer), or NULL. */
static const void *driver_symbol(CudaFunctions *f, const char *name)
{
    return (const void *)FFNV_SYM_FUNC(f->lib, name);
}

static void driver_load(void)
{
    CudaFunctions *f = NULL;
    if (cuda_load_functions(&f, NULL) != 0 || !f) {
        driver.f = NULL;
        return;
    }
    driver.init = f->cuInit(0);
    /* An entry point of the driver the nv-codec-headers table does not load
     * (dlsym / GetProcAddress hand a function back as an object pointer). */
    const void *const total_memory = driver_symbol(f, "cuDeviceTotalMem_v2");
    memcpy((void *)&driver.total_memory, (const void *)&total_memory, sizeof(driver.total_memory));
    driver.f = f;
}

static const VmafxCudaDriver *driver_loaded(void)
{
    (void)pthread_once(&driver_once, driver_load);
    return driver.f ? &driver : NULL;
}

const VmafxCudaDriver *vmafx_cuda_driver(void)
{
    const VmafxCudaDriver *const drv = driver_loaded();
    return drv && drv->init == CUDA_SUCCESS ? drv : NULL;
}

/* The driver; VMAFX_E_NOTSUP naming the backend when it cannot be loaded,
 * VMAFX_E_DEVICE when it does not initialise (no device is not a failure:
 * `*drv` is then the driver and the caller sees 0 devices). */
static VmafxStatus driver_or_fail(const VmafxReport *report, const VmafxCudaDriver **drv)
{
    *drv = driver_loaded();
    if (!*drv) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_BACKEND, "cuda",
                          "backend cuda: the CUDA driver library could not be loaded "
                          "(libcuda.so.1 / nvcuda.dll); see docs/backends/cuda/overview.md");
    }
    if ((*drv)->init != CUDA_SUCCESS && (int)(*drv)->init != VMAFX_CU_ERROR_NO_DEVICE) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, (int32_t)(*drv)->init, VMAFX_SUBJECT_BACKEND,
                          "cuda", "backend cuda: cuInit(0) failed (%d)", (int)(*drv)->init);
    }
    return VMAFX_OK;
}

/* A failed driver call, named. */
static VmafxStatus cuda_failed(const VmafxReport *report, CUresult res, const char *subject,
                               const char *call)
{
    const char *name = "?";
    if (driver.f) {
        (void)driver.f->cuGetErrorName(res, &name);
    }
    return VMAFX_FAIL(report,
                      (int)res == VMAFX_CU_ERROR_OUT_OF_MEMORY ? VMAFX_E_NOMEM : VMAFX_E_DEVICE,
                      (int32_t)res, VMAFX_SUBJECT_DEVICE, subject,
                      "backend cuda: %s failed: %s (%d)", call, name, (int)res);
}

int vmafx_cuda_push(const VmafxCudaDevice *dev)
{
    const CUresult res = dev->state.f->cuCtxPushCurrent(dev->state.ctx);
    return res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res);
}

int vmafx_cuda_pop(const VmafxCudaDevice *dev, int err)
{
    CUcontext popped = NULL;
    const CUresult res = dev->state.f->cuCtxPopCurrent(&popped);
    return err ? err : (res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res));
}

/* ---- Enumeration -------------------------------------------------------------- */

static uint32_t visible_devices(const VmafxCudaDriver *drv)
{
    int n = 0;
    if (drv->init != CUDA_SUCCESS || drv->f->cuDeviceGetCount(&n) != CUDA_SUCCESS || n < 0) {
        return 0u;
    }
    return n > VMAFX_CUDA_MAX_DEVICES ? VMAFX_CUDA_MAX_DEVICES : (uint32_t)n;
}

VmafxStatus vmafx_cuda_device_count(const VmafxReport *report, uint32_t *count)
{
    const VmafxCudaDriver *drv = NULL;
    const VmafxStatus status = driver_or_fail(report, &drv);
    *count = status == VMAFX_OK ? visible_devices(drv) : 0u;
    return status;
}

/* Names of the devices, kept for the process (VmafxDeviceInfo.name). */
static char device_names[VMAFX_CUDA_MAX_DEVICES][256];
static pthread_mutex_t names_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *device_name(const VmafxCudaDriver *drv, CUdevice dev)
{
    if (dev < 0 || dev >= VMAFX_CUDA_MAX_DEVICES) {
        return "cuda";
    }
    (void)pthread_mutex_lock(&names_lock);
    char *const name = device_names[dev];
    if (name[0] == '\0' &&
        drv->f->cuDeviceGetName(name, (int)sizeof(device_names[0]) - 1, dev) != CUDA_SUCCESS) {
        (void)snprintf(name, sizeof(device_names[0]), "cuda device %d", (int)dev);
    }
    (void)pthread_mutex_unlock(&names_lock);
    return name;
}

static uint64_t device_memory(const VmafxCudaDriver *drv, CUdevice dev)
{
    size_t bytes = 0;
    if (!drv->total_memory || drv->total_memory(&bytes, dev) != CUDA_SUCCESS) {
        return 0u;
    }
    return (uint64_t)bytes;
}

/* What a CUDA device imports: device pointers, CUDA arrays (copied only
 * when allowed, converted when semi-planar) and GL textures; it waits on
 * NONE, HOST, CUDA_EVENT and GL_SYNC acquire fences and returns HOST and
 * CUDA_EVENT release fences. */
static VmafxDeviceInfo cuda_info(const VmafxCudaDriver *drv, CUdevice dev, int32_t index,
                                 uint32_t flags)
{
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    info.backend = VMAFX_BACKEND_CUDA;
    info.index = index;
    info.flags = flags;
    info.memory_kinds = (1u << VMAFX_MEMORY_DEVICE_POINTER) | (1u << VMAFX_MEMORY_DEVICE_ARRAY) |
                        (1u << VMAFX_MEMORY_GL_TEXTURE);
    info.fence_kinds = (1u << VMAFX_FENCE_NONE) | (1u << VMAFX_FENCE_HOST) |
                       (1u << VMAFX_FENCE_CUDA_EVENT) | (1u << VMAFX_FENCE_GL_SYNC);
    info.total_memory = device_memory(drv, dev);
    info.name = device_name(drv, dev);
    return info;
}

/* CUDA device `index` of `drv` (0 for -1), or VMAFX_E_NOTFOUND naming it. */
static VmafxStatus device_at(const VmafxReport *report, const VmafxCudaDriver *drv, int32_t index,
                             const char *subject, CUdevice *dev)
{
    const uint32_t n = visible_devices(drv);
    const int32_t at = index == -1 ? 0 : index;
    if (at < 0 || (uint32_t)at >= n) {
        return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_DEVICE, subject,
                          "backend cuda has %u device%s, not %d", (unsigned)n, n == 1u ? "" : "s",
                          (int)index);
    }
    const CUresult res = drv->f->cuDeviceGet(dev, at);
    return res == CUDA_SUCCESS ? VMAFX_OK : cuda_failed(report, res, subject, "cuDeviceGet");
}

VmafxStatus vmafx_cuda_device_info(const VmafxReport *report, int32_t index, VmafxDeviceInfo *info)
{
    const VmafxCudaDriver *drv = NULL;
    VmafxStatus status = driver_or_fail(report, &drv);
    CUdevice dev = 0;
    if (status == VMAFX_OK) {
        status = device_at(report, drv, index, "index", &dev);
    }
    if (status == VMAFX_OK) {
        *info = cuda_info(drv, dev, index == -1 ? 0 : index, 0u);
    }
    return status;
}

/* ---- Opening a device ------------------------------------------------------------ */

/* Compute capability at or above the fatbin's floor (ADR-1223). */
static VmafxStatus check_arch(const VmafxReport *report, const VmafxCudaDriver *drv, CUdevice dev)
{
    int major = 0;
    int minor = 0;
    CUresult res =
        drv->f->cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
    if (res == CUDA_SUCCESS) {
        res =
            drv->f->cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
    }
    if (res != CUDA_SUCCESS) {
        return cuda_failed(report, res, "device", "cuDeviceGetAttribute");
    }
    if (!vmaf_cuda_arch_supported(major, minor)) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "backend cuda: %s has compute capability %d.%d, below the minimum "
                          "%d.%d (ADR-1223)",
                          device_name(drv, dev), major, minor, VMAF_CUDA_MIN_COMPUTE_MAJOR,
                          VMAF_CUDA_MIN_COMPUTE_MINOR);
    }
    return VMAFX_OK;
}

/* The device's context: the caller's (external[0]) or the retained primary
 * context of the indexed device. */
static VmafxStatus open_context(const VmafxReport *report, const VmafxCudaDriver *drv,
                                const VmafxDeviceDesc *desc, VmafxCudaDevice *dev)
{
    CUresult res = CUDA_SUCCESS;
    if (desc->external[0] != 0u) {
        /* NOLINTNEXTLINE(performance-no-int-to-ptr): the caller's CUcontext crosses the ABI as uintptr_t (VmafxDeviceDesc.external, ADR-1929). */
        dev->state.ctx = (CUcontext)desc->external[0];
        dev->index = -1;
        res = drv->f->cuCtxPushCurrent(dev->state.ctx);
        if (res == CUDA_SUCCESS) {
            res = drv->f->cuCtxGetDevice(&dev->state.dev);
            CUcontext popped = NULL;
            (void)drv->f->cuCtxPopCurrent(&popped);
        }
        return res == CUDA_SUCCESS ? check_arch(report, drv, dev->state.dev) :
                                     cuda_failed(report, res, "desc.external[0]", "cuCtxGetDevice");
    }
    VmafxStatus status = device_at(report, drv, desc->index, "desc.index", &dev->state.dev);
    if (status == VMAFX_OK) {
        status = check_arch(report, drv, dev->state.dev);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    dev->index = desc->index == -1 ? 0 : desc->index;
    res = drv->f->cuDevicePrimaryCtxRetain(&dev->state.ctx, dev->state.dev);
    dev->own_context = res == CUDA_SUCCESS;
    return res == CUDA_SUCCESS ? VMAFX_OK :
                                 cuda_failed(report, res, "desc.index", "cuDevicePrimaryCtxRetain");
}

/* The library stream (the caller's, external[1]) and the conversion kernels;
 * the context is pushed. */
static VmafxStatus open_stream_and_kernels(const VmafxReport *report, const VmafxDeviceDesc *desc,
                                           VmafxCudaDevice *dev)
{
    CudaFunctions *const f = dev->state.f;
    CUresult res = CUDA_SUCCESS;
    if (desc->external[0] != 0u && desc->external[1] != 0u) {
        /* NOLINTNEXTLINE(performance-no-int-to-ptr): the caller's CUstream crosses the ABI as uintptr_t (VmafxDeviceDesc.external, ADR-1929). */
        dev->state.str = (CUstream)desc->external[1];
    } else {
        res = f->cuStreamCreateWithPriority(&dev->state.str, CU_STREAM_NON_BLOCKING, 0);
        dev->own_stream = res == CUDA_SUCCESS;
        if (res != CUDA_SUCCESS) {
            return cuda_failed(report, res, "device", "cuStreamCreateWithPriority");
        }
    }
    VmafxCudaKernels *const k = &dev->kernels;
    res = f->cuModuleLoadData(&k->module, import_convert_ptx);
    if (res == CUDA_SUCCESS) {
        res = f->cuModuleGetFunction(&k->deint_8, k->module, "vmafx_import_deint_8");
    }
    if (res == CUDA_SUCCESS) {
        res = f->cuModuleGetFunction(&k->deint_16, k->module, "vmafx_import_deint_16");
    }
    if (res == CUDA_SUCCESS) {
        res = f->cuModuleGetFunction(&k->shift_16, k->module, "vmafx_import_shift_16");
    }
    return res == CUDA_SUCCESS ? VMAFX_OK : cuda_failed(report, res, "device", "cuModuleLoadData");
}

/* Free what open_context() / open_stream_and_kernels() made (any subset). */
static void free_device(VmafxCudaDevice *dev)
{
    CudaFunctions *const f = dev->state.f;
    if (dev->state.ctx && f->cuCtxPushCurrent(dev->state.ctx) == CUDA_SUCCESS) {
        /* Every frame's work and release completion runs on the library
         * stream; the caller's other streams in its context are not waited
         * for. */
        if (dev->state.str) {
            (void)f->cuStreamSynchronize(dev->state.str);
        }
        if (dev->kernels.module) {
            (void)f->cuModuleUnload(dev->kernels.module);
        }
        if (dev->own_stream) {
            (void)f->cuStreamDestroy(dev->state.str);
        }
        CUcontext popped = NULL;
        (void)f->cuCtxPopCurrent(&popped);
    }
    if (dev->own_context) {
        (void)f->cuDevicePrimaryCtxRelease(dev->state.dev);
    }
    free(dev);
}

/* The flags a CUDA device takes: none (no kernel profiler in this build). */
static VmafxStatus check_cuda_desc(const VmafxReport *report, const VmafxDeviceDesc *desc)
{
    if (desc->flags & ~(uint32_t)VMAFX_DEVICE_PROFILING) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.flags",
                          "unknown flag bits 0x%x",
                          (unsigned)(desc->flags & ~(uint32_t)VMAFX_DEVICE_PROFILING));
    }
    if (desc->flags & VMAFX_DEVICE_PROFILING) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.flags",
                          "VMAFX_DEVICE_PROFILING: CUDA devices have no kernel profiler in this "
                          "build; use the vendor profiler (docs/api/vmafx/index.md)");
    }
    if (desc->external[0] == 0u && desc->external[1] != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.external[1]",
                          "a stream without its context (external[0])");
    }
    return VMAFX_OK;
}

static VmafxCudaDevice *new_device(const VmafxCudaDriver *drv)
{
    VmafxCudaDevice *const dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return NULL;
    }
    dev->state.f = drv->f;
    atomic_init(&dev->copy_logged, 0u);
    return dev;
}

VmafxStatus vmafx_cuda_device_open(const VmafxReport *report, const VmafxDeviceDesc *desc,
                                   VmafxDevice *device)
{
    const VmafxCudaDriver *drv = NULL;
    VmafxStatus status = driver_or_fail(report, &drv);
    if (status == VMAFX_OK) {
        status = check_cuda_desc(report, desc);
    }
    VmafxCudaDevice *const dev = status == VMAFX_OK ? new_device(drv) : NULL;
    if (status != VMAFX_OK || !dev) {
        return status != VMAFX_OK ? status :
                                    VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_DEVICE,
                                               "device", "cannot allocate a CUDA device");
    }
    status = open_context(report, drv, desc, dev);
    if (status == VMAFX_OK && vmafx_cuda_push(dev) == 0) {
        status = open_stream_and_kernels(report, desc, dev);
        (void)vmafx_cuda_pop(dev, 0);
    } else if (status == VMAFX_OK) {
        status = VMAFX_FAIL(report, VMAFX_E_DEVICE, 0, VMAFX_SUBJECT_DEVICE, "device",
                            "backend cuda: cannot make the device's context current");
    }
    if (status != VMAFX_OK) {
        free_device(dev);
        return status;
    }
    assert(dev->state.ctx != NULL && dev->state.str != NULL);
    dev->total_memory = device_memory(drv, dev->state.dev);
    (void)snprintf(dev->name, sizeof(dev->name), "%s", device_name(drv, dev->state.dev));
    device->lane = dev;
    device->index = dev->index;
    return VMAFX_OK;
}

void vmafx_cuda_device_close(VmafxDevice *device)
{
    if (device && device->lane) {
        free_device(vmafx_cuda_dev(device));
        device->lane = NULL;
    }
}

void vmafx_cuda_device_describe(const VmafxDevice *device, VmafxDeviceInfo *info)
{
    const VmafxCudaDevice *const dev = vmafx_cuda_dev(device);
    *info = cuda_info(&driver, dev->state.dev, dev->index, device->flags);
}

/* ---- Contexts ------------------------------------------------------------------- */

VmafxStatus vmafx_cuda_context_attach(const VmafxReport *report, VmafxContext *context,
                                      VmafxDevice *device)
{
    const VmafxCudaDevice *const dev = vmafx_cuda_dev(device);
    VmafCudaConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cu_ctx = dev->state.ctx;
    VmafCudaState *state = NULL;
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    int err = vmaf_cuda_state_init(&state, cfg);
    if (!err) {
        err = vmaf_cuda_import_state(vmafx_context_engine(context), state);
        if (err) {
            (void)vmaf_cuda_state_free(state);
        }
    }
    vmafx_engine_leave(previous);
    if (err) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, err, VMAFX_SUBJECT_DEVICE, "device",
                          "backend cuda device %d: the engine cannot score on it (%d)",
                          (int)dev->index, err);
    }
    context->lane_state = state;
    return VMAFX_OK;
}

void vmafx_cuda_context_detach(VmafxContext *context)
{
    if (context->lane_state) {
        (void)vmaf_cuda_state_free(context->lane_state);
        context->lane_state = NULL;
    }
}

/* NOLINTEND(modernize-use-nullptr) */
