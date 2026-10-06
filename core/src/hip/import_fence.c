/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Fences of the HIP lane (RC4 WP3, ADR-1852 design section 2.7, ADR-2092).
 *
 * A frame's release is enqueued on the library stream where its last
 * reference is dropped (vmafx_hip_frame_release()), behind every reader
 * (every read of a frame of the device is enqueued on that stream, and the
 * twins' streams wait for it, picture_hip.h):
 *
 * - HOST: a host function there signals the library's host fence; the
 *   callback makes no HIP call.
 * - HIP_EVENT: the frame's release event is recorded there. A HIP event
 *   cannot be recorded ahead of time, and a stream wait on an event never
 *   recorded returns at once (hipEventQuery() answers hipSuccess for it,
 *   measured on ROCm 7.2.4), so the release events handed out wait in the
 *   lane's table until they are recorded (release_events.c, shared with the
 *   CUDA lane); vmafx_fence_wait() answers "pending" for them, and a
 *   producer waits on the device from the frame's release callback, which
 *   runs after the recording.
 * - SYNC_FILE: refused. A sync_file is a kernel dma-fence; HIP queues are
 *   user-mode queues the kernel does not schedule, so no HIP operation
 *   signals one, and importing a DRM syncobj as a HIP external semaphore
 *   aborts the process in ROCm 7.2.4 (ADR-2092). A producer of dma-buf
 *   frames waits on HOST or HIP_EVENT, or keeps its buffer until the
 *   release callback.
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <hip/hip_runtime_api.h>

#include "common.h"
#include "hip_handle.h"
#include "log.h"
#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_hip.h"
#include "vmafx_hip_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* A HIP release event the table drops last. */
static void destroy_event(uintptr_t event)
{
    (void)hipEventDestroy(vmaf_hip_event_of(event));
}

/* The HIP lane's release events (release_events.c, ADR-2023 item 3). */
static VmafxReleaseEvents release_events = {
    .lock = PTHREAD_MUTEX_INITIALIZER, .high = 0u, .destroy = destroy_event};

/* ---- Frame state -------------------------------------------------------------- */

VmafxHipFrame *vmafx_hip_frame_state_new(VmafxHipDevice *dev)
{
    VmafxHipFrame *const hf = calloc(1, sizeof(*hf));
    if (!hf) {
        return NULL;
    }
    if (pthread_mutex_init(&hf->lock, NULL) != 0) {
        free(hf);
        return NULL;
    }
    hf->dev = dev;
    for (uint32_t k = 0; k < VMAFX_HIP_PLANES; k++) {
        hf->dmabuf.fd[k] = -1;
    }
    return hf;
}

void vmafx_hip_frame_state_free(VmafxHipFrame *hf)
{
    if (hf) {
        (void)pthread_mutex_destroy(&hf->lock);
        free(hf);
    }
}

/* ---- Release events ------------------------------------------------------------- */

/* The frame's release event with one more reference for the caller, created
 * on first use (frame locked). */
static VmafxStatus release_event_take(const VmafxReport *report, VmafxHipFrame *hf,
                                      uintptr_t *event)
{
    hipEvent_t fresh = NULL;
    if (!hf->release_slot) {
        const hipError_t rc = hipEventCreateWithFlags(&fresh, hipEventDisableTiming);
        if (rc != hipSuccess) {
            return vmafx_hip_failed(report, rc, VMAFX_SUBJECT_FENCE, "out",
                                    "hipEventCreateWithFlags");
        }
    }
    const uintptr_t bits = fresh ? vmaf_hip_event_bits(fresh) : 0u;
    if (vmafx_release_events_take(&release_events, &hf->release_slot, bits, event) != 0) {
        (void)hipEventDestroy(fresh);
        return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "out",
                          "backend hip: %u frames hold HIP_EVENT release fences already; one is "
                          "freed when its frame is released and its fences destroyed",
                          VMAFX_RELEASE_EVENTS);
    }
    return VMAFX_OK;
}

/* hipEventRecord() of a release event on the library stream of the device
 * `arg`. */
static int record_on(uintptr_t event, void *arg)
{
    const VmafxHipDevice *const dev = arg;
    return vmaf_hip_rc_to_errno(hipEventRecord(vmaf_hip_event_of(event), dev->str));
}

/* The stream of the planted early release (test-only, made once). */
static hipStream_t early_stream;
static pthread_once_t early_once = PTHREAD_ONCE_INIT;

