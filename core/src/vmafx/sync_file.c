/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Linux sync_file descriptors as acquire fences (RC4 WP3, ADR-2092).
 *
 * A sync_file is the kernel's handle on a dma-fence: a producer whose GPU
 * work the kernel schedules (a GL or Vulkan driver, a video decoder) hands
 * one out, or the consumer exports the fences of a dma-buf with
 * DMA_BUF_IOCTL_EXPORT_SYNC_FILE (Linux 6.0). It becomes readable when the
 * fence signals. A device whose queues the kernel does not see (HIP) cannot
 * wait on it, so it is checked on the host: a signalled sync_file is passed,
 * an unsignalled one makes the import VMAFX_E_BUSY, and
 * vmafx_context_import_frame() waits on it with poll() (bounded) and retries
 * once (decision D8). The descriptor stays the caller's: the check reads its
 * state and keeps nothing.
 */

#include <errno.h>
#include <stdint.h>

#if defined(__linux__)
#include <poll.h>
#endif

#include "error_internal.h"
#include "frame_import_hooks.h"
#include "internal.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#if defined(__linux__)
/* The sync_file as a poll answer: 1 signalled, 0 not yet, negative on an
 * error (a descriptor that is not a sync_file reports POLLNVAL or never
 * becomes readable; the caller names it). */
static int sync_file_state(const void *arg)
{
    struct pollfd p = {.fd = *(const int *)arg, .events = POLLIN, .revents = 0};
    const int n = poll(&p, 1u, 0);
    if (n < 0) {
        return errno == EINTR ? 0 : -errno;
    }
    if (n > 0 && (p.revents & (POLLNVAL | POLLERR))) {
        return -EBADF;
    }
    return (n > 0 && (p.revents & POLLIN)) ? 1 : 0;
}

int vmafx_sync_file_wait(int fd, uint64_t timeout_ns)
{
    if (fd < 0) {
        return -EBADF;
    }
    /* vmafx_fence_poll() sleeps between looks on the clock host fences use,
     * the virtual test clock included. */
    return vmafx_fence_poll(sync_file_state, &fd, timeout_ns);
}
#else
int vmafx_sync_file_wait(int fd, uint64_t timeout_ns)
{
    (void)fd;
    (void)timeout_ns;
    return -ENOSYS;
}
#endif

VmafxStatus vmafx_sync_file_acquire(const VmafxReport *report, const VmafxFence *acquire,
                                    const char *backend)
{
    if (acquire->kind != VMAFX_FENCE_SYNC_FILE) {
        return VMAFX_OK;
    }
    const int state = vmafx_sync_file_wait(acquire->fd, 0u);
    if (state == 1 || (state == 0 && vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT))) {
        return VMAFX_OK;
    }
    if (state < 0) {
        return VMAFX_FAIL(report, state == -ENOSYS ? VMAFX_E_NOTSUP : VMAFX_E_INVALID, state,
                          VMAFX_SUBJECT_FENCE, "desc.acquire.fd",
                          "backend %s: descriptor %d is not a sync_file this host can poll (%d)",
                          backend, (int)acquire->fd, state);
    }
    return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "desc.acquire",
                      "the producer's sync_file has not signalled; the %s device waits on a "
                      "sync_file on the host (vmafx_context_import_frame() waits and retries "
                      "once)",
                      backend);
}

/* NOLINTEND(modernize-use-nullptr) */
