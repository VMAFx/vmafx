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

/* Import layouts beyond planar (ADR-2133): semi-planar (NV12, NV16, NV24 and
 * the P0xx / P2xx / P4xx family) and packed (Y210, Y410 / XV30). */
static inline bool vt_is_semi(uint32_t pix_fmt)
{
    return pix_fmt >= VMAFX_PIXEL_FORMAT_NV12 && pix_fmt <= VMAFX_PIXEL_FORMAT_P416;
}

static inline bool vt_is_packed(uint32_t pix_fmt)
{
    return pix_fmt == VMAFX_PIXEL_FORMAT_Y210 || pix_fmt == VMAFX_PIXEL_FORMAT_Y410 ||
           pix_fmt == VMAFX_PIXEL_FORMAT_YUYV422 || pix_fmt == VMAFX_PIXEL_FORMAT_Y212 ||
           pix_fmt == VMAFX_PIXEL_FORMAT_VUYX || pix_fmt == VMAFX_PIXEL_FORMAT_XV36;
}

/* Planar words with the sample in the top bits (YUV444P_MSB). */
static inline bool vt_is_msb(uint32_t pix_fmt)
{
    return pix_fmt == VMAFX_PIXEL_FORMAT_YUV444P_MSB;
}

/* The semi-planar import format of the planar format `planar_fmt` at `bpc`
 * (8: NVxx, 10: P0xx / P2xx / P4xx with the samples in the top bits, 16:
 * P0x6); UNKNOWN for another depth or a luma-only frame. */
static inline uint32_t vt_semi_format(uint32_t planar_fmt, uint32_t bpc)
{
    static const uint32_t planar[3] = {VMAFX_PIXEL_FORMAT_YUV420P, VMAFX_PIXEL_FORMAT_YUV422P,
                                       VMAFX_PIXEL_FORMAT_YUV444P};
    static const uint32_t semi[3][3] = {
        {VMAFX_PIXEL_FORMAT_NV12, VMAFX_PIXEL_FORMAT_P010, VMAFX_PIXEL_FORMAT_P016},
        {VMAFX_PIXEL_FORMAT_NV16, VMAFX_PIXEL_FORMAT_P210, VMAFX_PIXEL_FORMAT_P216},
        {VMAFX_PIXEL_FORMAT_NV24, VMAFX_PIXEL_FORMAT_P410, VMAFX_PIXEL_FORMAT_P416}};
    const unsigned depth = bpc == 8u ? 0u : (bpc == 10u ? 1u : (bpc == 16u ? 2u : 3u));
    for (unsigned k = 0; k < 3u && depth < 3u; k++) {
        if (planar[k] == planar_fmt) {
            return semi[k][depth];
        }
    }
    return VMAFX_PIXEL_FORMAT_UNKNOWN;
}

/* The left shift of the samples of a producer frame at `bpc` bits (10: in
 * the top bits of 16). */
static inline unsigned vt_semi_shift(uint32_t bpc)
{
    return bpc == 10u ? 6u : 0u;
}

/* Bytes per row of the packed form of a frame of `d`'s geometry (1 byte per
 * element at 8 bits, else 2; Y410 has 32-bit words). */
static inline size_t vt_packed_row(const VmafxFrameDesc *d, uint32_t pix_fmt)
{
    const size_t bytes = d->bpc > 8u ? 2u : 1u;
    switch (pix_fmt) {
    case VMAFX_PIXEL_FORMAT_Y210:
    case VMAFX_PIXEL_FORMAT_Y212:
    case VMAFX_PIXEL_FORMAT_YUYV422:
        return (size_t)((d->w + 1u) / 2u) * 4u * bytes;
    case VMAFX_PIXEL_FORMAT_Y410:
        return (size_t)d->w * 4u;
    default: /* XV36, VUYX */
        return (size_t)d->w * 4u * bytes;
    }
}

/* Bytes a producer buffer of one frame needs in any import layout: twice the
 * planar frame's (the packed 4:4:4 layouts carry an unused element) plus the
 * padding the 4:2:2 groups of an odd width carry. */
static inline size_t vt_import_bytes(const VmafxFrameDesc *d)
{
    return vt_frame_bytes(d) * 2u + (size_t)d->h * 8u;
}

