/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Vulkan frames on a CUDA device (RC4 WP3 Vulkan lane, ADR-1852 design
 * section 2.7). Import only (Q-011): the producer's Vulkan device exported
 * its memory and timeline semaphores, this file imports them with CUDA's
 * external-memory and external-semaphore interop.
 *
 * - Memory: each plane's VkDeviceMemory, exported as an opaque descriptor,
 *   is imported once per descriptor (cuImportExternalMemory, the allocation's
 *   size). OPTIMAL images become CUDA arrays at the plane's offset
 *   (cuExternalMemoryGetMappedMipmappedArray), which the array path of
 *   import_frame.c reads out and converts; LINEAR images and buffers become
 *   device pointers (cuExternalMemoryGetMappedBuffer), which the pointer path
 *   binds where they are. The producer's GPU must be the device's (PCI).
 * - Acquire: each VULKAN_SEMAPHORE acquire fence is imported as a timeline
 *   semaphore and waited on, on the device, by the library stream
 *   (cuWaitExternalSemaphoresAsync) ahead of the import's reads.
 * - Release: a VULKAN_SEMAPHORE given to vmafx_frame_signal_on_release() is
 *   signalled on the library stream where the frame's last reference is
 *   dropped, behind every reader (ADR-2023 item 1: every reader of a frame of
 *   the device is on that stream), before the release callback runs.
 * - The imported objects may not be destroyed while the device still uses
 *   them, and the release must not wait on the host: they are retired behind
 *   an event recorded after the release's signals and destroyed once it
 *   completed, at the device's next Vulkan import and at its close.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "log.h"
#include "picture.h"
#include "vmafx/error_internal.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_cuda.h"
#include "vmafx_cuda_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* CUDA_EXTERNAL_MEMORY_DEDICATED of cuda.h, which the nv-codec-headers table
 * does not declare (CUDA driver API reference, cuImportExternalMemory()). */
#define VMAFX_CU_EXTERNAL_MEMORY_DEDICATED 0x1u

/* Retired imports kept before a Vulkan import drains them (HISS-02 bound on
 * the walk; a decoder pool holds a few dozen frames). */
#define VMAFX_CUDA_VK_RETIRED_MAX 4096u

/* ---- The device's PCI location ------------------------------------------------------- */

void vmafx_cuda_device_pci(const VmafxCudaDriver *drv, CUdevice dev, uint32_t pci[4])
{
    char bus[32];
    const bool ok = drv->f->cuDeviceGetPCIBusId &&
                    drv->f->cuDeviceGetPCIBusId(bus, (int)sizeof(bus), dev) == CUDA_SUCCESS;
    vmafx_parse_pci_bus_id(ok ? bus : NULL, pci);
}

/* ---- Checks ----------------------------------------------------------------------------- */

/* The interop entry points this file calls (the driver table loads them as
 * optional symbols). */
static bool interop_loaded(const CudaFunctions *f)
{
    return f->cuImportExternalMemory && f->cuDestroyExternalMemory &&
           f->cuExternalMemoryGetMappedBuffer && f->cuExternalMemoryGetMappedMipmappedArray &&
           f->cuMipmappedArrayGetLevel && f->cuMipmappedArrayDestroy &&
           f->cuImportExternalSemaphore && f->cuDestroyExternalSemaphore &&
           f->cuWaitExternalSemaphoresAsync && f->cuSignalExternalSemaphoresAsync;
}

VmafxStatus vmafx_cuda_vulkan_check(const VmafxReport *report, const VmafxCudaDevice *dev,
                                    const VmafxFrameImport *d)
{
    if (!interop_loaded(dev->state.f)) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend cuda: the CUDA driver lacks the external memory and semaphore "
                          "interop VULKAN memory needs");
    }
    if (d->vulkan_handle_type != VMAFX_VULKAN_HANDLE_OPAQUE_FD) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER,
                          "desc.vulkan_handle_type",
                          "backend cuda: handle type %s; CUDA imports Vulkan memory exported as "
                          "OPAQUE_FD (the driver imports dma-bufs on Tegra only)",
                          vmafx_vulkan_handle_name(d->vulkan_handle_type));
    }
    if (d->vulkan_tiling == VMAFX_VULKAN_TILING_DRM_FORMAT_MODIFIER) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.vulkan_tiling",
                          "backend cuda: DRM_FORMAT_MODIFIER tiling is a dma-buf layout; export "
                          "OPTIMAL or LINEAR images (or buffers) as OPAQUE_FD for CUDA");
    }
    return vmafx_import_check_vulkan_device(report, d, dev->pci, "cuda");
}

