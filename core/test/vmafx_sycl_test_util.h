/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Helpers of the VMAFx SYCL lane tests (RC4 WP3, ADR-2091): a producer
 * (vmafx_sycl_producer.h) and a VMAFx SYCL device on the same Level Zero
 * GPU and context, frames uploaded by the producer into USM laid out as a
 * decoder would (planar or semi-planar, row padding, an odd start), and the
 * import descriptors over them.
 */

#ifndef VMAFX_SYCL_TEST_UTIL_H
#define VMAFX_SYCL_TEST_UTIL_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vmafx/vmafx.h"
#include "vmafx_import_test_util.h"
#include "vmafx_sycl_producer.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr`. ADR-1138. */

typedef struct VsGpu {
    VsProducer *producer;
    VmafxDevice *device;
} VsGpu;

/* The producer on GPU 0 and a VMAFx device on the same context: by index 0
 * (the device's default context), or from the producer's queue
 * (`external`). False, nothing held, without a Level Zero GPU. */
static inline bool vs_open_gpu(VsGpu *g, bool external)
{
    memset(g, 0, sizeof(*g));
    g->producer = vs_producer_open();
    if (!g->producer) {
        return false;
    }
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_SYCL;
    desc.index = 0;
    desc.external[0] = external ? vs_producer_queue(g->producer) : 0u;
    if (vmafx_device_create(&desc, &g->device, NULL) != VMAFX_OK) {
        vs_producer_close(g->producer);
        g->producer = NULL;
        return false;
    }
    return true;
}

static inline void vs_close_gpu(VsGpu *g)
{
    vmafx_device_unref(g->device);
    vs_producer_close(g->producer);
    memset(g, 0, sizeof(*g));
}

/* A frame in USM of the producer: plane i at base + offset[i]. */
typedef struct VsPlanes {
    uint8_t *base;
    uint64_t offset[3];
    uint64_t pitch[3];
    uint32_t n;
} VsPlanes;

/* Bytes of the rows of each producer plane of `d` laid out as `pix_fmt`
 * (planar, semi-planar or packed: vt_device_layout()), with `pad` bytes after
 * each row and each plane starting `skew` bytes after a 64-byte boundary:
 * the SYCL device reads any address and pitch (its readers copy rows). */
static inline void vs_layout(const VmafxFrameDesc *d, uint32_t pix_fmt, size_t pad, size_t skew,
                             VsPlanes *p, size_t rows[3], size_t row_bytes[3])
{
    VtDeviceLayout layout;
    (void)vt_device_layout(d, pix_fmt, pad, skew, &layout, rows, row_bytes);
    memcpy(p->offset, layout.offset, sizeof(p->offset));
    memcpy(p->pitch, layout.pitch, sizeof(p->pitch));
    p->n = layout.n;
}

/* Bytes the planes of `p` (laid out by vs_layout()) span, plus a margin. */
static inline size_t vs_span(const VsPlanes *p, const size_t rows[3])
{
    return (size_t)(p->offset[p->n - 1u] + p->pitch[p->n - 1u] * rows[p->n - 1u]) + 64u;
}

/* Enqueue the tightly packed planar frame `planar` into USM at `p->base`
 * laid out by vs_layout() as `pix_fmt` (P010 samples shifted left by `shift`). */
static inline bool vs_write(const VsGpu *g, const VmafxFrameDesc *d, const uint8_t *planar,
                            uint32_t pix_fmt, unsigned shift, const VsPlanes *p,
                            const size_t rows[3], const size_t row_bytes[3])
{
    uint8_t *staged = NULL;
    const uint8_t *src = vt_producer_bytes(d, planar, pix_fmt, shift, &staged);
    bool ok = src != NULL;
    for (uint32_t i = 0; i < p->n && i < 3u && ok; i++) {
        ok = vs_copy_2d(g->producer, p->base + p->offset[i], (size_t)p->pitch[i], src, row_bytes[i],
                        row_bytes[i], rows[i]) == 0;
        src += row_bytes[i] * rows[i];
    }
    /* The copies read `staged` on the host side of the queue: done first. */
    ok = vs_finish(g->producer) == 0 && ok;
    free(staged);
    return ok;
}

/* Upload a planar frame into fresh device USM laid out as `pix_fmt`. */
static inline bool vs_upload(const VsGpu *g, const VmafxFrameDesc *d, const uint8_t *planar,
                             uint32_t pix_fmt, unsigned shift, size_t pad, size_t skew, VsPlanes *p)
{
    size_t rows[3];
    size_t row_bytes[3];
    vs_layout(d, pix_fmt, pad, skew, p, rows, row_bytes);
    assert(p->n >= 1u && p->n <= 3u && vt_frame_bytes(d) > 0u);
    p->base = vs_alloc(g->producer, vs_span(p, rows));
    return p->base && vs_write(g, d, planar, pix_fmt, shift, p, rows, row_bytes);
}

static inline void vs_free_planes(const VsGpu *g, VsPlanes *p)
{
    if (p->base) {
        vs_free(g->producer, p->base);
    }
    memset(p, 0, sizeof(*p));
}

/* The import descriptor of USM over `p`. */
static inline VmafxFrameImport vs_import_desc(const VmafxFrameDesc *d, uint32_t pix_fmt,
                                              uint32_t bpc, const VsPlanes *p)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_DEVICE_POINTER;
    imp.pix_fmt = pix_fmt;
    imp.bpc = bpc;
    imp.w = d->w;
    imp.h = d->h;
    imp.n_planes = p->n;
    assert(p->n <= 3u);
    for (uint32_t i = 0; i < p->n && i < 3u; i++) {
        imp.plane[i].handle = (uintptr_t)p->base;
        imp.plane[i].offset = p->offset[i];
        imp.plane[i].pitch = p->pitch[i];
    }
    return imp;
}

/* A SYCL_EVENT fence naming `event` (a sycl::event * the caller keeps). */
static inline VmafxFence vs_event_fence(uintptr_t event)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    fence.kind = event ? VMAFX_FENCE_SYCL_EVENT : VMAFX_FENCE_NONE;
    fence.handle = event;
    return fence;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_SYCL_TEST_UTIL_H */
