/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Linux dma-bufs imported on a HIP device as external memory (RC4 WP3,
 * ADR-1852 design sections 2.7 and 5.3, ADR-2092): the path the FFmpeg
 * filter reaches HIP through (DRM PRIME frames; FFmpeg has no HIP device
 * type).
 *
 * Each distinct descriptor of an import is duplicated, imported with
 * hipImportExternalMemory() (hipExternalMemoryHandleTypeOpaqueFd: on Linux
 * the AMD runtime maps the dma-buf's buffer object) and mapped whole; a
 * plane is the mapping plus its offset, read where it is. The runtime does
 * not check the size it is given against the buffer (measured, ROCm 7.2.4),
 * so the import takes the dma-buf's own size (lseek(SEEK_END)) and refuses a
 * plane whose rows reach past it. It does not take the descriptor either:
 * the duplicate is closed when the import is freed.
 *
 * Only linear layouts are read (modifier 0, DRM_FORMAT_MOD_LINEAR); a tiled
 * modifier is refused naming the plane (no de-tiling on HIP yet).
 *
 * Freeing is deferred: hipDestroyExternalMemory() and the hipFree() of a
 * mapping are not stream ordered, so a released frame's imports wait in the
 * device's graves behind an event recorded on the library stream, and are
 * freed once the stream has passed it (at the next import or release, or
 * when the device is closed).
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <hip/hip_runtime_api.h>

#include "common.h"
#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_hip.h"
#include "vmafx_hip_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#if defined(__linux__)

/* Bytes of the dma-buf behind `fd` (the kernel answers lseek(SEEK_END) on a
 * dma-buf with its size), 0 when `fd` is not one. */
static uint64_t dmabuf_size(int fd)
{
    if (fd < 0) {
        return 0u;
    }
    const off_t end = lseek(fd, 0, SEEK_END);
    return end > 0 ? (uint64_t)end : 0u;
}

/* Rows of `row` bytes, `pitch` apart, from `offset` fit in `size` bytes. */
static bool rows_fit(uint64_t offset, uint64_t pitch, uint64_t row, uint64_t rows, uint64_t size)
{
    if (rows == 0u || (rows - 1u) > (UINT64_MAX - row) / pitch) {
        return false;
    }
    const uint64_t extent = (rows - 1u) * pitch + row;
    return offset <= size && extent <= size - offset;
}

/* The layout of one dma-buf plane: a descriptor, linear, located by offset
 * and pitch (plane_index 0), its rows inside the buffer. */
static VmafxStatus check_layout(const VmafxReport *report, const VmafxImportPlane *p, uint32_t i,
                                uint64_t row)
{
    if (p->fd < 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "fd"), "plane %u has no dma-buf descriptor",
                          (unsigned)i);
    }
    if (p->modifier != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "modifier"),
                          "backend hip, memory DMABUF, plane %u: modifier 0x%llx; a HIP device "
                          "reads linear dma-bufs (modifier 0) and de-tiles nothing",
                          (unsigned)i, (unsigned long long)p->modifier);
    }
    if (p->plane_index != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "plane_index"),
                          "plane %u: plane index %u; a dma-buf plane is located by its fd, "
                          "offset and pitch (plane index 0)",
                          (unsigned)i, (unsigned)p->plane_index);
    }
    if (p->pitch < row || p->pitch > (uint64_t)PTRDIFF_MAX) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "pitch"),
                          "plane %u: pitch %llu cannot hold a row of %llu bytes", (unsigned)i,
                          (unsigned long long)p->pitch, (unsigned long long)row);
    }
    return VMAFX_OK;
}

/* The rows of plane `i` lie inside the dma-buf, whose own size bounds a
 * size the producer gave. */