/* ---- Objects of one frame ---------------------------------------------------------------- */

/* A frame's Vulkan objects, created on demand (frame state locked or not yet
 * shared). */
static VmafxCudaVulkan *vulkan_of(VmafxCudaFrame *cf)
{
    if (!cf->vk) {
        cf->vk = calloc(1, sizeof(*cf->vk));
    }
    return cf->vk;
}

static VmafxStatus cuda_error(const VmafxReport *report, CUresult res, const char *subject,
                              const char *call)
{
    return VMAFX_FAIL(report,
                      (int)res == VMAFX_CU_ERROR_OUT_OF_MEMORY ? VMAFX_E_NOMEM : VMAFX_E_DEVICE,
                      (int32_t)res, VMAFX_SUBJECT_PARAMETER, subject,
                      "backend cuda: %s failed (CUDA error %d)", call, (int)res);
}

/* Import a VULKAN_SEMAPHORE fence as a timeline semaphore (context pushed). */
static VmafxStatus import_semaphore(const VmafxReport *report, const VmafxCudaDevice *dev,
                                    const VmafxFence *fence, const char *subject,
                                    CUexternalSemaphore *out)
{
    if (fence->fd < 0) {
        return VMAFX_FAIL(report, fence->handle ? VMAFX_E_NOTSUP : VMAFX_E_INVALID, 0,
                          VMAFX_SUBJECT_FENCE, subject,
                          fence->handle ? "backend cuda: a Windows handle of a Vulkan semaphore; "
                                          "Windows handles are not imported until a Windows "
                                          "device runs their tests" :
                                          "a Vulkan semaphore without its descriptor");
    }
    CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_FD;
    desc.handle.fd = dup(fence->fd); /* CUDA owns the descriptor it imports */
    if (desc.handle.fd < 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, errno, VMAFX_SUBJECT_FENCE, subject,
                          "cannot duplicate descriptor %d", (int)fence->fd);
    }
    const CUresult res = dev->state.f->cuImportExternalSemaphore(out, &desc);
    if (res != CUDA_SUCCESS) {
        (void)close(desc.handle.fd);
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, (int32_t)res, VMAFX_SUBJECT_FENCE, subject,
                          "backend cuda: cannot import the Vulkan timeline semaphore (CUDA error "
                          "%d); is it an exported VK_SEMAPHORE_TYPE_TIMELINE of the device's GPU?",
                          (int)res);
    }
    return VMAFX_OK;
}

/* The memory import of plane `i`: a plane before it with the same
 * descriptor shares it. */
static VmafxStatus import_memory(const VmafxReport *report, const VmafxCudaDevice *dev,
                                 const VmafxFrameImport *d, uint32_t i, VmafxCudaVulkan *vk)
{
    for (uint32_t j = 0; j < i; j++) {
        if (d->plane[j].fd == d->plane[i].fd && d->plane[j].size == d->plane[i].size) {
            vk->mem_of[i] = vk->mem_of[j];
            return VMAFX_OK;
        }
    }
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    desc.size = d->plane[i].size;
    desc.flags =
        (d->vulkan_flags & VMAFX_VULKAN_DEDICATED) ? VMAFX_CU_EXTERNAL_MEMORY_DEDICATED : 0u;
    desc.handle.fd = dup(d->plane[i].fd);
    if (desc.handle.fd < 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, errno, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "fd"), "cannot duplicate descriptor %d",
                          (int)d->plane[i].fd);
    }
    assert(vk->n_mem < 3u);
    const CUresult res = dev->state.f->cuImportExternalMemory(&vk->mem[vk->n_mem], &desc);
    if (res != CUDA_SUCCESS) {
        (void)close(desc.handle.fd);
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, (int32_t)res, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "fd"),
                          "backend cuda: cannot import plane %u's Vulkan memory (%llu bytes, CUDA "
                          "error %d); is it OPAQUE_FD memory of the device's GPU?",
                          (unsigned)i, (unsigned long long)d->plane[i].size, (int)res);
    }
    vk->mem_of[i] = vk->n_mem++;
    return VMAFX_OK;
}

