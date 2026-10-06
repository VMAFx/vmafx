/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The fence machinery the device lanes share, on the CPU (RC4 WP3,
 * ADR-2023, ADR-2092):
 *
 * - SYNC_FILE fences are checked on the host with poll() (sync_object.c): a
 *   descriptor that is not readable yet is pending (VMAFX_PENDING for a
 *   poll, VMAFX_E_TIMEOUT for a wait that expires), a readable one is
 *   signalled, a closed one is refused naming fence.fd, and the library
 *   destroys no sync_file. A pipe stands in for the sync_file: poll() treats
 *   both alike.
 * - A GL_SYNC fence without a GL context is refused naming fence.handle, and
 *   the library destroys no GL sync (sync_object.c).
 * - The release-event table (release_events.c): a release event handed out
 *   is unrecorded until the frame records it, each holder's reference is
 *   dropped once, the last one destroys the event, and a full table refuses
 *   a new event without taking it.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/internal.h"
#include "vmafx/sync_object.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static VmafxFence sync_file_fence(int fd)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    fence.kind = VMAFX_FENCE_SYNC_FILE;
    fence.fd = fd;
    return fence;
}

static char *test_sync_file_states(void)
{
    int fds[2] = {-1, -1};
    mu_assert("pipe", pipe(fds) == 0);
    const VmafxFence fence = sync_file_fence(fds[0]);
    VmafxError *error = NULL;
    mu_assert("pending: a poll answers", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_PENDING);
    mu_assert("pending: a wait expires",
              vmafx_fence_wait(&fence, 2000000u, &error) == VMAFX_E_TIMEOUT &&
                  vt_failed(&error, VMAFX_E_TIMEOUT, "fence", VMAFX_SUBJECT_FENCE));
    mu_assert("signal", write(fds[1], "x", 1) == 1);
    mu_assert("signalled", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_OK &&
                               vmafx_fence_wait(&fence, UINT64_MAX, NULL) == VMAFX_OK);
    mu_assert("the library destroys no sync_file",
              vmafx_fence_destroy(&fence, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "fence.kind", VMAFX_SUBJECT_FENCE));
    (void)close(fds[0]);
    (void)close(fds[1]);
    const VmafxFence closed = sync_file_fence(fds[0]);
    mu_assert("a closed descriptor is refused, named",
              vmafx_fence_wait(&closed, 0u, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "fence.fd", VMAFX_SUBJECT_FENCE));
    return NULL;
}

/* The import-side check: an unsignalled sync_file is VMAFX_E_BUSY naming
 * the acquire fence, a signalled one passes, a bad descriptor is named. */
static char *test_sync_file_acquire(void)
{
    int fds[2] = {-1, -1};
    mu_assert("pipe", pipe(fds) == 0);
    VmafxError *error = NULL;
    const VmafxReport report = {.error = &error, .sink = NULL, .function = __func__};
    const VmafxFence fence = sync_file_fence(fds[0]);
    mu_assert("busy", vmafx_sync_file_acquire(&report, &fence, "test") == VMAFX_E_BUSY &&
                          vt_failed(&error, VMAFX_E_BUSY, "desc.acquire", VMAFX_SUBJECT_FENCE));
    mu_assert("signal", write(fds[1], "x", 1) == 1);
    mu_assert("passes", vmafx_sync_file_acquire(&report, &fence, "test") == VMAFX_OK);
    const VmafxFence bad = sync_file_fence(-1);
    mu_assert("no descriptor",
              vmafx_sync_file_acquire(&report, &bad, "test") == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "desc.acquire.fd", VMAFX_SUBJECT_FENCE));
    VmafxFence none = VMAFX_FENCE_INIT;
    mu_assert("another kind is not its business",
              vmafx_sync_file_acquire(&report, &none, "test") == VMAFX_OK);
    (void)close(fds[0]);
    (void)close(fds[1]);
    return NULL;
}

