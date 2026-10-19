/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * OpenGL textures on a HIP device (RC4 WP3, #2238, ADR-2132): a frame held in
 * GL 2D textures (one per plane; NV12 as an R8 and an RG8 texture) becomes
 * the dma-bufs of its textures (core/src/vmafx/egl_export.c: EGL's dma-buf
 * export of the current EGL context, a GPU copy into a linear dma-buf where
 * the driver exports a tiled layout) and is imported as a DMABUF frame
 * (import_dmabuf.c). The HIP runtime's own GL interop is not used: the
 * pinned ROCm 10.1 maps a texture but cannot read it, and ROCm 7.2 reads only
 * the current GLX context and crashes after a failed first call
 * (ADR-2092; T-HIP-ROCM10-GL-TEXTURE-READ-2026-10-06, closed by ADR-2132).
 *
 * The GL sync object of the import is the acquire fence, checked on the host
 * before any export (vmafx_gl_sync_acquire(), core/src/vmafx/sync_object.c).
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"
#include "picture.h"
#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/sync_object.h"
#include "vmafx/vmafx.h"
#include "vmafx_hip.h"
#include "vmafx_hip_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* DRM fourccs of the plane formats a GL frame has (drm_fourcc.h). */
#define VMAFX_DRM_R8 0x20203852u
#define VMAFX_DRM_GR88 0x38385247u
#define VMAFX_DRM_R16 0x20363152u
#define VMAFX_DRM_GR1616 0x32335247u

/* The DRM fourcc of plane `i`'s texture: one channel, two for the
 * interleaved chroma of a semi-planar layout; 8 or 16 bits. */
static uint32_t plane_fourcc(const VmafxImportLayout *layout, uint32_t bpc, uint32_t i)
{
    const bool two = layout->interleaved && i > 0u;
    if (bpc > 8u) {
        return two ? VMAFX_DRM_GR1616 : VMAFX_DRM_R16;
    }
    return two ? VMAFX_DRM_GR88 : VMAFX_DRM_R8;
}

VmafxStatus vmafx_hip_gl_export(const VmafxReport *report, const VmafxHipDevice *dev,
                                const VmafxFrameImport *d, const VmafxImportLayout *layout,
                                VmafxFrameImport *out, bool *copied)
{
    assert(d->memory == VMAFX_MEMORY_GL_TEXTURE && layout->n_planes >= 1u &&
           layout->n_planes <= 3u);
    unsigned pw[3];
    unsigned ph[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, pw, ph);
    VmafxEglTarget targets[3];
    for (uint32_t i = 0; i < layout->n_planes; i++) {
        targets[i] = (VmafxEglTarget){.texture = (uintptr_t)d->plane[i].handle,
                                      .fourcc = plane_fourcc(layout, d->bpc, i),
                                      .w = pw[i],
                                      .h = ph[i]};
    }
    VmafxEglPlane planes[3];
    const VmafxStatus status = vmafx_egl_export_planes(
        report, "hip", dev->pci_bus_id, targets, layout->n_planes,
        (d->flags & VMAFX_IMPORT_ALLOW_COPY) != 0u ? VMAFX_EGL_TILED_COPY : VMAFX_EGL_TILED_REFUSE,
        planes, copied);
    if (status != VMAFX_OK) {
        return status;
    }
    *out = *d;
    out->memory = VMAFX_MEMORY_DMABUF;
    out->acquire = (VmafxFence)VMAFX_FENCE_INIT;
    for (uint32_t i = 0; i < layout->n_planes; i++) {
        out->plane[i].handle = 0u;
        out->plane[i].fd = planes[i].fd;
        out->plane[i].offset = planes[i].offset;
        out->plane[i].pitch = planes[i].pitch;
        out->plane[i].modifier = planes[i].modifier;
        out->plane[i].size = planes[i].size;
        out->plane[i].plane_index = 0u;
    }
    return VMAFX_OK;
}

void vmafx_hip_gl_close(VmafxFrameImport *dmabuf, uint32_t n_planes)
{
    VmafxEglPlane planes[3];
    for (uint32_t i = 0; i < n_planes && i < 3u; i++) {
        planes[i].fd = dmabuf->plane[i].fd;
    }
    vmafx_egl_close_planes(planes, n_planes);
}

/* NOLINTEND(modernize-use-nullptr) */
