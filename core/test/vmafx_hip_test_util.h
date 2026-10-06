/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Helpers of the white-box tests of the VMAFx HIP lane (RC4 WP3, ADR-2092):
 * the test plays the producer. It holds HIP device 0, a producer stream, and
 * frames it uploads into device memory of its own, laid out planar or
 * semi-planar with a pitch of its choosing, to import them. With libgbm it
 * also produces Linux dma-bufs: linear buffer objects on the HIP device's
 * render node, written through the GBM mapping, which the driver lands with
 * a GPU copy behind a kernel fence (exported as a sync_file).
 */

#ifndef VMAFX_HIP_TEST_UTIL_H
#define VMAFX_HIP_TEST_UTIL_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "hip/hip_handle.h"
#include "vmafx/vmafx.h"
#include "vmafx_import_test_util.h"
#include "vmafx_test_util.h"

#ifdef VMAFX_TEST_HAVE_GBM
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr`. ADR-1138. */

typedef struct VhGpu {
    hipStream_t producer; /* the producer's stream */
    VmafxDevice *device;  /* VMAFx device 0 */
    char pci_bus_id[32];  /* "dddd:bb:dd.f" */
} VhGpu;

/* HIP device 0, a producer stream and a VMAFx device by index 0; false
 * (nothing held) without a HIP device. */
static inline bool vh_open(VhGpu *g)
{
    memset(g, 0, sizeof(*g));
    int n = 0;
    if (hipGetDeviceCount(&n) != hipSuccess || n < 1 || hipSetDevice(0) != hipSuccess ||
        hipStreamCreateWithFlags(&g->producer, hipStreamNonBlocking) != hipSuccess) {
        return false;
    }
    if (hipDeviceGetPCIBusId(g->pci_bus_id, (int)sizeof(g->pci_bus_id), 0) != hipSuccess) {
        g->pci_bus_id[0] = '\0';
    }
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_HIP;
    desc.index = 0;
    return vmafx_device_create(&desc, &g->device, NULL) == VMAFX_OK;
}

static inline void vh_close(VhGpu *g)
{
    vmafx_device_unref(g->device);
    if (g->producer) {
        (void)hipStreamSynchronize(g->producer);
        (void)hipStreamDestroy(g->producer);
    }
    memset(g, 0, sizeof(*g));
}

/* A frame in device memory of the test: plane i at base + layout.offset[i]. */
typedef struct VhPlanes {
    uint8_t *base;
    VtDeviceLayout layout;
} VhPlanes;

/* Upload the tightly packed planar frame `planar` into device memory of the
 * test laid out as `pix_fmt` (samples shifted left by `shift` for P010), on
 * the producer stream (vt_device_layout(): `pad` bytes after each row, each
 * plane `skew` bytes past an aligned start); free `p` with vh_free(). */
static inline bool vh_upload_skewed(const VhGpu *g, const VmafxFrameDesc *d, const uint8_t *planar,
                                    uint32_t pix_fmt, unsigned shift, size_t pad, size_t skew,
                                    VhPlanes *p)
{
    size_t rows[3];
    size_t row_bytes[3];
    const size_t bytes = vt_device_layout(d, pix_fmt, pad, skew, &p->layout, rows, row_bytes);
    uint8_t *staged = NULL;
    const uint8_t *src = vt_producer_bytes(d, planar, pix_fmt, shift, &staged);
    bool ok = src && hipMalloc((void **)&p->base, bytes) == hipSuccess;
    for (uint32_t i = 0; i < p->layout.n && i < 3u && ok; i++) {
        ok = hipMemcpy2DAsync(p->base + p->layout.offset[i], p->layout.pitch[i], src, row_bytes[i],
                              row_bytes[i], rows[i], hipMemcpyHostToDevice,
                              g->producer) == hipSuccess;
        src += row_bytes[i] * rows[i];
    }
    /* The staged bytes are read by the copies: drain before freeing them. */
    ok = ok && hipStreamSynchronize(g->producer) == hipSuccess;
    free(staged);
    return ok;
}

static inline bool vh_upload(const VhGpu *g, const VmafxFrameDesc *d, const uint8_t *planar,
                             uint32_t pix_fmt, unsigned shift, size_t pad, VhPlanes *p)
{
    return vh_upload_skewed(g, d, planar, pix_fmt, shift, pad, 0u, p);
}

static inline void vh_free(const VhGpu *g, VhPlanes *p)
{
    if (p->base) {
        (void)hipStreamSynchronize(g->producer);
        (void)hipFree(p->base);
    }
    memset(p, 0, sizeof(*p));
}

/* The import descriptor of device pointer memory over `p`. */
static inline VmafxFrameImport vh_import_desc(const VmafxFrameDesc *d, uint32_t pix_fmt,
                                              uint32_t bpc, const VhPlanes *p)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_DEVICE_POINTER;
    imp.pix_fmt = pix_fmt;
    imp.bpc = bpc;
    imp.w = d->w;
    imp.h = d->h;
    imp.n_planes = p->layout.n;
    assert(p->layout.n <= 3u);
    for (uint32_t i = 0; i < p->layout.n && i < 3u; i++) {
        imp.plane[i].handle = (uintptr_t)p->base;
        imp.plane[i].offset = p->layout.offset[i];
        imp.plane[i].pitch = p->layout.pitch[i];
    }
    return imp;
}

/* The HIP event a HIP_EVENT fence holds. */
static inline hipEvent_t vh_event(const VmafxFence *fence)
{
    return vmaf_hip_event_of(fence->handle);
}

/* A HIP event recorded on the producer stream (an acquire fence). */
static inline VmafxFence vh_record(const VhGpu *g, hipEvent_t *event)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    if (hipEventCreateWithFlags(event, hipEventDisableTiming) == hipSuccess &&
        hipEventRecord(*event, g->producer) == hipSuccess) {
        fence.kind = VMAFX_FENCE_HIP_EVENT;
        fence.handle = vmaf_hip_event_bits(*event);
    }
    return fence;
}

#ifdef VMAFX_TEST_HAVE_GBM

/* The GBM device of the HIP device's render node. */
typedef struct VhGbm {
    int drm;
    struct gbm_device *gbm;
    /* Bytes each buffer object of vh_dmabuf_write() holds at least: a larger
     * object takes the driver's copy at unmap longer to land (the fence
     * tests' slow producer); 0: the frame's bytes. */
    size_t min_bytes;
} VhGbm;

static inline bool vh_gbm_open(const VhGpu *g, VhGbm *m)
{
    memset(m, 0, sizeof(*m));
    m->drm = -1;
    char path[96];
    (void)snprintf(path, sizeof(path), "/dev/dri/by-path/pci-%s-render", g->pci_bus_id);
    m->drm = g->pci_bus_id[0] ? open(path, O_RDWR | O_CLOEXEC) : -1;
    m->gbm = m->drm >= 0 ? gbm_create_device(m->drm) : NULL;
    return m->gbm != NULL;
}

static inline void vh_gbm_close(VhGbm *m)
{
    if (m->gbm) {
        gbm_device_destroy(m->gbm);
    }
    if (m->drm >= 0) {
        (void)close(m->drm);
    }
    memset(m, 0, sizeof(*m));
    m->drm = -1;
}

/* A frame in a linear dma-buf: one buffer object used as `bytes` linear
 * bytes (rows of VH_DMABUF_ROW bytes), planes at layout.offset. */
#define VH_DMABUF_ROW 4096u

typedef struct VhDmabuf {
    struct gbm_bo *bo;
    int fd;
    uint64_t size;
    VtDeviceLayout layout;
} VhDmabuf;

/* Write the frame into a new linear buffer object through its mapping. The
 * driver lands the write with a GPU copy at unmap: the dma-buf's fences
 * (vh_dmabuf_sync_file()) order it. */
static inline bool vh_dmabuf_write(const VhGbm *m, const VmafxFrameDesc *d, const uint8_t *planar,
                                   uint32_t pix_fmt, unsigned shift, size_t pad, VhDmabuf *b)
{
    memset(b, 0, sizeof(*b));
    b->fd = -1;
    size_t rows[3];
    size_t row_bytes[3];
    size_t bytes = vt_device_layout(d, pix_fmt, pad, 0u, &b->layout, rows, row_bytes);
    bytes = bytes > m->min_bytes ? bytes : m->min_bytes;
    const uint32_t height = (uint32_t)((bytes + VH_DMABUF_ROW - 1u) / VH_DMABUF_ROW);
    b->bo = gbm_bo_create(m->gbm, VH_DMABUF_ROW, height, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
    uint32_t stride = 0;
    void *map_data = NULL;
    uint8_t *const map = b->bo ? gbm_bo_map(b->bo, 0, 0, VH_DMABUF_ROW, height,
                                            GBM_BO_TRANSFER_WRITE, &stride, &map_data) :
                                 NULL;
    uint8_t *staged = NULL;
    const uint8_t *src = vt_producer_bytes(d, planar, pix_fmt, shift, &staged);
    /* One row of the mapping per VH_DMABUF_ROW bytes: a linear view. */
    const bool ok = map && src && stride == VH_DMABUF_ROW;
    for (uint32_t i = 0; i < b->layout.n && ok; i++) {
        for (size_t y = 0; y < rows[i]; y++) {
            memcpy(map + b->layout.offset[i] + y * b->layout.pitch[i], src, row_bytes[i]);
            src += row_bytes[i];
        }
    }
    if (map) {
        gbm_bo_unmap(b->bo, map_data);
    }
    free(staged);
    b->fd = ok ? gbm_bo_get_fd(b->bo) : -1;
    b->size = (uint64_t)VH_DMABUF_ROW * height;
    return b->fd >= 0;
}

static inline void vh_dmabuf_free(VhDmabuf *b)
{
    if (b->fd >= 0) {
        (void)close(b->fd);
    }
    if (b->bo) {
        gbm_bo_destroy(b->bo);
    }
    memset(b, 0, sizeof(*b));
    b->fd = -1;
}

/* A sync_file of the dma-buf's pending writes (DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
 * Linux 6.0): the fence a reader waits on; -1 on failure. */
static inline int vh_dmabuf_sync_file(const VhDmabuf *b)
{
    struct dma_buf_export_sync_file ex = {.flags = DMA_BUF_SYNC_READ, .fd = -1};
    return ioctl(b->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &ex) == 0 ? ex.fd : -1;
}

/* The import descriptor of DMABUF memory over `b` (modifier 0: linear). */
static inline VmafxFrameImport vh_dmabuf_desc(const VmafxFrameDesc *d, uint32_t pix_fmt,
                                              uint32_t bpc, const VhDmabuf *b)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_DMABUF;
    imp.pix_fmt = pix_fmt;
    imp.bpc = bpc;
    imp.w = d->w;
    imp.h = d->h;
    imp.n_planes = b->layout.n;
    for (uint32_t i = 0; i < b->layout.n && i < 3u; i++) {
        imp.plane[i].fd = b->fd;
        imp.plane[i].offset = b->layout.offset[i];
        imp.plane[i].pitch = b->layout.pitch[i];
        imp.plane[i].size = b->size;
    }
    return imp;
}

#endif /* VMAFX_TEST_HAVE_GBM */

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_HIP_TEST_UTIL_H */
