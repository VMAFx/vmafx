/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Linux dma-buf imports of the VMAFx SYCL lane (RC4 WP3, ADR-2091; not the
 * VA-surface import of dmabuf_import.cpp, whose Level Zero import it reuses).
 *
 * Each plane's dma-buf is duplicated (the descriptor the caller passed stays
 * the caller's) and imported as device memory once per descriptor; a linear
 * plane (modifier 0) is bound in it at its offset and pitch, an Intel Y-tiled
 * or Tile4 plane is de-tiled on the device (detile.h, ADR-1121), any other
 * modifier is refused naming the plane. The dma-buf's implicit write fences
 * (DMA_BUF_IOCTL_EXPORT_SYNC_FILE) are honoured: a pending one makes the
 * import VMAFX_E_BUSY, and the D8 helper waits on it before its retry.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "picture.h"
#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_sycl.h"
#include "vmafx_sycl_internal.h"
#include "vmafx_sycl_rt.h"

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>

#include "vmafx/sync_object.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#ifdef __linux__

/* DRM format modifiers (drm_fourcc.h: fourcc_mod_code(INTEL, 2 / 9)). */
#define VMAFX_MOD_LINEAR 0x0ull
#define VMAFX_MOD_INTEL_Y_TILED 0x0100000000000002ull
#define VMAFX_MOD_INTEL_4_TILED 0x0100000000000009ull
/* Bytes of an Intel tile row and rows of a tile. */
#define VMAFX_TILE_WIDTH 128u
#define VMAFX_TILE_ROWS 32u

static const char *const plane_fd_names[] = {"desc.plane[0].fd", "desc.plane[1].fd",
                                             "desc.plane[2].fd"};

/* A plane's modifier as a layout the device reads, or false. */
static bool known_modifier(uint64_t modifier, bool *tiled, enum VmafxSyclTiling *tiling)
{
    *tiled = modifier != VMAFX_MOD_LINEAR;
    *tiling = modifier == VMAFX_MOD_INTEL_4_TILED ? VMAFX_SYCL_TILING_4 : VMAFX_SYCL_TILING_Y;
    return modifier == VMAFX_MOD_LINEAR || modifier == VMAFX_MOD_INTEL_Y_TILED ||
           modifier == VMAFX_MOD_INTEL_4_TILED;
}

/* Bytes from the plane's offset its rows reach: linear rows, or whole tile
 * rows of a tiled plane. */
static uint64_t plane_extent(const VmafxImportPlane *p, bool tiled, uint64_t row, uint64_t rows)
{
    if (!tiled) {
        return (rows - 1u) * p->pitch + row;
    }
    const uint64_t tile_rows = (rows + VMAFX_TILE_ROWS - 1u) / VMAFX_TILE_ROWS;
    return tile_rows * VMAFX_TILE_ROWS * p->pitch;
}

/* Descriptor, size, modifier, pitch and extent of dma-buf plane `i`. */
static VmafxStatus check_dmabuf_plane(const VmafxReport *report, const VmafxImportPlane *p,
                                      uint32_t i, uint64_t row, uint64_t rows, bool *tiled,
                                      enum VmafxSyclTiling *tiling)
{
    if (p->fd < 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE, plane_fd_names[i],
                          "plane %u has no dma-buf descriptor", (unsigned)i);
    }
    if (p->size == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "size"),
                          "plane %u: a DMABUF import needs the size of the dma-buf", (unsigned)i);
    }
    if (!known_modifier(p->modifier, tiled, tiling)) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "modifier"),
                          "plane %u: modifier 0x%llx; the SYCL device reads linear (0), Intel "
                          "Y-tiled (0x%llx) and Tile4 (0x%llx) dma-bufs and never de-tiles "
                          "through a copy on the host",
                          (unsigned)i, (unsigned long long)p->modifier, VMAFX_MOD_INTEL_Y_TILED,
                          VMAFX_MOD_INTEL_4_TILED);
    }
    if (p->pitch < row || (*tiled && p->pitch % VMAFX_TILE_WIDTH != 0u)) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "pitch"),
                          "plane %u: pitch %llu cannot hold a row of %llu bytes%s", (unsigned)i,
                          (unsigned long long)p->pitch, (unsigned long long)row,
                          *tiled ? " in 128-byte tiles" : "");
    }
    const uint64_t extent = plane_extent(p, *tiled, row, rows);
    if (p->offset > p->size || extent > p->size - p->offset) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "size"),
                          "plane %u: %llu bytes from offset %llu do not fit in the %llu-byte "
                          "dma-buf",
                          (unsigned)i, (unsigned long long)extent, (unsigned long long)p->offset,
                          (unsigned long long)p->size);
    }
    return VMAFX_OK;
}

