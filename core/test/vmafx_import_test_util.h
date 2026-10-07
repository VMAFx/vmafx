/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Helpers of the VMAFx device-frame tests (RC4 WP3 common lane): import
 * descriptors over host buffers, and NV12 / P010 / P016 buffers made from the
 * tightly packed planar frames of vmafx_test_util.h.
 */

#ifndef VMAFX_IMPORT_TEST_UTIL_H
#define VMAFX_IMPORT_TEST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr` and the required
 * Windows builds compile the tests with cl.exe (C2065). ADR-1138. */

/* An import descriptor of HOST memory over the tightly packed planar frame
 * `data` of geometry `d`. */
static inline VmafxFrameImport vt_import_planar(const VmafxFrameDesc *d, const uint8_t *data)
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(d, w, h, row);
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_HOST;
    imp.pix_fmt = d->pix_fmt;
    imp.bpc = d->bpc;
    imp.w = d->w;
    imp.h = d->h;
    imp.n_planes = d->pix_fmt == VMAFX_PIXEL_FORMAT_YUV400P ? 1u : 3u;
    for (uint32_t p = 0; p < imp.n_planes; p++) {
        imp.plane[p].handle = (uintptr_t)data;
        imp.plane[p].pitch = row[p];
        data += row[p] * h[p];
    }
    return imp;
}

/* Bytes of the semi-planar form of a 4:2:0 frame `d`. */
static inline size_t vt_semiplanar_bytes(const VmafxFrameDesc *d)
{
    return vt_frame_bytes(d);
}

/* Sample `i` of a plane of `bytes` bytes per sample. */
static inline uint16_t vt_sample(const uint8_t *plane, size_t i, size_t bytes)
{
    uint16_t v = plane[i * bytes];
    if (bytes == 2u) {
        memcpy(&v, plane + i * 2u, sizeof(v));
    }
    return v;
}

static inline void vt_put_sample(uint8_t *plane, size_t i, size_t bytes, uint16_t v)
{
    if (bytes == 2u) {
        memcpy(plane + i * 2u, &v, sizeof(v));
    } else {
        plane[i] = (uint8_t)v;
    }
}

/* Write the semi-planar form of the tightly packed YUV420P frame `planar`
 * (geometry `d`) into `out` (vt_semiplanar_bytes() bytes): the luma plane,
 * then the interleaved Cb / Cr plane, every sample shifted up by `shift`
 * (6 for P010, 0 for NV12 / P016). */
static inline void vt_to_semiplanar(const VmafxFrameDesc *d, const uint8_t *planar, unsigned shift,
                                    uint8_t *out)
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(d, w, h, row);
    const size_t bytes = d->bpc > 8u ? 2u : 1u;
    const size_t n_luma = (size_t)w[0] * h[0];
    const size_t n_chroma = (size_t)w[1] * h[1];
    for (size_t i = 0; i < n_luma; i++) {
        vt_put_sample(out, i, bytes, (uint16_t)(vt_sample(planar, i, bytes) << shift));
    }
    const uint8_t *const cb = planar + n_luma * bytes;
    const uint8_t *const cr = cb + n_chroma * bytes;
    uint8_t *const uv = out + n_luma * bytes;
    for (size_t i = 0; i < n_chroma; i++) {
        vt_put_sample(uv, 2u * i, bytes, (uint16_t)(vt_sample(cb, i, bytes) << shift));
        vt_put_sample(uv, 2u * i + 1u, bytes, (uint16_t)(vt_sample(cr, i, bytes) << shift));
    }
}

/* An import descriptor of HOST memory over the semi-planar frame `data`
 * written by vt_to_semiplanar() for geometry `d` (YUV420P). */
static inline VmafxFrameImport vt_import_semiplanar(const VmafxFrameDesc *d, uint32_t pix_fmt,
                                                    uint32_t bpc, const uint8_t *data)
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(d, w, h, row);
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_HOST;
    imp.pix_fmt = pix_fmt;
    imp.bpc = bpc;
    imp.w = d->w;
    imp.h = d->h;
    imp.n_planes = 2u;
    imp.plane[0].handle = (uintptr_t)data;
    imp.plane[0].pitch = row[0];
    imp.plane[1].handle = (uintptr_t)(data + row[0] * h[0]);
    imp.plane[1].pitch = row[1] * 2u;
    return imp;
}

/* Shift every sample of a tightly packed 16-bit frame up by `shift`. */
static inline void vt_shift_up(const VmafxFrameDesc *d, uint8_t *data, unsigned shift)
{
    const size_t n = vt_frame_bytes(d) / 2u;
    for (size_t i = 0; i < n; i++) {
        vt_put_sample(data, i, 2u, (uint16_t)(vt_sample(data, i, 2u) << shift));
    }
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_IMPORT_TEST_UTIL_H */
