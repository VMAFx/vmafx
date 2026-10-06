/**
 *  Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * State the C files of the VMAFx SYCL lane share (import_device.c,
 * import_frame.c, import_fence.c, import_dmabuf.c, import_gl.c,
 * import_pool.c; ADR-2091). The SYCL calls are vmafx_sycl_rt.cpp's.
 */

#ifndef VMAF_SRC_SYCL_VMAFX_SYCL_INTERNAL_H_
#define VMAF_SRC_SYCL_VMAFX_SYCL_INTERNAL_H_

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_sycl_rt.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr`. ADR-1138. */

typedef struct VmafxSyclDevice {
    VmafxSyclRt *rt;         /* the library queue on the device's context */
    atomic_uint copy_logged; /* VMAFX_IMPORT_ALLOW_COPY device copy logged once */
} VmafxSyclDevice;

/* The lane's state of one frame. */
typedef struct VmafxSyclFrame {
    VmafxSyclDevice *dev;
    VmafxSyclFrameRt *rt;  /* ready event and readers; NULL once released */
    void *owned;           /* converted / de-tiled planes (device USM) */
    void *imports[3];      /* Level Zero imports of the planes' dma-bufs */
    int fds[3];            /* the dma-bufs (duplicated), -1: none */
    uint32_t release_slot; /* 1 + the slot of its SYCL_EVENT release fence; 0: none */
    pthread_mutex_t lock;  /* release-fence calls on several threads */
} VmafxSyclFrame;

/* What a pool frame keeps for its life (VmafxFrame.lane_persistent). */
typedef struct VmafxSyclPoolFrame {
    uint32_t idle_slot; /* 1 + the slot its last release recorded its readers in */
} VmafxSyclPoolFrame;

/* One producer plane as the device reads it: linear memory at `base` +
 * `offset` with `pitch` bytes per row, or a plane in Intel tiles
 * (`tiled`), whose `pitch` is the tiled pitch. */
typedef struct VmafxSyclSource {
    uint8_t *base; /* the engine never writes a frame's planes (ADR-1929) */
    uint64_t offset;
    uint64_t pitch;
    bool tiled;
    enum VmafxSyclTiling tiling;
} VmafxSyclSource;

/* Device of a VmafxDevice of the SYCL backend. */
static inline VmafxSyclDevice *vmafx_sycl_dev(const VmafxDevice *device)
{
    return (VmafxSyclDevice *)device->lane;
}

/* A new lane state of a frame on `dev` (its runtime frame made), or NULL. */
VmafxSyclFrame *vmafx_sycl_frame_state_new(VmafxSyclDevice *dev);
/* Free the lane state of an import that failed: drain what it enqueued and
 * free its memory, imports and descriptors. */
void vmafx_sycl_frame_state_discard(VmafxSyclFrame *sf);
/* The lane's release of a frame (VmafxFrame.lane_release). */
int vmafx_sycl_frame_release(VmafxFrame *frame, VmafPicture *pic);
/* Make `pic` a SYCL device picture of the frame (its private part names the
 * lane's runtime frame, which the engine's readers read through). */
void vmafx_sycl_picture_attach(VmafPicture *pic, VmafxSyclFrame *sf);

/* The idle slot of a pool frame (import_pool.c), or 0 for any other frame. */
uint32_t vmafx_sycl_pool_idle_slot(const VmafxFrame *frame);

/* Linux dma-buf imports (import_dmabuf.c): check the planes of a DMABUF
 * descriptor, duplicate and import their dma-bufs into `sf`, and resolve
 * each producer plane; the dma-bufs' implicit write fences are waited on for
 * at most `implicit_wait_ns` (VMAFX_E_BUSY while still pending). */
VmafxStatus vmafx_sycl_dmabuf_planes(const VmafxReport *report, VmafxSyclFrame *sf,
                                     const VmafxFrameImport *d, const VmafxImportLayout *layout,
                                     uint64_t implicit_wait_ns, VmafxSyclSource src[3]);

/* OpenGL textures (import_gl.c): export the GL texture of each plane of a
 * GL_TEXTURE descriptor as a dma-buf through EGL and rewrite `*d` into the
 * equivalent DMABUF descriptor; the exported descriptors are stored in
 * `exported` (-1: none) for the caller to close once imported. */
VmafxStatus vmafx_sycl_gl_export(const VmafxReport *report, VmafxSyclFrame *sf, VmafxFrameImport *d,
                                 int exported[3]);

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAF_SRC_SYCL_VMAFX_SYCL_INTERNAL_H_ */