static char *test_gl_sync_without_gl(void)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    fence.kind = VMAFX_FENCE_GL_SYNC;
    fence.handle = 1u;
    VmafxError *error = NULL;
    mu_assert("no GL context: refused, named",
              vmafx_fence_wait(&fence, 0u, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "fence.handle", VMAFX_SUBJECT_FENCE));
    mu_assert("the library destroys no GL sync",
              vmafx_fence_destroy(&fence, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "fence.kind", VMAFX_SUBJECT_FENCE));
    return NULL;
}

/* ---- Release events ------------------------------------------------------------ */

static unsigned destroyed;
static uintptr_t last_destroyed;

static void fake_destroy(uintptr_t event)
{
    destroyed++;
    last_destroyed = event;
}

static VmafxReleaseEvents table = {
    .lock = PTHREAD_MUTEX_INITIALIZER, .high = 0u, .destroy = fake_destroy};

static int fake_record(uintptr_t event, void *arg)
{
    *(uintptr_t *)arg = event;
    return 0;
}

/* Two release fences of one frame: one event, handed out unrecorded. */
static char *test_release_events_handed_out(uint32_t *slot)
{
    uintptr_t a = 0;
    uintptr_t b = 0;
    mu_assert("first fence: the frame's event",
              vmafx_release_events_take(&table, slot, 0x1000u, &a) == 0 && a == 0x1000u &&
                  *slot != 0u);
    mu_assert("second fence: the same event",
              vmafx_release_events_take(&table, slot, 0u, &b) == 0 && b == 0x1000u);
    mu_assert("unrecorded", vmafx_release_events_unrecorded(&table, 0x1000u));
    return NULL;
}

static char *test_release_events(void)
{
    uint32_t slot = 0;
    destroyed = 0;
    char *const msg = test_release_events_handed_out(&slot);
    if (msg) {
        return msg;
    }
    uintptr_t recorded = 0;
    mu_assert("recorded at the release",
              vmafx_release_events_record(&table, &slot, fake_record, &recorded) == 0 &&
                  recorded == 0x1000u && slot == 0u);
    mu_assert("no longer unrecorded", !vmafx_release_events_unrecorded(&table, 0x1000u));
    mu_assert("a fence drops its reference", vmafx_release_events_drop(&table, 0x1000u));
    mu_assert("held by the other fence", destroyed == 0u);
    mu_assert("the last reference destroys it", vmafx_release_events_drop(&table, 0x1000u) &&
                                                    destroyed == 1u && last_destroyed == 0x1000u);
    mu_assert("not an event of the table", !vmafx_release_events_drop(&table, 0x2000u));
    mu_assert("a frame without a fence records nothing",
              vmafx_release_events_record(&table, &slot, fake_record, &recorded) == 0);
    return NULL;
}

static char *test_release_events_full(void)
{
    uint32_t slots[VMAFX_RELEASE_EVENTS];
    memset(slots, 0, sizeof(slots));
    uintptr_t event = 0;
    int err = 0;
    for (uint32_t i = 0; i < VMAFX_RELEASE_EVENTS && err == 0; i++) {
        err = vmafx_release_events_take(&table, &slots[i], 0x10000u + i, &event);
    }
    mu_assert("the table holds VMAFX_RELEASE_EVENTS events", err == 0);
    uint32_t extra = 0;
    mu_assert("a full table refuses",
              vmafx_release_events_take(&table, &extra, 0x9999u, &event) == -EBUSY && extra == 0u);
    uintptr_t recorded = 0;
    for (uint32_t i = 0; i < VMAFX_RELEASE_EVENTS; i++) {
        (void)vmafx_release_events_record(&table, &slots[i], fake_record, &recorded);
        (void)vmafx_release_events_drop(&table, 0x10000u + i);
    }
    mu_assert("a freed slot is taken again",
              vmafx_release_events_take(&table, &extra, 0x9999u, &event) == 0);
    (void)vmafx_release_events_record(&table, &extra, fake_record, &recorded);
    (void)vmafx_release_events_drop(&table, 0x9999u);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_sync_file_states),    MU_TEST(test_sync_file_acquire),
        MU_TEST(test_gl_sync_without_gl),  MU_TEST(test_release_events),
        MU_TEST(test_release_events_full),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
