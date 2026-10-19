/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Synchronisation objects the VMAFx fences wrap that belong to no backend
 * (RC4 WP3, ADR-2091): Linux sync_file descriptors (VMAFX_FENCE_SYNC_FILE),
 * the implicit fences of a Linux dma-buf, and OpenGL sync objects
 * (VMAFX_FENCE_GL_SYNC). One implementation for every lane that imports
 * them (HISS-19): fence.c waits on them for vmafx_fence_wait(), the lanes
 * check them at import. Nothing here is exported.
 */

#ifndef VMAFX_SYNC_OBJECT_H
#define VMAFX_SYNC_OBJECT_H

#include <stdbool.h>
#include <stdint.h>

#include "internal.h"
#include "vmafx/vmafx.h"

/* Whether sync_file `fd` is signalled: 1 yes, 0 not within `timeout_ns`
 * (0: a poll), a negative errno on a failure; -ENOTSUP off Linux. */
int vmafx_sync_file_wait(int fd, uint64_t timeout_ns);

/* The fences a reader of dma-buf `fd` must wait for (its writers), exported
 * as a new sync_file descriptor (DMA_BUF_IOCTL_EXPORT_SYNC_FILE with
 * DMA_BUF_SYNC_READ, Linux 6.0): the descriptor, or a negative errno
 * (-ENOTTY on an older kernel, -ENOTSUP off Linux). */
int vmafx_dmabuf_export_read_fence(int fd);

/* Attach sync_file `sync_file` to dma-buf `fd` as a reader's fence
 * (DMA_BUF_IOCTL_IMPORT_SYNC_FILE with DMA_BUF_SYNC_READ): a later writer
 * that synchronises implicitly waits for it. 0 or a negative errno. */
int vmafx_dmabuf_import_read_fence(int fd, int sync_file);

/* Host wait, at most `timeout_ns` per plane, until the writers of the
 * dma-bufs of a DMABUF import descriptor finished (their implicit fences):
 * the D8 helper's wait before its retry when an import was VMAFX_E_BUSY on
 * an implicit fence. A no-op off Linux. */
void vmafx_dmabuf_wait_writers(const VmafxFrameImport *d, uint64_t timeout_ns);

/* Whether `fd` is a Linux dma-buf (its /proc/self/fd link names a dmabuf
 * inode). False off Linux and for any other descriptor. */
bool vmafx_fd_is_dmabuf(int fd);

/* Whether GL sync object `sync` of the GL context current on this thread is
 * signalled: 1 yes, 0 not within `timeout_ns`, -1 on a GL error, without a
 * current context or without GL. GL entry points are resolved at run time;
 * no GL header or library is a build dependency. */
int vmafx_gl_sync_wait(uintptr_t sync, uint64_t timeout_ns);

/* An acquire fence of kind GL_SYNC as an import on `backend` sees it:
 * VMAFX_OK when signalled (or the planted skipped wait), VMAFX_E_BUSY when
 * not yet (vmafx_context_import_frame() waits on the host and retries once),
 * VMAFX_E_INVALID when GL cannot answer. VMAFX_OK for any other kind. */
VmafxStatus vmafx_gl_sync_acquire(const VmafxReport *report, const VmafxFence *acquire,
                                  const char *backend);

/* The same for an acquire fence of kind SYNC_FILE. */
VmafxStatus vmafx_sync_file_acquire(const VmafxReport *report, const VmafxFence *acquire,
                                    const char *backend);

#endif /* VMAFX_SYNC_OBJECT_H */
