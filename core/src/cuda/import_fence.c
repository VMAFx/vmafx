/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Fences of the CUDA lane (RC4 WP3, ADR-1852 design section 2.7, ADR-2023).
 *
 * A frame's release is enqueued on the library stream where its last
 * reference is dropped (vmafx_cuda_frame_release()), behind every reader
 * (every reader of a frame of the device runs on that stream):
 *
 * - HOST: a host function there signals the library's host fence; the
 *   callback makes no CUDA call.
 * - CUDA_EVENT: the frame's release event is recorded there. A CUDA event
 *   cannot be recorded ahead of time, and a stream wait on an event that was
 *   never recorded returns at once; so the library keeps every release event
 *   it handed out in a table until it is recorded (release_events.c, shared
 *   with the HIP lane), and vmafx_fence_wait() answers "pending" for such an
 *   event instead of asking the driver. A
 *   producer waits on the device only once the event is recorded: from the
 *   frame's release callback (VmafxFrameImport.release), which runs after
 *   the recording, or after a host wait returned.
 *
 * (A release recorded at request time behind a stream that waits on a gate
 * word was measured and dropped: a stream blocked on a gate holds up every
 * cuCtxSynchronize() of the context, the engine's flush and the ADR-1199
 * barrier among them, and deadlocks a second context that still reads the
 * frame.)
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "common.h"
#include "cuda_helper.cuh"
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

/* A CUDA release event the table drops last. */
static void destroy_event(uintptr_t event)
{
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): the table holds the CUevent as uintptr_t, as VmafxFence.handle carries it (ADR-1929). */
    (void)vmafx_cuda_driver()->f->cuEventDestroy((CUevent)event);
}

/* The CUDA lane's release events (release_events.c, ADR-2023 item 3). */
static VmafxReleaseEvents release_events = {
    .lock = PTHREAD_MUTEX_INITIALIZER, .high = 0u, .destroy = destroy_event};

/* ---- Frame state -------------------------------------------------------------- */

VmafxCudaFrame *vmafx_cuda_frame_state_new(VmafxCudaDevice *dev)
{
    VmafxCudaFrame *const cf = calloc(1, sizeof(*cf));
    if (!cf) {
        return NULL;
    }
    if (pthread_mutex_init(&cf->lock, NULL) != 0) {
        free(cf);
        return NULL;
    }
    cf->dev = dev;
    return cf;
}

void vmafx_cuda_frame_state_free(VmafxCudaFrame *cf)
{
    if (cf) {
        (void)pthread_mutex_destroy(&cf->lock);
        free(cf);
    }
}

/* ---- Release events ------------------------------------------------------------- */

/* The frame's release event with one more reference for the caller, created
 * on first use (context pushed, frame locked). */
static VmafxStatus release_event_take(const VmafxReport *report, VmafxCudaFrame *cf, CUevent *event)
{
    CUevent fresh = NULL;
    if (!cf->release_slot) {
        const CUresult res = cf->dev->state.f->cuEventCreate(&fresh, CU_EVENT_DISABLE_TIMING);
        if (res != CUDA_SUCCESS) {
            return VMAFX_FAIL(report, VMAFX_E_DEVICE, (int32_t)res, VMAFX_SUBJECT_FENCE, "out",
                              "backend cuda: cannot create a release event (CUDA error %d)",
                              (int)res);
        }
    }
    uintptr_t taken = 0u;
    if (vmafx_release_events_take(&release_events, &cf->release_slot, (uintptr_t)fresh, &taken) !=
        0) {
        (void)cf->dev->state.f->cuEventDestroy(fresh);
        return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "out",
                          "backend cuda: %u frames hold CUDA_EVENT release fences already; one "
                          "is freed when its frame is released and its fences destroyed",
                          VMAFX_RELEASE_EVENTS);
    }
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): the table holds the CUevent as uintptr_t, as VmafxFence.handle carries it (ADR-1929). */
    *event = (CUevent)taken;
    return VMAFX_OK;
}

/* cuEventRecord() of a release event on the stream `arg`. */
static int record_on(uintptr_t event, void *arg)
{
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): the table holds the CUevent as uintptr_t, as VmafxFence.handle carries it (ADR-1929). */
    const CUresult res = vmafx_cuda_driver()->f->cuEventRecord((CUevent)event, (CUstream)arg);
    return res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res);
}

