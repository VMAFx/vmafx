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
 *   it handed out in a table until it is recorded, and vmafx_fence_wait()
 *   answers "pending" for such an event instead of asking the driver. A
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

/* Release events handed out and not yet both recorded and destroyed by
 * every holder: frames with a pending CUDA_EVENT release fence (HISS-02
 * bound; a decoder pool holds a few dozen). */
#define VMAFX_CUDA_RELEASE_EVENTS 4096u

/* One release event: the frame holds a reference until it records it, each
 * fence handed out one until the caller destroys it. */
typedef struct ReleaseEvent {
    CUevent event; /* NULL: a free slot */
    uint32_t refs;
    bool recorded;
} ReleaseEvent;

static ReleaseEvent release_events[VMAFX_CUDA_RELEASE_EVENTS];
static uint32_t release_high; /* slots below it were used at least once */
static pthread_mutex_t release_lock = PTHREAD_MUTEX_INITIALIZER;

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

/* Drop one reference of slot `i` (lock held); the last destroys the event. */
static void release_unref_locked(uint32_t i)
{
    ReleaseEvent *const r = &release_events[i];
    assert(r->event != NULL && r->refs > 0u);
    if (--r->refs == 0u) {
        (void)vmafx_cuda_driver()->f->cuEventDestroy(r->event);
        r->event = NULL;
        r->recorded = false;
    }
}

/* The slot of a free entry for a new event (lock held), or UINT32_MAX. */
static uint32_t release_free_slot_locked(void)
{
    for (uint32_t i = 0; i < release_high; i++) {
        if (!release_events[i].event) {
            return i;
        }
    }
    return release_high < VMAFX_CUDA_RELEASE_EVENTS ? release_high++ : UINT32_MAX;
}

/* The slot holding `event` (lock held), or UINT32_MAX. */
static uint32_t release_find_locked(CUevent event)
{
    for (uint32_t i = 0; i < release_high; i++) {
        if (release_events[i].event == event) {
            return i;
        }
    }
    return UINT32_MAX;
}

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
    (void)pthread_mutex_lock(&release_lock);
    const uint32_t slot = cf->release_slot ? cf->release_slot - 1u : release_free_slot_locked();
    if (slot != UINT32_MAX && fresh) {
        release_events[slot] = (ReleaseEvent){.event = fresh, .refs = 1u, .recorded = false};
        cf->release_slot = slot + 1u;
    }
    if (slot != UINT32_MAX) {
        release_events[slot].refs++;
        *event = release_events[slot].event;
    }
    (void)pthread_mutex_unlock(&release_lock);
    if (slot == UINT32_MAX) {
        (void)cf->dev->state.f->cuEventDestroy(fresh);
        return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "out",
                          "backend cuda: %u frames hold CUDA_EVENT release fences already; one "
                          "is freed when its frame is released and its fences destroyed",
                          VMAFX_CUDA_RELEASE_EVENTS);
    }
    return VMAFX_OK;
}

/* Record the frame's release event on `stream` and drop the frame's
 * reference (context pushed). */