/* Write the packed form of the tightly packed frame `planar` (geometry `d`:
 * 4:2:2 for the YUYV family, 4:4:4 for the others) into `out`, rows of
 * vt_packed_row() bytes, laid out by hand from the formats' definitions
 * (independent of the library's table): YUYV422 / Y210 / Y212 the elements
 * Y0 Cb Y1 Cr (the top `bpc` bits of 16-bit words above 8 bits), Y410 the
 * 32-bit word Cb | Y << 10 | Cr << 20 | 3 << 30, XV36 the words Cb Y Cr X (the
 * top 12 bits), VUYX the bytes V Cb Y X. The unused element carries a marker
 * (0xa5) the import must never read. */
static inline void vt_pack_pixel(uint8_t *o, uint32_t pix_fmt, unsigned x, size_t bytes,
                                 unsigned shift, uint32_t lum, uint32_t u, uint32_t v)
{
    if (pix_fmt == VMAFX_PIXEL_FORMAT_Y410) {
        const uint32_t word = u | (lum << 10u) | (v << 20u) | (3u << 30u);
        memcpy(o + (size_t)4u * x, &word, sizeof(word));
    } else if (pix_fmt == VMAFX_PIXEL_FORMAT_XV36 || pix_fmt == VMAFX_PIXEL_FORMAT_VUYX) {
        const bool vuyx = pix_fmt == VMAFX_PIXEL_FORMAT_VUYX;
        const size_t at = (size_t)4u * x;
        vt_put_sample(o, at + (vuyx ? 1u : 0u), bytes, (uint16_t)(u << shift));
        vt_put_sample(o, at + (vuyx ? 2u : 1u), bytes, (uint16_t)(lum << shift));
        vt_put_sample(o, at + (vuyx ? 0u : 2u), bytes, (uint16_t)(v << shift));
    } else {
        const size_t group = (size_t)4u * (x / 2u);
        vt_put_sample(o, group + (size_t)(x % 2u) * 2u, bytes, (uint16_t)(lum << shift));
        if (x % 2u == 0u) {
            vt_put_sample(o, group + 1u, bytes, (uint16_t)(u << shift));
            vt_put_sample(o, group + 3u, bytes, (uint16_t)(v << shift));
        }
    }
}

static inline void vt_to_packed(const VmafxFrameDesc *d, const uint8_t *planar, uint32_t pix_fmt,
                                uint8_t *out)
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(d, w, h, row);
    const uint8_t *const cb = planar + row[0] * h[0];
    const uint8_t *const cr = cb + row[1] * h[1];
    const size_t pitch = vt_packed_row(d, pix_fmt);
    const size_t bytes = d->bpc > 8u ? 2u : 1u;
    const unsigned shift = bytes == 2u ? 16u - d->bpc : 0u;
    for (unsigned y = 0; y < h[0]; y++) {
        uint8_t *const o = out + (size_t)y * pitch;
        memset(o, 0xa5, pitch);
        for (unsigned x = 0; x < w[0]; x++) {
            const unsigned c = w[1] == w[0] ? x : x / 2u;
            vt_pack_pixel(o, pix_fmt, x, bytes, shift,
                          vt_sample(planar + (size_t)y * row[0], x, bytes),
                          vt_sample(cb + (size_t)y * row[1], c, bytes),
                          vt_sample(cr + (size_t)y * row[2], c, bytes));
        }
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

/* An import descriptor of HOST memory over the packed frame `data` written
 * by vt_to_packed() for geometry `d`. */
static inline VmafxFrameImport vt_import_packed(const VmafxFrameDesc *d, uint32_t pix_fmt,
                                                const uint8_t *data)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_HOST;
    imp.pix_fmt = pix_fmt;
    imp.bpc = d->bpc;
    imp.w = d->w;
    imp.h = d->h;
    imp.n_planes = 1u;
    imp.plane[0].handle = (uintptr_t)data;
    imp.plane[0].pitch = vt_packed_row(d, pix_fmt);
    return imp;
}

/* An import descriptor of HOST memory over the three planes of the planar
 * frame `data` of geometry `d` (YUV444P_MSB: written by the caller already
 * shifted up). */