/* Plane `i` of an OPTIMAL frame as a CUDA array of the plane's geometry. */
static VmafxStatus map_array(const VmafxReport *report, const VmafxCudaDevice *dev,
                             const VmafxFrameImport *d, const VmafxImportLayout *layout, uint32_t i,
                             VmafxCudaVulkan *vk, CUarray *array)
{
    unsigned pw[3];
    unsigned ph[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, pw, ph);
    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.offset = d->plane[i].offset;
    desc.arrayDesc.Width = pw[i];
    desc.arrayDesc.Height = ph[i];
    desc.arrayDesc.Format = d->bpc > 8u ? CU_AD_FORMAT_UNSIGNED_INT16 : CU_AD_FORMAT_UNSIGNED_INT8;
    desc.arrayDesc.NumChannels = layout->interleaved && i == 1u ? 2u : 1u;
    desc.numLevels = 1;
    CudaFunctions *const f = dev->state.f;
    CUresult res =
        f->cuExternalMemoryGetMappedMipmappedArray(&vk->mipmap[i], vk->mem[vk->mem_of[i]], &desc);
    if (res == CUDA_SUCCESS) {
        res = f->cuMipmappedArrayGetLevel(array, vk->mipmap[i], 0);
    }
    return res == CUDA_SUCCESS ? VMAFX_OK :
                                 cuda_error(report, res, vmafx_import_plane_field(i, "offset"),
                                            "cuExternalMemoryGetMappedMipmappedArray");
}

/* Plane `i` of a LINEAR frame: the whole memory mapped once, the plane a
 * pointer into it. */
static VmafxStatus map_pointer(const VmafxReport *report, const VmafxCudaDevice *dev,
                               const VmafxFrameImport *d, uint32_t i, VmafxCudaVulkan *vk,
                               VmafxImportPlane *plane)
{
    const uint32_t m = vk->mem_of[i];
    assert(m < 3u);
    if (!vk->mapped[m]) {
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.size = d->plane[i].size;
        const CUresult res =
            dev->state.f->cuExternalMemoryGetMappedBuffer(&vk->mapped[m], vk->mem[m], &desc);
        if (res != CUDA_SUCCESS) {
            vk->mapped[m] = 0;
            return cuda_error(report, res, vmafx_import_plane_field(i, "size"),
                              "cuExternalMemoryGetMappedBuffer");
        }
    }
    memset(plane, 0, sizeof(*plane));
    plane->handle = (uintptr_t)vk->mapped[m];
    plane->offset = d->plane[i].offset;
    plane->pitch = d->plane[i].pitch;
    plane->size = d->plane[i].size;
    return VMAFX_OK;
}

VmafxStatus vmafx_cuda_vulkan_map(const VmafxReport *report, VmafxCudaFrame *cf,
                                  const VmafxFrameImport *d, const VmafxImportLayout *layout,
                                  CUarray arrays[3], VmafxFrameImport *pointers)
{
    vmafx_cuda_vulkan_drain(cf->dev, false);
    VmafxCudaVulkan *const vk = vulkan_of(cf);
    if (!vk) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "frame",
                          "cannot allocate the frame's Vulkan imports");
    }
    const bool linear = d->vulkan_tiling == VMAFX_VULKAN_TILING_LINEAR;
    if (linear) {
        *pointers = *d;
        pointers->memory = VMAFX_MEMORY_DEVICE_POINTER;
    }
    VmafxStatus status = VMAFX_OK;
    for (uint32_t i = 0; i < layout->n_planes && status == VMAFX_OK; i++) {
        status = import_memory(report, cf->dev, d, i, vk);
        if (status == VMAFX_OK) {
            status = linear ? map_pointer(report, cf->dev, d, i, vk, &pointers->plane[i]) :
                              map_array(report, cf->dev, d, layout, i, vk, &arrays[i]);
        }
    }
    return status;
}

/* ---- Acquire ------------------------------------------------------------------------------ */