/* Record the frame's release event on `stream` and drop the frame's
 * reference (context pushed). */
static int release_event_record(VmafxCudaFrame *cf, CUstream stream)
{
    return vmafx_release_events_record(&release_events, &cf->release_slot, record_on,
                                       (void *)stream);
}

/* ---- Completion --------------------------------------------------------------- */

/* Host function behind the frame's last reader: signal the host release
 * fence. No CUDA call. */
static void CUDAAPI fence_done(void *arg)
{
    vmafx_host_fence_signal_unref(arg);
}

int vmafx_cuda_release_frame(VmafxCudaFrame *cf, VmafxHostFence *fence)
{
    assert(cf != NULL && cf->dev != NULL);
    VmafxCudaDevice *const dev = cf->dev;
    CudaFunctions *const f = dev->state.f;
    CUstream stream = dev->state.str;
    int err = release_event_record(cf, stream);
    if (cf->owned) {
        const CUresult res = f->cuMemFreeAsync(cf->owned, stream);
        err = err ? err : (res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res));
        cf->owned = 0;
    }
    vmafx_cuda_frame_state_free(cf);
    if (!fence) {
        return err;
    }
    const CUresult res = f->cuLaunchHostFunc(stream, fence_done, fence);
    if (res != CUDA_SUCCESS) {
        /* No callback: wait for the readers here and signal. */
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmafx: backend cuda: cannot enqueue the release (%d); waiting for the device\n",
                 (int)res);
        (void)f->cuStreamSynchronize(stream);
        vmafx_host_fence_signal_unref(fence);
    }
    return err ? err : (res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res));
}

/* ---- Public kinds ------------------------------------------------------------- */

VmafxStatus vmafx_cuda_release_fence(const VmafxReport *report, VmafxFrame *frame, uint32_t kind,
                                     VmafxFence *out)
{
    if (kind != VMAFX_FENCE_CUDA_EVENT) {
        return VMAFX_FAIL(report, kind > VMAFX_FENCE_KIND_LAST ? VMAFX_E_INVALID : VMAFX_E_NOTSUP,
                          0, VMAFX_SUBJECT_FENCE, "kind",
                          "backend cuda: a release fence of kind %u; CUDA frames signal HOST "
                          "and CUDA_EVENT release fences",
                          (unsigned)kind);
    }
    VmafxCudaFrame *const cf = frame->lane;
    CUevent event = NULL;
    (void)pthread_mutex_lock(&cf->lock);
    VmafxStatus status = vmafx_cuda_push(cf->dev) == 0 ?
                             VMAFX_OK :
                             VMAFX_FAIL(report, VMAFX_E_DEVICE, 0, VMAFX_SUBJECT_DEVICE, "frame",
                                        "backend cuda: cannot make the device current");
    if (status == VMAFX_OK) {
        status = release_event_take(report, cf, &event);
        (void)vmafx_cuda_pop(cf->dev, 0);
    }
    (void)pthread_mutex_unlock(&cf->lock);
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxFence full = VMAFX_FENCE_INIT;
    full.kind = VMAFX_FENCE_CUDA_EVENT;
    full.handle = (uintptr_t)event;
    status = vmafx_write_sized(report, out, &full, (uint32_t)sizeof(full), "out");
    if (status != VMAFX_OK) {
        (void)vmafx_release_events_drop(&release_events, (uintptr_t)event);
    }
    return status;
}

void vmafx_cuda_release_early(VmafxFrame *frame)
{
    /* The planted defect: the release event recorded when the submit
     * returns, on the legacy stream (not behind the readers on the library
     * stream), and the release callback run then. */
    VmafxCudaFrame *const cf = frame->lane;
    if (!cf || vmafx_cuda_push(cf->dev) != 0) {
        return;
    }
    (void)release_event_record(cf, NULL);
    vmafx_cuda_vulkan_release_early(cf);
    (void)vmafx_cuda_pop(cf->dev, 0);
    const VmafxFrameReleaseCallback release = frame->release;
    frame->release = NULL;
    if (release) {
        release(frame->user);
    }
}