/* The dma-buf's writers have finished (its implicit write fences are
 * signalled, waited on for at most `wait_ns`), or VMAFX_E_BUSY. A kernel
 * without the export ioctl, or a dma-buf that has no fences, is passed. */
static VmafxStatus check_implicit_fence(const VmafxReport *report, int fd, uint32_t i,
                                        uint64_t wait_ns)
{
    const int sync_file = vmafx_dmabuf_export_read_fence(fd);
    if (sync_file < 0) {
        return VMAFX_OK;
    }
    const int state = vmafx_sync_file_wait(sync_file, wait_ns);
    (void)close(sync_file);
    if (state == 1) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_BUSY, state, VMAFX_SUBJECT_PLANE, plane_fd_names[i],
                      "plane %u: the dma-buf's implicit write fence is pending "
                      "(vmafx_context_import_frame() waits on it and retries once)",
                      (unsigned)i);
}

/* Duplicate and import plane `i`'s dma-buf, unless an earlier plane named
 * the same descriptor (then its import is shared). */
static VmafxStatus import_plane(const VmafxReport *report, VmafxSyclFrame *sf,
                                const VmafxFrameImport *d, uint32_t i, uint8_t **base)
{
    const VmafxImportPlane *const p = &d->plane[i];
    for (uint32_t j = 0; j < i; j++) {
        if (d->plane[j].fd == p->fd && sf->imports[j]) {
            *base = sf->imports[j];
            return VMAFX_OK;
        }
    }
    sf->fds[i] = fcntl(p->fd, F_DUPFD_CLOEXEC, 0);
    if (sf->fds[i] < 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, -errno, VMAFX_SUBJECT_PLANE, plane_fd_names[i],
                          "plane %u: cannot duplicate dma-buf descriptor %d", (unsigned)i,
                          (int)p->fd);
    }
    void *ptr = NULL;
    const int err = vmafx_sycl_rt_dmabuf_import(sf->dev->rt, sf->fds[i], (size_t)p->size, &ptr);
    if (err) {
        return VMAFX_FAIL(report, VMAFX_E_DEVICE, err, VMAFX_SUBJECT_PLANE, plane_fd_names[i],
                          "plane %u: Level Zero cannot import the %llu-byte dma-buf (%d); is it "
                          "memory this GPU can reach?",
                          (unsigned)i, (unsigned long long)p->size, err);
    }
    sf->imports[i] = ptr;
    *base = ptr;
    return VMAFX_OK;
}

VmafxStatus vmafx_sycl_dmabuf_planes(const VmafxReport *report, VmafxSyclFrame *sf,
                                     const VmafxFrameImport *d, const VmafxImportLayout *layout,
                                     uint64_t implicit_wait_ns, VmafxSyclSource src[3])
{
    unsigned pw[3];
    unsigned ph[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, pw, ph);
    for (uint32_t i = 0; i < 3u; i++) {
        src[i] = (VmafxSyclSource){
            .base = NULL, .offset = 0u, .pitch = 0u, .tiled = false, .tiling = VMAFX_SYCL_TILING_Y};
    }
    for (uint32_t i = 0; i < layout->n_planes; i++) {
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(layout, d->bpc, i, pw, ph, &row, &rows);
        VmafxSyclSource *const s = &src[i];
        VmafxStatus status =
            check_dmabuf_plane(report, &d->plane[i], i, row, rows, &s->tiled, &s->tiling);
        if (status == VMAFX_OK) {
            status = check_implicit_fence(report, d->plane[i].fd, i, implicit_wait_ns);
        }
        if (status == VMAFX_OK) {
            status = import_plane(report, sf, d, i, &s->base);
        }
        if (status != VMAFX_OK) {
            return status;
        }
        s->offset = d->plane[i].offset;
        s->pitch = d->plane[i].pitch;
    }
    return VMAFX_OK;
}

#else /* !__linux__ */

VmafxStatus vmafx_sycl_dmabuf_planes(const VmafxReport *report, VmafxSyclFrame *sf,
                                     const VmafxFrameImport *d, const VmafxImportLayout *layout,
                                     uint64_t implicit_wait_ns, VmafxSyclSource src[3])
{
    (void)sf;
    (void)d;
    (void)implicit_wait_ns;
    (void)src;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                      "backend sycl, pixel format %s: dma-bufs are Linux objects", layout->name);
}

#endif /* __linux__ */

/* NOLINTEND(modernize-use-nullptr) */
