/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Fences of the SYCL lane (RC4 WP3, ADR-1852 design section 2.7, ADR-2091).
 *
 * A frame's release is enqueued on the device's library queue where its last
 * reference is dropped (vmafx_sycl_frame_release()): one barrier over every
 * read of the frame, on whatever queue each reader ran (ADR-2091 item 3).
 *
 * - HOST: a host task behind the barrier signals the library's host fence.
 * - SYCL_EVENT: the barrier's event. A SYCL event exists only once its
 *   command is submitted, so the library hands out a pointer to an event
 *   object of its own, kept in a table until the release recorded the
 *   barrier into it; vmafx_fence_wait() answers "pending" until then. A
 *   producer waits on the device once it is recorded: from the frame's
 *   release callback (VmafxFrameImport.release), which runs after the
 *   recording, or after a host wait returned (ADR-2023 item 3, the same
 *   rule as the CUDA lane's).
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

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

/* Write a SYCL_EVENT fence naming `slot` (one reference of it moves into
 * `out`; released here when `out` cannot take it). */
static VmafxStatus write_slot_fence(const VmafxReport *report, uint32_t slot, VmafxFence *out)
{
    VmafxFence full = VMAFX_FENCE_INIT;
    full.kind = VMAFX_FENCE_SYCL_EVENT;
    full.handle = vmafx_sycl_rt_slot_handle(slot);
    const VmafxStatus status = vmafx_write_sized(report, out, &full, (uint32_t)sizeof(full), "out");
    if (status != VMAFX_OK) {
        vmafx_sycl_rt_slot_unref(slot);
    }
    return status;
}

VmafxStatus vmafx_sycl_release_fence(const VmafxReport *report, VmafxFrame *frame, uint32_t kind,
                                     VmafxFence *out)
{
    if (kind != VMAFX_FENCE_SYCL_EVENT) {
        return VMAFX_FAIL(report, kind > VMAFX_FENCE_GL_SYNC ? VMAFX_E_INVALID : VMAFX_E_NOTSUP, 0,
                          VMAFX_SUBJECT_FENCE, "kind",
                          "backend sycl: a release fence of kind %u; SYCL frames signal HOST and "
                          "SYCL_EVENT release fences",
                          (unsigned)kind);
    }
    VmafxSyclFrame *const sf = frame->lane;
    assert(sf != NULL);
    (void)pthread_mutex_lock(&sf->lock);
    int err = 0;
    if (!sf->release_slot) {
        err = vmafx_sycl_rt_slot_new(false, &sf->release_slot); /* the frame's reference */
    }
    const uint32_t slot = sf->release_slot;
    if (!err) {
        vmafx_sycl_rt_slot_ref(slot); /* the fence's */
    }
    (void)pthread_mutex_unlock(&sf->lock);
    if (err) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, err, VMAFX_SUBJECT_FENCE, "out",
                          "backend sycl: no release event slot is free (frames with a pending "
                          "SYCL_EVENT release fence are bounded)");
    }
    return write_slot_fence(report, slot, out);
}

void vmafx_sycl_release_early(VmafxFrame *frame)
{
    /* The planted defect: the release event opened when the submit returns
     * (not behind the readers), and the release callback run then. */
    VmafxSyclFrame *const sf = frame->lane;
    if (!sf) {
        return;
    }
    (void)pthread_mutex_lock(&sf->lock);
    const uint32_t slot = sf->release_slot;
    (void)pthread_mutex_unlock(&sf->lock);
    if (slot) {
        vmafx_sycl_rt_slot_open(slot);
    }
    const VmafxFrameReleaseCallback release = frame->release;
    frame->release = NULL;
    if (release) {
        release(frame->user);
    }
}

VmafxStatus vmafx_sycl_fence_create(const VmafxReport *report, VmafxDevice *device, uint32_t kind,
                                    VmafxFence *out)
{
    (void)device;
    if (kind != VMAFX_FENCE_SYCL_EVENT) {
        return VMAFX_FAIL(report, kind > VMAFX_FENCE_GL_SYNC ? VMAFX_E_INVALID : VMAFX_E_NOTSUP, 0,
                          VMAFX_SUBJECT_FENCE, "kind",
                          "backend sycl: a fence of kind %u; a SYCL device creates SYCL_EVENT "
                          "fences (and HOST fences, as every device)",
                          (unsigned)kind);
    }
    uint32_t slot = 0;
    if (vmafx_sycl_rt_slot_new(true, &slot) != 0) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FENCE, "out",
                          "backend sycl: no event slot is free");
    }
    return write_slot_fence(report, slot, out);
}

/* A slot of the table as a poll answer. */
static int slot_done(const void *arg)
{
    return vmafx_sycl_rt_slot_poll(*(const uint32_t *)arg);
}

/* The caller's own sycl::event as a poll answer. */
static int event_done(const void *arg)
{
    return vmafx_sycl_rt_event_poll(*(const uintptr_t *)arg);
}

VmafxStatus vmafx_sycl_fence_wait(const VmafxReport *report, const VmafxFence *fence,
                                  uint64_t timeout_ns)
{
    if (fence->handle == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.handle",
                          "a SYCL_EVENT fence without its event");
    }
    uint32_t slot = 0;
    const bool library = vmafx_sycl_rt_slot_of(fence->handle, &slot) == 0;
    const int state = library ? vmafx_fence_poll(slot_done, &slot, timeout_ns) :
                                vmafx_fence_poll(event_done, &fence->handle, timeout_ns);
    if (state == 1) {
        return VMAFX_OK;
    }
    if (state < 0) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, state, VMAFX_SUBJECT_FENCE, "fence",
                          "backend sycl: waiting on a SYCL event failed (%d)", state);
    }
    if (timeout_ns == 0u) {
        return VMAFX_PENDING;
    }
    return VMAFX_FAIL(report, VMAFX_E_TIMEOUT, 0, VMAFX_SUBJECT_FENCE, "fence",
                      "SYCL event not signalled within %llu ns", (unsigned long long)timeout_ns);
}

VmafxStatus vmafx_sycl_fence_destroy(const VmafxReport *report, const VmafxFence *fence)
{
    uint32_t slot = 0;
    if (fence->handle == 0u || vmafx_sycl_rt_slot_of(fence->handle, &slot) != 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.handle",
                          "backend sycl: 0x%llx is no SYCL_EVENT fence the library returned; the "
                          "caller's own events stay the caller's",
                          (unsigned long long)fence->handle);
    }
    vmafx_sycl_rt_slot_unref(slot); /* a release event: the frame may still hold it */
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