static inline VmafxFrameImport vt_import_planar_words(const VmafxFrameDesc *d, uint32_t pix_fmt,
                                                      const uint8_t *data)
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(d, w, h, row);
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_HOST;
    imp.pix_fmt = pix_fmt;
    imp.bpc = d->bpc;
    imp.w = d->w;
    imp.h = d->h;
    imp.n_planes = 3u;
    const uint8_t *at = data;
    for (unsigned i = 0; i < 3u; i++) {
        imp.plane[i].handle = (uintptr_t)at;
        imp.plane[i].pitch = row[i];
        at += row[i] * h[i];
    }
    return imp;
}

/* Where the planes of a producer's device frame lie in its memory. */
typedef struct VtDeviceLayout {
    uint64_t offset[3];
    uint64_t pitch[3];
    uint32_t n;
} VtDeviceLayout;

/* Lay out the planes of `d` as `pix_fmt` (planar, semi-planar or packed) in one buffer, with `pad` bytes after each row. Aligned
 * (`skew` 0): every plane starts 8-byte aligned (not at the buffer's start)
 * with a pitch a multiple of 8; `skew` > 0 starts each plane `skew` bytes
 * later and leaves the pitch unrounded. Fills the rows and bytes per row of
 * each plane; returns the bytes the buffer needs. Shared by the CUDA and HIP
 * lane tests (ADR-2023, ADR-2091, ADR-2092). */
static inline size_t vt_device_layout(const VmafxFrameDesc *d, uint32_t pix_fmt, size_t pad,
                                      size_t skew, VtDeviceLayout *p, size_t rows[3],
                                      size_t row_bytes[3])
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(d, w, h, row);
    const bool packed = vt_is_packed(pix_fmt);
    const bool semi = vt_is_semi(pix_fmt);
    p->n = packed ? 1u : (semi ? 2u : 3u);
    uint64_t at = 64u;
    for (uint32_t i = 0; i < 3u; i++) {
        row_bytes[i] =
            i < p->n ? (packed ? vt_packed_row(d, pix_fmt) : row[i] * (semi && i == 1u ? 2u : 1u)) :
                       0u;
        rows[i] = i < p->n ? h[i] : 0u;
        const uint64_t pitch = row_bytes[i] ? row_bytes[i] + pad : 0u;
        p->pitch[i] = skew ? pitch : (pitch + 7u) & ~(uint64_t)7u;
        p->offset[i] = at + skew;
        at = (p->offset[i] + p->pitch[i] * rows[i] + 64u) & ~(uint64_t)63u;
    }
    return (size_t)(p->offset[p->n - 1u] + p->pitch[p->n - 1u] * rows[p->n - 1u]) + 64u;
}

/* Shift every sample of a tightly packed 16-bit frame up by `shift`. */
static inline void vt_shift_up(const VmafxFrameDesc *d, uint8_t *data, unsigned shift)
{
    const size_t n = vt_frame_bytes(d) / 2u;
    for (size_t i = 0; i < n; i++) {
        vt_put_sample(data, i, 2u, (uint16_t)(vt_sample(data, i, 2u) << shift));
    }
}

/* The tightly packed form of the producer's frame: `planar` itself for
 * YUV420P, else its semi-planar form (vt_to_semiplanar()) in `*staged`,
 * which the caller frees. NULL when out of memory. */
static inline const uint8_t *vt_producer_bytes(const VmafxFrameDesc *d, const uint8_t *planar,
                                               uint32_t pix_fmt, unsigned shift, uint8_t **staged)
{
    *staged = NULL;
    if (!vt_is_semi(pix_fmt) && !vt_is_packed(pix_fmt) && !vt_is_msb(pix_fmt)) {
        return planar;
    }
    const size_t bytes = vt_import_bytes(d);
    if (bytes == 0u) {
        return NULL;
    }
    *staged = malloc(bytes);
    if (*staged && vt_is_packed(pix_fmt)) {
        vt_to_packed(d, planar, pix_fmt, *staged);
    } else if (*staged && vt_is_msb(pix_fmt)) {
        memcpy(*staged, planar, vt_frame_bytes(d));
        vt_shift_up(d, *staged, 16u - d->bpc);
    } else if (*staged) {
        vt_to_semiplanar(d, planar, shift, *staged);
    }
    return *staged;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_IMPORT_TEST_UTIL_H */
