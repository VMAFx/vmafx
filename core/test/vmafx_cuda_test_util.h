/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Helpers of the white-box tests of the VMAFx CUDA lane (RC4 WP3, ADR-2023):
 * the test plays the producer. It holds device 0's primary context (the
 * context a VMAFx device created by index uses too), a producer stream, and
 * frames it uploads into device memory of its own, laid out planar or
 * semi-planar with a pitch of its choosing, to import them.
 */

#ifndef VMAFX_CUDA_TEST_UTIL_H
#define VMAFX_CUDA_TEST_UTIL_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cuda/vmafx_cuda_internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_import_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr`. ADR-1138. */

typedef struct VcGpu {
    CudaFunctions *f;
    CUdevice dev;
    CUcontext ctx;     /* device 0's primary context, retained by the test */
    CUstream producer; /* the producer's stream */
    VmafxDevice *device;
} VcGpu;

/* Device 0, its primary context, a producer stream and a VMAFx device by
 * index 0; false (nothing held) without a CUDA device. */
static inline bool vc_open(VcGpu *g)
{
    memset(g, 0, sizeof(*g));
    const VmafxCudaDriver *const drv = vmafx_cuda_driver();
    if (!drv) {
        return false;
    }
    g->f = drv->f;
    int n = 0;
    if (g->f->cuDeviceGetCount(&n) != CUDA_SUCCESS || n < 1 ||
        g->f->cuDeviceGet(&g->dev, 0) != CUDA_SUCCESS ||
        g->f->cuDevicePrimaryCtxRetain(&g->ctx, g->dev) != CUDA_SUCCESS) {
        return false;
    }
    CUcontext popped = NULL;
    const bool stream = g->f->cuCtxPushCurrent(g->ctx) == CUDA_SUCCESS &&
                        g->f->cuStreamCreate(&g->producer, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS;
    (void)g->f->cuCtxPopCurrent(&popped);
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_CUDA;
    desc.index = 0;
    return stream && vmafx_device_create(&desc, &g->device, NULL) == VMAFX_OK;
}

static inline void vc_close(VcGpu *g)
{
    vmafx_device_unref(g->device);
    CUcontext popped = NULL;
    if (g->ctx && g->f->cuCtxPushCurrent(g->ctx) == CUDA_SUCCESS) {
        if (g->producer) {
            (void)g->f->cuStreamSynchronize(g->producer);
            (void)g->f->cuStreamDestroy(g->producer);
        }
        (void)g->f->cuCtxPopCurrent(&popped);
    }
    if (g->ctx) {
        (void)g->f->cuDevicePrimaryCtxRelease(g->dev);
    }
    memset(g, 0, sizeof(*g));
}

static inline bool vc_push(const VcGpu *g)
{
    return g->f->cuCtxPushCurrent(g->ctx) == CUDA_SUCCESS;
}

static inline void vc_pop(const VcGpu *g)
{
    CUcontext popped = NULL;
    (void)g->f->cuCtxPopCurrent(&popped);
}

/* A frame in device memory of the test: plane i at base + offset[i]. */
typedef struct VcPlanes {
    CUdeviceptr base;
    uint64_t offset[3];
    uint64_t pitch[3];
    uint32_t n;
} VcPlanes;

/* The layout of vt_device_layout() in `p` (rows and bytes per row of each
 * producer plane). */
static inline void vc_layout(const VmafxFrameDesc *d, uint32_t pix_fmt, size_t pad, size_t skew,
                             VcPlanes *p, size_t rows[3], size_t row_bytes[3])
{
    VtDeviceLayout layout;
    (void)vt_device_layout(d, pix_fmt, pad, skew, &layout, rows, row_bytes);
    memcpy(p->offset, layout.offset, sizeof(p->offset));
    memcpy(p->pitch, layout.pitch, sizeof(p->pitch));
    p->n = layout.n;
}

/* Upload the tightly packed planar frame `planar` into device memory of the
 * test laid out as `pix_fmt` (samples shifted left by `shift` for P010), on
 * the producer stream; the caller frees `p` with vc_free(). */
static inline bool vc_upload_skewed(const VcGpu *g, const VmafxFrameDesc *d, const uint8_t *planar,
                                    uint32_t pix_fmt, unsigned shift, size_t pad, size_t skew,
                                    VcPlanes *p)
{
    size_t rows[3];
    size_t row_bytes[3];
    vc_layout(d, pix_fmt, pad, skew, p, rows, row_bytes);
    assert(p->n >= 2u && p->n <= 3u && vt_frame_bytes(d) > 0u);
    const size_t bytes =
        (size_t)(p->offset[p->n - 1u] + p->pitch[p->n - 1u] * rows[p->n - 1u]) + 64u;
    uint8_t *const staged =
        pix_fmt == VMAFX_PIXEL_FORMAT_YUV420P ? NULL : malloc(vt_frame_bytes(d));
    const uint8_t *src = planar;
    if (staged) {
        vt_to_semiplanar(d, planar, shift, staged);
        src = staged;
    }
    bool ok = vc_push(g) && g->f->cuMemAlloc(&p->base, bytes) == CUDA_SUCCESS;
    for (uint32_t i = 0; i < p->n && i < 3u && ok; i++) {
        CUDA_MEMCPY2D m = {.srcMemoryType = CU_MEMORYTYPE_HOST,
                           .srcHost = src,
                           .srcPitch = row_bytes[i],
                           .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                           .dstDevice = p->base + p->offset[i],
                           .dstPitch = p->pitch[i],
                           .WidthInBytes = row_bytes[i],
                           .Height = rows[i]};
        ok = g->f->cuMemcpy2DAsync(&m, g->producer) == CUDA_SUCCESS;
        src += row_bytes[i] * rows[i];
    }
    vc_pop(g);
    free(staged);
    return ok;
}

static inline bool vc_upload(const VcGpu *g, const VmafxFrameDesc *d, const uint8_t *planar,
                             uint32_t pix_fmt, unsigned shift, size_t pad, VcPlanes *p)
{
    return vc_upload_skewed(g, d, planar, pix_fmt, shift, pad, 0u, p);
}

static inline void vc_free(const VcGpu *g, VcPlanes *p)
{
    if (p->base && vc_push(g)) {
        (void)g->f->cuStreamSynchronize(g->producer);
        (void)g->f->cuMemFree(p->base);
        vc_pop(g);
    }
    memset(p, 0, sizeof(*p));
}

/* The import descriptor of device pointer memory over `p`. */
static inline VmafxFrameImport vc_import_desc(const VmafxFrameDesc *d, uint32_t pix_fmt,
                                              uint32_t bpc, const VcPlanes *p)
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

/* The CUDA event a CUDA_EVENT fence holds. */
static inline CUevent vc_event(const VmafxFence *fence)
{
    const union {
        uintptr_t handle;
        CUevent event;
    } u = {.handle = fence->handle};
    return u.event;
}

/* A CUDA event recorded on the producer stream (an acquire fence). */
static inline VmafxFence vc_record(const VcGpu *g, CUevent *event)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    if (vc_push(g)) {
        if (g->f->cuEventCreate(event, CU_EVENT_DISABLE_TIMING) == CUDA_SUCCESS &&
            g->f->cuEventRecord(*event, g->producer) == CUDA_SUCCESS) {
            fence.kind = VMAFX_FENCE_CUDA_EVENT;
            fence.handle = (uintptr_t)*event;
        }
        vc_pop(g);
    }
    return fence;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_CUDA_TEST_UTIL_H */