VmafxStatus vmafx_cuda_fence_create(const VmafxReport *report, VmafxDevice *device, uint32_t kind,
                                    VmafxFence *out)
{
    if (kind != VMAFX_FENCE_CUDA_EVENT) {
        return VMAFX_FAIL(report, kind > VMAFX_FENCE_KIND_LAST ? VMAFX_E_INVALID : VMAFX_E_NOTSUP,
                          0, VMAFX_SUBJECT_FENCE, "kind",
                          "backend cuda: a fence of kind %u; a CUDA device creates HOST and "
                          "CUDA_EVENT fences (a GL sync comes from glFenceSync())",
                          (unsigned)kind);
    }
    const VmafxCudaDevice *const dev = vmafx_cuda_dev(device);
    CUevent event = NULL;
    const int pushed = vmafx_cuda_push(dev);
    CUresult res = CUDA_ERROR_UNKNOWN;
    if (pushed == 0) {
        res = dev->state.f->cuEventCreate(&event, CU_EVENT_DISABLE_TIMING);
        (void)vmafx_cuda_pop(dev, 0);
    }
    if (pushed != 0 || res != CUDA_SUCCESS) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, pushed ? pushed : (int32_t)res,
                          VMAFX_SUBJECT_FENCE, "out",
                          "backend cuda: cannot create an event (%s %d)",
                          pushed ? "context" : "CUDA error", pushed ? pushed : (int)res);
    }
    VmafxFence full = VMAFX_FENCE_INIT;
    full.kind = VMAFX_FENCE_CUDA_EVENT;
    full.handle = (uintptr_t)event;
    const VmafxStatus status = vmafx_write_sized(report, out, &full, (uint32_t)sizeof(full), "out");
    if (status != VMAFX_OK) {
        (void)dev->state.f->cuEventDestroy(event);
    }
    return status;
}

/* A CUDA event as a poll answer: 1 done, 0 pending (a release event not
 * recorded yet, or work the driver has not finished), negative on an
 * error. */
static int event_done(const void *arg)
{
    CUevent event = *(const CUevent *)arg;
    if (vmafx_release_events_unrecorded(&release_events, (uintptr_t)event)) {
        return 0;
    }
    const CUresult res = vmafx_cuda_driver()->f->cuEventQuery(event);
    return res == CUDA_SUCCESS ? 1 : res == CUDA_ERROR_NOT_READY ? 0 : -(int)res;
}

VmafxStatus vmafx_cuda_fence_wait(const VmafxReport *report, const VmafxFence *fence,
                                  uint64_t timeout_ns)
{
    if (!vmafx_cuda_driver() || fence->handle == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.handle", "%s",
                          fence->handle ? "backend cuda: no CUDA driver" :
                                          "a fence without its handle");
    }
    assert(fence->kind == VMAFX_FENCE_CUDA_EVENT);
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): a CUDA event crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
    CUevent event = (CUevent)fence->handle;
    const int state = vmafx_fence_poll(event_done, (const void *)&event, timeout_ns);
    if (state == 1) {
        return VMAFX_OK;
    }
    if (state < 0) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, state, VMAFX_SUBJECT_FENCE, "fence",
                          "backend cuda: waiting on a CUDA event failed (%d)", state);
    }
    if (timeout_ns == 0u) {
        return VMAFX_PENDING;
    }
    return VMAFX_FAIL(report, VMAFX_E_TIMEOUT, 0, VMAFX_SUBJECT_FENCE, "fence",
                      "CUDA event not signalled within %llu ns", (unsigned long long)timeout_ns);
}

VmafxStatus vmafx_cuda_fence_destroy(const VmafxReport *report, const VmafxFence *fence)
{
    const VmafxCudaDriver *const drv = vmafx_cuda_driver();
    assert(fence->kind == VMAFX_FENCE_CUDA_EVENT);
    if (!drv || fence->handle == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.handle", "%s",
                          fence->handle ? "backend cuda: no CUDA driver" :
                                          "a fence without its handle");
    }
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): a CUDA event crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
    CUevent event = (CUevent)fence->handle;
    /* A release event: the frame may still hold it, the table destroys it. */
    const bool release = vmafx_release_events_drop(&release_events, fence->handle);
    const CUresult res = release ? CUDA_SUCCESS : drv->f->cuEventDestroy(event);
    if (res != CUDA_SUCCESS) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, (int32_t)res, VMAFX_SUBJECT_FENCE,
                          "fence.handle",
                          "backend cuda: cannot destroy event 0x%llx (CUDA error %d)",
                          (unsigned long long)fence->handle, (int)res);
    }
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
