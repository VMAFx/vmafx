/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Source layouts of the Metal IOSurface import (ADR-1679).
 *
 *  VideoToolbox decodes to bi-planar surfaces: NV12 ('420v' / '420f', one
 *  luma plane and one plane of interleaved Cb/Cr bytes) and P010 ('x420' /
 *  'xf20', the same with 16-bit samples whose 10 bits sit in the most
 *  significant bits). libvmaf scores planar YUV 4:2:0 with the samples in the
 *  least significant bits. vmaf_metal_picture_import() reads a surface
 *  through this header: plane 0 is the luma plane, planes 1 and 2 are the
 *  even and odd samples of the interleaved plane, and an MSB-aligned sample
 *  is shifted down while it is copied. A layout that is not in the table is
 *  refused (-ENOTSUP), never copied as if it were another one.
 *
 *  Plain C with no Apple header, so core/test/test_metal_iosurface_layout.c
 *  runs the same code on every host. The four-character codes are those of
 *  CoreVideo's CVPixelBuffer.h (kCVPixelFormatType_420YpCbCr8Planar = 'y420',
 *  ..._420YpCbCr10BiPlanarFullRange = 'xf20').
 */

#ifndef LIBVMAF_METAL_IOSURFACE_LAYOUT_H_
#define LIBVMAF_METAL_IOSURFACE_LAYOUT_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../vmafx/import_convert.h"

#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C header included by a C test
 * translation unit; MSVC's C mode has no `nullptr`. ADR-1138. */

#define VMAF_METAL_FOURCC(a, b, c, d)                                                              \
    ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) |             \
                (uint32_t)(d)))

/** One accepted surface layout. Every entry is 4:2:0. */
typedef struct VmafMetalSurfaceFormat {
    uint32_t fourcc;  /**< CoreVideo pixel format type of the IOSurface */
    unsigned planes;  /**< 3: planar Y, Cb, Cr; 2: Y, then interleaved Cb/Cr */
    unsigned bpc;     /**< bits per sample after the shift */
    unsigned shift;   /**< right shift that brings an MSB-aligned sample to bit 0 */
    const char *name; /**< FFmpeg's name of the layout, for messages */
} VmafMetalSurfaceFormat;

/** How one VmafPicture plane is read from the surface. */
typedef struct VmafMetalPlaneRead {
    unsigned src_plane; /**< IOSurface plane to read */
    unsigned step;      /**< samples per element: 1 planar, 2 interleaved */
    unsigned offset;    /**< sample within the element: 0 = Cb, 1 = Cr */
    unsigned bytes;     /**< bytes per sample: 1 or 2 */
    unsigned shift;     /**< right shift per sample */
} VmafMetalPlaneRead;

/** Geometry of one surface plane as IOSurface reports it. */
typedef struct VmafMetalSurfacePlane {
    size_t width;             /**< elements per row */
    size_t height;            /**< rows */
    size_t bytes_per_element; /**< IOSurfaceGetBytesPerElementOfPlane() */
    size_t bytes_per_row;     /**< IOSurfaceGetBytesPerRowOfPlane() */
} VmafMetalSurfacePlane;

/** The accepted layout of a CoreVideo pixel format type, or NULL. */
static inline const VmafMetalSurfaceFormat *vmaf_metal_surface_format(uint32_t fourcc)
{
    static const VmafMetalSurfaceFormat formats[] = {
        {VMAF_METAL_FOURCC('y', '4', '2', '0'), 3u, 8u, 0u, "yuv420p"},
        {VMAF_METAL_FOURCC('f', '4', '2', '0'), 3u, 8u, 0u, "yuv420p"},
        {VMAF_METAL_FOURCC('4', '2', '0', 'v'), 2u, 8u, 0u, "nv12"},
        {VMAF_METAL_FOURCC('4', '2', '0', 'f'), 2u, 8u, 0u, "nv12"},
        {VMAF_METAL_FOURCC('x', '4', '2', '0'), 2u, 10u, 6u, "p010"},
        {VMAF_METAL_FOURCC('x', 'f', '2', '0'), 2u, 10u, 6u, "p010"},
    };
    for (size_t i = 0u; i < sizeof(formats) / sizeof(formats[0]); i++) {
        if (formats[i].fourcc == fourcc) {
            return &formats[i];
        }
    }
    return NULL;
}

/** The surface plane that holds VmafPicture plane `plane` (0 Y, 1 Cb, 2 Cr). */
static inline unsigned vmaf_metal_surface_src_plane(const VmafMetalSurfaceFormat *fmt,
                                                    unsigned plane)
{
    return (fmt->planes == 2u && plane > 0u) ? 1u : plane;
}

/**
 * Plan the read of VmafPicture plane `plane` (dst_w x dst_h samples at `bpc`
 * bits) from a surface of layout `fmt` with `plane_count` planes, whose
 * source plane has the geometry `src`. -EINVAL when the surface does not
 * have the layout's planes, the caller's bit depth is not the layout's, or
 * the source plane is smaller than the picture plane.
 */
static inline int vmaf_metal_plane_read_plan(const VmafMetalSurfaceFormat *fmt, size_t plane_count,
                                             unsigned plane, unsigned bpc,
                                             const VmafMetalSurfacePlane *src, unsigned dst_w,
                                             unsigned dst_h, VmafMetalPlaneRead *out)
{
    if (fmt == NULL || src == NULL || out == NULL || plane > 2u) {
        return -EINVAL;
    }
    if (plane_count != (size_t)fmt->planes || bpc != fmt->bpc) {
        return -EINVAL;
    }
    const int pair = (fmt->planes == 2u && plane > 0u);
    out->src_plane = vmaf_metal_surface_src_plane(fmt, plane);
    out->step = pair ? 2u : 1u;
    out->offset = pair ? plane - 1u : 0u;
    out->bytes = (bpc > 8u) ? 2u : 1u;
    out->shift = fmt->shift;
    const size_t element = (size_t)out->step * out->bytes;
    if (src->bytes_per_element != element) {
        return -EINVAL;
    }
    if (src->width < (size_t)dst_w || src->height < (size_t)dst_h) {
        return -EINVAL;
    }
    if (src->bytes_per_row < (size_t)dst_w * element) {
        return -EINVAL;
    }
    return 0;
}

/** Copy w x h samples of a planned read from `src` into a planar plane: the
 *  VMAFx import conversions' one CPU reference (ADR-2133). */
static inline void vmaf_metal_read_plane(uint8_t *dst, size_t dst_stride, const uint8_t *src,
                                         size_t src_stride, unsigned w, unsigned h,
                                         const VmafMetalPlaneRead *rd)
{
    const VmafxImportRead read = {.src_plane = rd->src_plane,
                                  .step = rd->step,
                                  .offset = rd->offset,
                                  .in_bytes = rd->bytes,
                                  .shift = rd->shift,
                                  .mask = 0u};
    vmafx_import_read_plane(dst, dst_stride, rd->bytes, src, src_stride, w, h, &read);
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* LIBVMAF_METAL_IOSURFACE_LAYOUT_H_ */
