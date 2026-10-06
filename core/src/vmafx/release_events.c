/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Release events handed out before they are recorded (RC4 WP3, ADR-2023
 * item 3, ADR-2092): the table the CUDA and HIP lanes keep their
 * CUDA_EVENT / HIP_EVENT release fences in.
 *
 * A device event cannot be recorded ahead of time, and a stream wait on an
 * event never recorded returns at once; a release fence is handed out when
 * the caller asks (before the frame is submitted), but its event is recorded
 * where the frame's last reference is dropped. So the lane keeps every
 * release event it handed out in a table until it is recorded:
 * vmafx_fence_wait() answers "pending" for such an event instead of asking
 * the runtime, and the release callback runs after the recording.
 *
 * Each entry is counted: the frame holds a reference until it records the
 * event, each fence handed out one until the caller destroys it; the last
 * reference destroys the event (the lane's `destroy`).
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Drop one reference of slot `i` (lock held); the last destroys the event. */
static void unref_locked(VmafxReleaseEvents *t, uint32_t i)
{
    VmafxReleaseEvent *const r = &t->slot[i];
    assert(r->event != 0u && r->refs > 0u);
    if (--r->refs == 0u) {
        t->destroy(r->event);
        r->event = 0u;
        r->recorded = false;
    }
}

/* The slot of a free entry for a new event (lock held), or UINT32_MAX. */
static uint32_t free_slot_locked(VmafxReleaseEvents *t)
{
    for (uint32_t i = 0; i < t->high; i++) {
        if (t->slot[i].event == 0u) {
            return i;
        }
    }
    return t->high < VMAFX_RELEASE_EVENTS ? t->high++ : UINT32_MAX;
}

/* The slot holding `event` (lock held), or UINT32_MAX. */
static uint32_t find_locked(const VmafxReleaseEvents *t, uintptr_t event)
{
    for (uint32_t i = 0; i < t->high; i++) {
        if (t->slot[i].event == event) {
            return i;
        }
    }
    return UINT32_MAX;
}

int vmafx_release_events_take(VmafxReleaseEvents *t, uint32_t *slot1, uintptr_t fresh,
                              uintptr_t *event)
{
    assert((*slot1 == 0u) == (fresh != 0u));
    (void)pthread_mutex_lock(&t->lock);
    const uint32_t slot = *slot1 ? *slot1 - 1u : free_slot_locked(t);
    if (slot != UINT32_MAX && fresh) {
        t->slot[slot] = (VmafxReleaseEvent){.event = fresh, .refs = 1u, .recorded = false};
        *slot1 = slot + 1u;
    }
    if (slot != UINT32_MAX) {
        t->slot[slot].refs++;
        *event = t->slot[slot].event;
    }
    (void)pthread_mutex_unlock(&t->lock);
    return slot == UINT32_MAX ? -EBUSY : 0;
}

int vmafx_release_events_record(VmafxReleaseEvents *t, uint32_t *slot1,
                                int (*record)(uintptr_t event, void *arg), void *arg)
{
    if (*slot1 == 0u) {
        return 0;
    }
    const uint32_t slot = *slot1 - 1u;
    *slot1 = 0u;
    (void)pthread_mutex_lock(&t->lock);
    const int err = record(t->slot[slot].event, arg);
    t->slot[slot].recorded = true;
    unref_locked(t, slot);
    (void)pthread_mutex_unlock(&t->lock);
    return err;
}

bool vmafx_release_events_unrecorded(VmafxReleaseEvents *t, uintptr_t event)
{
    (void)pthread_mutex_lock(&t->lock);
    const uint32_t slot = find_locked(t, event);
    const bool unrecorded = slot != UINT32_MAX && !t->slot[slot].recorded;
    (void)pthread_mutex_unlock(&t->lock);
    return unrecorded;
}

bool vmafx_release_events_drop(VmafxReleaseEvents *t, uintptr_t event)
{
    (void)pthread_mutex_lock(&t->lock);
    const uint32_t slot = find_locked(t, event);
    if (slot != UINT32_MAX) {
        unref_locked(t, slot); /* a release event: the frame may still hold it */
    }
    (void)pthread_mutex_unlock(&t->lock);
    return slot != UINT32_MAX;
}

/* NOLINTEND(modernize-use-nullptr) */