static void early_stream_make(void)
{
    if (hipStreamCreateWithFlags(&early_stream, hipStreamNonBlocking) != hipSuccess) {
        early_stream = NULL;
    }
}

/* The planted early release: the event recorded on a stream of its own,
 * which waits for nothing of the frame's readers (the null stream would:
 * the readers make it wait for their copies). Made once, because creating
 * a stream waits for the device. */
static int record_early(uintptr_t event, void *arg)
{
    (void)arg;
    (void)pthread_once(&early_once, early_stream_make);
    return vmaf_hip_rc_to_errno(hipEventRecord(vmaf_hip_event_of(event), early_stream));
}

/* ---- Completion --------------------------------------------------------------- */

/* Host function behind the frame's last reader: signal the host release
 * fence. No HIP call. */
static void fence_done(void *arg)
{
    vmafx_host_fence_signal_unref(arg);
}

/* The host release fence behind the readers on the library stream. */
static int enqueue_host_release(const VmafxHipDevice *dev, VmafxHostFence *fence)
{
    const hipError_t rc = hipLaunchHostFunc(dev->str, fence_done, fence);
    if (rc != hipSuccess) {
        /* No callback: wait for the readers here and signal. */
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmafx: backend hip: cannot enqueue the release (%d); waiting for the device\n",
                 (int)rc);
        (void)hipStreamSynchronize(dev->str);
        vmafx_host_fence_signal_unref(fence);
    }
    return vmaf_hip_rc_to_errno(rc);
}

int vmafx_hip_release_frame(VmafxHipFrame *hf, VmafxHostFence *fence)
{
    assert(hf != NULL && hf->dev != NULL);
    VmafxHipDevice *const dev = hf->dev;
    int err = vmafx_release_events_record(&release_events, &hf->release_slot, record_on, dev);
    if (hf->owned) {
        const int freed = vmaf_hip_rc_to_errno(hipFreeAsync(hf->owned, dev->str));
        err = err ? err : freed;
        hf->owned = NULL;
    }
    const int buried = vmafx_hip_dmabuf_bury(hf);
    err = err ? err : buried;
    vmafx_hip_frame_state_free(hf);
    const int host = fence ? enqueue_host_release(dev, fence) : 0;
    /* Free the external memory of frames the stream has passed by now. */
    vmafx_hip_graves_reap(dev, false);
    return err ? err : host;
}

/* ---- Public kinds ------------------------------------------------------------- */

/* A release fence kind a HIP frame cannot signal, named. */
static VmafxStatus release_kind_refused(const VmafxReport *report, uint32_t kind)
{
    if (kind == VMAFX_FENCE_SYNC_FILE) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "kind",
                          "backend hip: a SYNC_FILE release fence; no HIP operation signals a "
                          "kernel fence (HIP queues are user-mode queues), so HIP frames signal "
                          "HOST and HIP_EVENT release fences and call the release callback");
    }
    return VMAFX_FAIL(report, kind > VMAFX_FENCE_GL_SYNC ? VMAFX_E_INVALID : VMAFX_E_NOTSUP, 0,
                      VMAFX_SUBJECT_FENCE, "kind",
                      "backend hip: a release fence of kind %u; HIP frames signal HOST and "
                      "HIP_EVENT release fences",
                      (unsigned)kind);
}

VmafxStatus vmafx_hip_release_fence(const VmafxReport *report, VmafxFrame *frame, uint32_t kind,
                                    VmafxFence *out)
{
    if (kind != VMAFX_FENCE_HIP_EVENT) {
        return release_kind_refused(report, kind);
    }
    VmafxHipFrame *const hf = frame->lane;
    uintptr_t event = 0u;
    (void)pthread_mutex_lock(&hf->lock);
    VmafxStatus status = vmafx_hip_bind(hf->dev) == 0 ?
                             VMAFX_OK :
                             VMAFX_FAIL(report, VMAFX_E_DEVICE, 0, VMAFX_SUBJECT_DEVICE, "frame",
                                        "backend hip: cannot make the device current");
    if (status == VMAFX_OK) {
        status = release_event_take(report, hf, &event);
    }
    (void)pthread_mutex_unlock(&hf->lock);
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxFence full = VMAFX_FENCE_INIT;
    full.kind = VMAFX_FENCE_HIP_EVENT;
    full.handle = event;
    status = vmafx_write_sized(report, out, &full, (uint32_t)sizeof(full), "out");
    if (status != VMAFX_OK) {
        (void)vmafx_release_events_drop(&release_events, event);
    }
    return status;
}

