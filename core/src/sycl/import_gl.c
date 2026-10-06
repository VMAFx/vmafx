/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * OpenGL interop of the SYCL lane (RC4 WP3, #2238, ADR-2091): a frame held in
 * GL 2D textures (one per plane; NV12 as an R8 and an RG8 texture) imported on
 * a SYCL device through EGL's dma-buf export (EGL_KHR_gl_texture_2D_image +
 * EGL_MESA_image_dma_buf_export): each texture becomes an EGL image of the
 * EGL context current on the importing thread, the image is exported as a
 * dma-buf with its pitch, offset and modifier, and the frame is imported as a
 * DMABUF frame (import_dmabuf.c: bound when linear, de-tiled on the device
 * when Intel-tiled). The exported descriptors are the frame's; the EGL images
 * are destroyed once exported.
 *
 * The export is the library's one EGL export (vmafx/egl_export.c, shared with
 * the HIP lane, HISS-19) in its keep mode: a tiled export is returned as the
 * driver made it, with its modifier, for this lane to de-tile.
 *
 * The acquire fence of a GL import is a GL sync object, checked on the host
 * (vmafx/sync_object.c, shared with the CUDA and HIP lanes): signalled, or
 * VMAFX_E_BUSY for the D8 retry. The dma-buf's implicit write fences, which
 * the GL driver sets for shared buffers, are honoured as for any dma-buf.
 * EGL entry points are resolved at run time; no EGL header or library is a
 * build dependency.
 */

#include <stdbool.h>
#include <stdint.h>

#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_sycl.h"
#include "vmafx_sycl_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

VmafxStatus vmafx_sycl_gl_export(const VmafxReport *report, VmafxSyclFrame *sf, VmafxFrameImport *d,
                                 int exported[3])
{
    (void)sf;
    const uint32_t n = d->n_planes < 3u ? d->n_planes : 3u;
    if (n == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.n_planes",
                          "backend sycl, memory GL_TEXTURE: no plane to export");
    }
    VmafxEglTarget targets[3];
    for (uint32_t i = 0; i < n; i++) {
        /* fourcc 0: the plane's format is the dma-buf import's to check; no
         * size: the keep mode never copies. */
        targets[i] = (VmafxEglTarget){
            .texture = (uintptr_t)d->plane[i].handle, .fourcc = 0u, .w = 0u, .h = 0u};
    }
    VmafxEglPlane planes[3];
    bool copied = false;
    const VmafxStatus status = vmafx_egl_export_planes(report, "sycl", NULL, targets, n,
                                                       VMAFX_EGL_TILED_KEEP, planes, &copied);
    if (status != VMAFX_OK) {
        return status;
    }
    for (uint32_t i = 0; i < n; i++) {
        exported[i] = planes[i].fd;
        d->plane[i].fd = planes[i].fd;
        d->plane[i].handle = 0u;
        d->plane[i].pitch = planes[i].pitch;
        d->plane[i].offset = planes[i].offset;
        d->plane[i].modifier = planes[i].modifier;
        d->plane[i].size = planes[i].size;
    }
    d->memory = VMAFX_MEMORY_DMABUF;
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