static VmafxStatus check_extent(const VmafxReport *report, const VmafxImportPlane *p, uint32_t i,
                                uint64_t row, uint64_t rows)
{
    const uint64_t actual = dmabuf_size(p->fd);
    if (actual == 0u) {
        return VMAFX_FAIL(
            report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE, vmafx_import_plane_field(i, "fd"),
            "plane %u: descriptor %d is not a dma-buf (no size)", (unsigned)i, (int)p->fd);
    }
    if (p->size > actual) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "size"),
                          "plane %u: size %llu, but the dma-buf holds %llu bytes", (unsigned)i,
                          (unsigned long long)p->size, (unsigned long long)actual);
    }
    const uint64_t size = p->size ? p->size : actual;
    if (!rows_fit(p->offset, p->pitch, row, rows, size)) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "size"),
                          "plane %u: %llu rows of pitch %llu from offset %llu do not fit in the "
                          "%llu bytes of the dma-buf",
                          (unsigned)i, (unsigned long long)rows, (unsigned long long)p->pitch,
                          (unsigned long long)p->offset, (unsigned long long)size);
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_hip_dmabuf_check_plane(const VmafxReport *report, const VmafxImportPlane *p,
                                         uint32_t i, uint64_t row, uint64_t rows)
{
    const VmafxStatus status = check_layout(report, p, i, row);
    return status == VMAFX_OK ? check_extent(report, p, i, row, rows) : status;
}

/* Close what the imports of `m` hold. */
static void dmabuf_free(VmafxHipDmabuf *m)
{
    for (uint32_t k = 0; k < m->n; k++) {
        if (m->mapped[k]) {
            (void)hipFree(m->mapped[k]);
        }
        if (m->ext[k]) {
            (void)hipDestroyExternalMemory(m->ext[k]);
        }
        if (m->fd[k] >= 0) {
            (void)close(m->fd[k]);
        }
        m->mapped[k] = NULL;
        m->ext[k] = NULL;
        m->fd[k] = -1;
    }
    m->n = 0;
}

/* Import and map the dma-buf behind the caller's `fd` as entry `k`. */
static VmafxStatus import_one(const VmafxReport *report, VmafxHipDmabuf *m, uint32_t k, int fd,
                              uint32_t plane)
{
    const int own = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (own < 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, -errno, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(plane, "fd"),
                          "plane %u: cannot duplicate dma-buf descriptor %d", (unsigned)plane, fd);
    }
    m->fd[k] = own;
    m->n = k + 1u;
    hipExternalMemoryHandleDesc desc = {.type = hipExternalMemoryHandleTypeOpaqueFd,
                                        .size = dmabuf_size(own)};
    desc.handle.fd = own;
    hipError_t rc = hipImportExternalMemory(&m->ext[k], &desc);
    if (rc == hipSuccess) {
        hipExternalMemoryBufferDesc buffer = {.offset = 0u, .size = desc.size, .flags = 0u};
        rc = hipExternalMemoryGetMappedBuffer(&m->mapped[k], m->ext[k], &buffer);
    }
    if (rc == hipSuccess) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, rc == hipErrorOutOfMemory ? VMAFX_E_NOMEM : VMAFX_E_NOTSUP,
                      (int32_t)rc, VMAFX_SUBJECT_PLANE, vmafx_import_plane_field(plane, "fd"),
                      "backend hip: cannot import dma-buf descriptor %d of plane %u as external "
                      "memory: %s (%d)",
                      fd, (unsigned)plane, hipGetErrorName(rc), (int)rc);
}