/* Wait for one VULKAN_SEMAPHORE acquire fence on the library stream. */
static VmafxStatus wait_semaphore(const VmafxReport *report, VmafxCudaFrame *cf,
                                  const VmafxFence *fence, const char *subject)
{
    VmafxCudaVulkan *const vk = vulkan_of(cf);
    if (!vk) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "frame",
                          "cannot allocate the frame's Vulkan imports");
    }
    assert(vk->n_wait < 3u);
    VmafxStatus status = import_semaphore(report, cf->dev, fence, subject, &vk->wait[vk->n_wait]);
    if (status != VMAFX_OK) {
        return status;
    }
    CUexternalSemaphore sem = vk->wait[vk->n_wait++];
    if (vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT)) {
        return VMAFX_OK; /* the planted defect */
    }
    CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS params;
    memset(&params, 0, sizeof(params));
    params.params.fence.value = fence->value;
    const CUresult res =
        cf->dev->state.f->cuWaitExternalSemaphoresAsync(&sem, &params, 1, cf->dev->state.str);
    return res == CUDA_SUCCESS ? VMAFX_OK :
                                 cuda_error(report, res, subject, "cuWaitExternalSemaphoresAsync");
}

/* Wait for one CUDA_EVENT acquire fence on the library stream. */
static VmafxStatus wait_event(const VmafxReport *report, const VmafxCudaDevice *dev,
                              const VmafxFence *fence, const char *subject)
{
    if (vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT)) {
        return VMAFX_OK; /* the planted defect */
    }
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): the producer's CUevent crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
    CUevent event = (CUevent)fence->handle;
    const CUresult res = dev->state.f->cuStreamWaitEvent(dev->state.str, event, 0);
    return res == CUDA_SUCCESS ? VMAFX_OK : cuda_error(report, res, subject, "cuStreamWaitEvent");
}

VmafxStatus vmafx_cuda_wait_acquires(const VmafxReport *report, VmafxCudaFrame *cf,
                                     const VmafxFrameImport *d)
{
    const VmafxFence *const fences[3] = {&d->acquire, &d->acquire_more[0], &d->acquire_more[1]};
    VmafxStatus status = VMAFX_OK;
    for (uint32_t i = 0; i < 3u && status == VMAFX_OK; i++) {
        const VmafxFence *const f = fences[i];
        if (f->kind == VMAFX_FENCE_VULKAN_SEMAPHORE) {
            status = wait_semaphore(report, cf, f, vmafx_import_acquire_name(i));
        } else if (f->kind == VMAFX_FENCE_CUDA_EVENT) {
            status = wait_event(report, cf->dev, f, vmafx_import_acquire_name(i));
        }
    }
    return status;
}

/* ---- Release ------------------------------------------------------------------------------ */

/* Add `signal` to the frame's release signals (frame locked). */
static VmafxStatus add_signal(const VmafxReport *report, VmafxCudaFrame *cf, VmafxCudaVulkan *vk,
                              const VmafxFence *signal)
{
    const uint32_t n = vk->n_signal;
    if (n >= 3u) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_FENCE, "signal",
                          "a frame signals up to 3 producer fences at release");
    }
    if (vmafx_cuda_push(cf->dev) != 0) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, 0, VMAFX_SUBJECT_DEVICE, "frame",
                          "backend cuda: cannot make the device current");
    }
    const VmafxStatus status = import_semaphore(report, cf->dev, signal, "signal", &vk->signal[n]);
    (void)vmafx_cuda_pop(cf->dev, 0);
    if (status == VMAFX_OK) {
        vk->signal_value[n] = signal->value;
        vk->n_signal = n + 1u;
    }
    return status;
}

VmafxStatus vmafx_cuda_signal_on_release(const VmafxReport *report, VmafxFrame *frame,
                                         const VmafxFence *signal)
{
    VmafxCudaFrame *const cf = frame->lane;
    assert(cf != NULL && signal->kind == VMAFX_FENCE_VULKAN_SEMAPHORE);
    (void)pthread_mutex_lock(&cf->lock);
    VmafxCudaVulkan *const vk = vulkan_of(cf);
    const VmafxStatus status =
        vk ? add_signal(report, cf, vk, signal) :
             VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FENCE, "signal",
                        "cannot allocate the frame's Vulkan import state");
    (void)pthread_mutex_unlock(&cf->lock);
    return status;
}