static int release_event_record(VmafxCudaFrame *cf, CUstream stream)
{
    if (!cf->release_slot) {
        return 0;
    }
    const uint32_t slot = cf->release_slot - 1u;
    cf->release_slot = 0;
    (void)pthread_mutex_lock(&release_lock);
    const CUresult res = cf->dev->state.f->cuEventRecord(release_events[slot].event, stream);
    release_events[slot].recorded = true;
    release_unref_locked(slot);
    (void)pthread_mutex_unlock(&release_lock);
    return res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res);
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
        return VMAFX_FAIL(report, kind > VMAFX_FENCE_GL_SYNC ? VMAFX_E_INVALID : VMAFX_E_NOTSUP, 0,
                          VMAFX_SUBJECT_FENCE, "kind",
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
        (void)pthread_mutex_lock(&release_lock);
        release_unref_locked(release_find_locked(event));
        (void)pthread_mutex_unlock(&release_lock);
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
        return VMAFX_FAIL(report, kind > VMAFX_FENCE_GL_SYNC ? VMAFX_E_INVALID : VMAFX_E_NOTSUP, 0,
                          VMAFX_SUBJECT_FENCE, "kind",
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

/* What a poll of a fence looks at. */
typedef struct PolledFence {
    CUevent event;
    uintptr_t sync;
} PolledFence;

/* A CUDA event as a poll answer: 1 done, 0 pending (a release event not
 * recorded yet, or work the driver has not finished), negative on an
 * error. */
static int event_done(const void *arg)
{
    CUevent event = ((const PolledFence *)arg)->event;
    (void)pthread_mutex_lock(&release_lock);
    const uint32_t slot = release_find_locked(event);
    const bool unrecorded = slot != UINT32_MAX && !release_events[slot].recorded;
    (void)pthread_mutex_unlock(&release_lock);
    if (unrecorded) {
        return 0;
    }
    const CUresult res = vmafx_cuda_driver()->f->cuEventQuery(event);
    return res == CUDA_SUCCESS ? 1 : res == CUDA_ERROR_NOT_READY ? 0 : -(int)res;
}

/* A GL sync as a poll answer, through glClientWaitSync without a wait. */
static int gl_sync_done(const void *arg)
{
    return vmafx_cuda_gl_sync_wait(((const PolledFence *)arg)->sync, 0u);
}

VmafxStatus vmafx_cuda_fence_wait(const VmafxReport *report, const VmafxFence *fence,
                                  uint64_t timeout_ns)
{
    if (!vmafx_cuda_driver() || fence->handle == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.handle", "%s",
                          fence->handle ? "backend cuda: no CUDA driver" :
                                          "a fence without its handle");
    }
    const bool event = fence->kind == VMAFX_FENCE_CUDA_EVENT;
    PolledFence polled = {.event = NULL, .sync = fence->handle};
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): a CUDA event crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
    polled.event = (CUevent)fence->handle;
    const int state = vmafx_fence_poll(event ? event_done : gl_sync_done, &polled, timeout_ns);
    if (state == 1) {
        return VMAFX_OK;
    }
    if (state < 0) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, state, VMAFX_SUBJECT_FENCE, "fence",
                          "backend cuda: waiting on a %s failed (%d)",
                          event ? "CUDA event" : "GL sync", state);
    }
    if (timeout_ns == 0u) {
        return VMAFX_PENDING;
    }
    return VMAFX_FAIL(report, VMAFX_E_TIMEOUT, 0, VMAFX_SUBJECT_FENCE, "fence",
                      "%s not signalled within %llu ns", event ? "CUDA event" : "GL sync",
                      (unsigned long long)timeout_ns);
}

VmafxStatus vmafx_cuda_fence_destroy(const VmafxReport *report, const VmafxFence *fence)
{
    const VmafxCudaDriver *const drv = vmafx_cuda_driver();
    if (fence->kind != VMAFX_FENCE_CUDA_EVENT) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                          "the library returns no GL sync; delete it with glDeleteSync()");
    }
    assert(fence->kind == VMAFX_FENCE_CUDA_EVENT);
    if (!drv || fence->handle == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.handle", "%s",
                          fence->handle ? "backend cuda: no CUDA driver" :
                                          "a fence without its handle");
    }
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): a CUDA event crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
    CUevent event = (CUevent)fence->handle;
    (void)pthread_mutex_lock(&release_lock);
    const uint32_t slot = release_find_locked(event);
    if (slot != UINT32_MAX) {
        release_unref_locked(slot); /* a release event: the frame may still hold it */
    }
    (void)pthread_mutex_unlock(&release_lock);
    const CUresult res = slot == UINT32_MAX ? drv->f->cuEventDestroy(event) : CUDA_SUCCESS;
    if (res != CUDA_SUCCESS) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, (int32_t)res, VMAFX_SUBJECT_FENCE,
                          "fence.handle",
                          "backend cuda: cannot destroy event 0x%llx (CUDA error %d)",
                          (unsigned long long)fence->handle, (int)res);
    }
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