VmafxStatus vmafx_hip_dmabuf_map(const VmafxReport *report, VmafxHipFrame *hf,
                                 const VmafxFrameImport *d, uint32_t n_planes, void *base[3])
{
    VmafxHipDmabuf *const m = &hf->dmabuf;
    for (uint32_t k = 0; k < VMAFX_HIP_PLANES; k++) {
        m->fd[k] = -1;
    }
    uint32_t entry_of[VMAFX_HIP_PLANES] = {0u, 0u, 0u};
    VmafxStatus status = VMAFX_OK;
    assert(n_planes <= VMAFX_HIP_PLANES);
    for (uint32_t i = 0; i < n_planes && i < VMAFX_HIP_PLANES && status == VMAFX_OK; i++) {
        /* Planes of one descriptor share its import (NV12 in one buffer). */
        uint32_t k = 0;
        while (k < i && d->plane[k].fd != d->plane[i].fd) {
            k++;
        }
        entry_of[i] = k < i ? entry_of[k] : m->n;
        if (k == i) {
            status = import_one(report, m, m->n, d->plane[i].fd, i);
        }
        assert(entry_of[i] < VMAFX_HIP_PLANES);
        base[i] =
            status == VMAFX_OK ? (uint8_t *)m->mapped[entry_of[i]] + d->plane[i].offset : NULL;
    }
    if (status != VMAFX_OK) {
        dmabuf_free(m);
    }
    return status;
}

/* ---- Graves ----------------------------------------------------------------------- */

void vmafx_hip_graves_reap(VmafxHipDevice *dev, bool all)
{
    (void)pthread_mutex_lock(&dev->graves_lock);
    VmafxHipGrave **link = &dev->graves;
    while (*link) {
        VmafxHipGrave *const g = *link;
        if (!all && hipEventQuery(g->passed) != hipSuccess) {
            link = &g->next;
            continue;
        }
        *link = g->next;
        dmabuf_free(&g->dmabuf);
        (void)hipEventDestroy(g->passed);
        free(g);
    }
    (void)pthread_mutex_unlock(&dev->graves_lock);
}

int vmafx_hip_dmabuf_bury(VmafxHipFrame *hf)
{
    assert(hf != NULL && hf->dev != NULL);
    if (hf->dmabuf.n == 0u) {
        return 0;
    }
    VmafxHipDevice *const dev = hf->dev;
    VmafxHipGrave *const g = calloc(1, sizeof(*g));
    hipError_t rc =
        g ? hipEventCreateWithFlags(&g->passed, hipEventDisableTiming) : hipErrorOutOfMemory;
    if (rc == hipSuccess) {
        rc = hipEventRecord(g->passed, dev->str);
    }
    if (rc != hipSuccess) {
        /* No event to free behind: wait for the readers here. */
        (void)hipStreamSynchronize(dev->str);
        dmabuf_free(&hf->dmabuf);
        if (g && g->passed) {
            (void)hipEventDestroy(g->passed);
        }
        free(g);
        return vmaf_hip_rc_to_errno(rc);
    }
    g->dmabuf = hf->dmabuf;
    hf->dmabuf.n = 0;
    (void)pthread_mutex_lock(&dev->graves_lock);
    g->next = dev->graves;
    dev->graves = g;
    (void)pthread_mutex_unlock(&dev->graves_lock);
    return 0;
}

#else /* !__linux__ */

VmafxStatus vmafx_hip_dmabuf_check_plane(const VmafxReport *report, const VmafxImportPlane *p,
                                         uint32_t i, uint64_t row, uint64_t rows)
{
    (void)p;
    (void)row;
    (void)rows;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PLANE,
                      vmafx_import_plane_field(i, "fd"), "dma-bufs are Linux memory");
}

VmafxStatus vmafx_hip_dmabuf_map(const VmafxReport *report, VmafxHipFrame *hf,
                                 const VmafxFrameImport *d, uint32_t n_planes, void *base[3])
{
    (void)hf;
    (void)d;
    (void)n_planes;
    (void)base;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                      "dma-bufs are Linux memory");
}

void vmafx_hip_graves_reap(VmafxHipDevice *dev, bool all)
{
    (void)dev;
    (void)all;
}

int vmafx_hip_dmabuf_bury(VmafxHipFrame *hf)
{
    (void)hf;
    return 0;
}

#endif /* __linux__ */

/* NOLINTEND(modernize-use-nullptr) */