/* Signal the frame's producer semaphores on `stream` (context pushed). */
static int signal_semaphores(const VmafxCudaDevice *dev, VmafxCudaVulkan *vk, CUstream stream)
{
    if (vk->signalled || vk->n_signal == 0u) {
        return 0;
    }
    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS params[3];
    memset(params, 0, sizeof(params));
    for (uint32_t i = 0; i < vk->n_signal; i++) {
        params[i].params.fence.value = vk->signal_value[i];
    }
    vk->signalled = true;
    const CUresult res =
        dev->state.f->cuSignalExternalSemaphoresAsync(vk->signal, params, vk->n_signal, stream);
    return res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res);
}

void vmafx_cuda_vulkan_release_early(VmafxCudaFrame *cf)
{
    /* The planted defect: the producer semaphores signalled now, on a
     * stream of their own that waits for nothing (not the legacy stream,
     * which waits for the engine's blocking streams), not behind the readers
     * on the library stream. */
    CudaFunctions *const f = cf->dev->state.f;
    CUstream early = NULL;
    if (!cf->vk || f->cuStreamCreate(&early, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS) {
        return;
    }
    (void)signal_semaphores(cf->dev, cf->vk, early);
    (void)f->cuStreamDestroy(early); /* its work still runs */
}

/* Destroy one frame's imports (context pushed, its work completed). */
static void destroy_imports(const VmafxCudaDevice *dev, VmafxCudaVulkan *vk)
{
    CudaFunctions *const f = dev->state.f;
    for (uint32_t i = 0; i < 3u; i++) {
        if (vk->mipmap[i]) {
            (void)f->cuMipmappedArrayDestroy(vk->mipmap[i]);
        }
        if (vk->mapped[i]) {
            (void)f->cuMemFree(vk->mapped[i]);
        }
    }
    for (uint32_t i = 0; i < vk->n_mem; i++) {
        (void)f->cuDestroyExternalMemory(vk->mem[i]);
    }
    for (uint32_t i = 0; i < vk->n_wait; i++) {
        (void)f->cuDestroyExternalSemaphore(vk->wait[i]);
    }
    for (uint32_t i = 0; i < vk->n_signal; i++) {
        (void)f->cuDestroyExternalSemaphore(vk->signal[i]);
    }
    if (vk->done) {
        (void)f->cuEventDestroy(vk->done);
    }
    free(vk);
}

int vmafx_cuda_vulkan_release(VmafxCudaFrame *cf)
{
    VmafxCudaVulkan *const vk = cf->vk;
    if (!vk) {
        return 0;
    }
    cf->vk = NULL;
    VmafxCudaDevice *const dev = cf->dev;
    CudaFunctions *const f = dev->state.f;
    int err = signal_semaphores(dev, vk, dev->state.str);
    CUresult res = f->cuEventCreate(&vk->done, CU_EVENT_DISABLE_TIMING);
    res = res == CUDA_SUCCESS ? f->cuEventRecord(vk->done, dev->state.str) : res;
    if (res != CUDA_SUCCESS) {
        /* No event to retire behind: wait for the stream here. */
        (void)f->cuStreamSynchronize(dev->state.str);
        destroy_imports(dev, vk);
        return err ? err : vmaf_cuda_result_to_errno((int)res);
    }
    (void)pthread_mutex_lock(&dev->vk_lock);
    vk->next = dev->vk_retired;
    dev->vk_retired = vk;
    (void)pthread_mutex_unlock(&dev->vk_lock);
    return err;
}

void vmafx_cuda_vulkan_drain(VmafxCudaDevice *dev, bool all)
{
    (void)pthread_mutex_lock(&dev->vk_lock);
    VmafxCudaVulkan **link = &dev->vk_retired;
    for (uint32_t n = 0; *link && n < VMAFX_CUDA_VK_RETIRED_MAX; n++) {
        VmafxCudaVulkan *const vk = *link;
        const CUresult done =
            all ? dev->state.f->cuEventSynchronize(vk->done) : dev->state.f->cuEventQuery(vk->done);
        if (done == CUDA_ERROR_NOT_READY) {
            link = &vk->next;
            continue;
        }
        *link = vk->next;
        destroy_imports(dev, vk);
    }
    (void)pthread_mutex_unlock(&dev->vk_lock);
}

/* NOLINTEND(modernize-use-nullptr) */