void vmafx_hip_release_early(VmafxFrame *frame)
{
    /* The planted defect: the release event recorded when the submit
     * returns, on a stream of its own (not behind the readers on the
     * library stream), and the release callback run then. */
    VmafxHipFrame *const hf = frame->lane;
    if (!hf || vmafx_hip_bind(hf->dev) != 0) {
        return;
    }
    (void)vmafx_release_events_record(&release_events, &hf->release_slot, record_early, NULL);
    const VmafxFrameReleaseCallback release = frame->release;
    frame->release = NULL;
    if (release) {
        release(frame->user);
    }
}

VmafxStatus vmafx_hip_fence_create(const VmafxReport *report, VmafxDevice *device, uint32_t kind,
                                   VmafxFence *out)
{
    if (kind != VMAFX_FENCE_HIP_EVENT) {
        return VMAFX_FAIL(report, kind > VMAFX_FENCE_GL_SYNC ? VMAFX_E_INVALID : VMAFX_E_NOTSUP, 0,
                          VMAFX_SUBJECT_FENCE, "kind",
                          "backend hip: a fence of kind %u; a HIP device creates HOST and "
                          "HIP_EVENT fences (a GL sync comes from glFenceSync(), a sync_file "
                          "from its producer)",
                          (unsigned)kind);
    }
    const VmafxHipDevice *const dev = vmafx_hip_dev(device);
    hipEvent_t event = NULL;
    hipError_t rc = hipSetDevice(dev->index);
    if (rc == hipSuccess) {
        rc = hipEventCreateWithFlags(&event, hipEventDisableTiming);
    }
    if (rc != hipSuccess) {
        return vmafx_hip_failed(report, rc, VMAFX_SUBJECT_FENCE, "out", "hipEventCreateWithFlags");
    }
    VmafxFence full = VMAFX_FENCE_INIT;
    full.kind = VMAFX_FENCE_HIP_EVENT;
    full.handle = vmaf_hip_event_bits(event);
    const VmafxStatus status = vmafx_write_sized(report, out, &full, (uint32_t)sizeof(full), "out");
    if (status != VMAFX_OK) {
        (void)hipEventDestroy(event);
    }
    return status;
}

/* A HIP event as a poll answer: 1 done, 0 pending (a release event not
 * recorded yet, or work the device has not finished), negative on an
 * error. */
static int event_done(const void *arg)
{
    const uintptr_t event = *(const uintptr_t *)arg;
    if (vmafx_release_events_unrecorded(&release_events, event)) {
        return 0;
    }
    const hipError_t rc = hipEventQuery(vmaf_hip_event_of(event));
    return rc == hipSuccess ? 1 : rc == hipErrorNotReady ? 0 : -(int)rc;
}

VmafxStatus vmafx_hip_fence_wait(const VmafxReport *report, const VmafxFence *fence,
                                 uint64_t timeout_ns)
{
    if (fence->handle == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.handle",
                          "a fence without its handle");
    }
    assert(fence->kind == VMAFX_FENCE_HIP_EVENT);
    const int state = vmafx_fence_poll(event_done, &fence->handle, timeout_ns);
    if (state == 1) {
        return VMAFX_OK;
    }
    if (state < 0) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, state, VMAFX_SUBJECT_FENCE, "fence",
                          "backend hip: waiting on a HIP event failed (%d)", state);
    }
    if (timeout_ns == 0u) {
        return VMAFX_PENDING;
    }
    return VMAFX_FAIL(report, VMAFX_E_TIMEOUT, 0, VMAFX_SUBJECT_FENCE, "fence",
                      "HIP event not signalled within %llu ns", (unsigned long long)timeout_ns);
}

VmafxStatus vmafx_hip_fence_destroy(const VmafxReport *report, const VmafxFence *fence)
{
    assert(fence->kind == VMAFX_FENCE_HIP_EVENT);
    if (fence->handle == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.handle",
                          "a fence without its handle");
    }
    /* A release event: the frame may still hold it, the table destroys it. */
    if (vmafx_release_events_drop(&release_events, fence->handle)) {
        return VMAFX_OK;
    }
    const hipError_t rc = hipEventDestroy(vmaf_hip_event_of(fence->handle));
    if (rc != hipSuccess) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, (int32_t)rc, VMAFX_SUBJECT_FENCE, "fence.handle",
                          "backend hip: cannot destroy event 0x%llx: %s (%d)",
                          (unsigned long long)fence->handle, hipGetErrorName(rc), (int)rc);
    }
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
