/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The import layout table's row type and what a layout means (RC4 WP3,
 * ADR-2133; WP13, ADR-2145): which planes the producer hands over, where each
 * plane of the planar frame it makes is read from (vmafx_import_plane_read(),
 * the plan vmafx_import_read_plane() executes), and how many bytes a row of a
 * producer plane takes. Header only, plain C, no engine dependency: the
 * library's host import (frame_import.c), the device lanes and the vmaf
 * command line's raw reader (core/tools/yuv_input.c) call it, so there is one
 * behaviour for each layout (HISS-19). The rows come from the generated table
 * (import_layouts_gen.h, `[[pixel_formats]]` of core/api/vmafx.toml).
 */

#ifndef VMAF_SRC_VMAFX_IMPORT_LAYOUT_H_
#define VMAF_SRC_VMAFX_IMPORT_LAYOUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vmafx/import_convert.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header included by C translation
 * units; MSVC's C mode has no `nullptr`. ADR-1138. */

/* V210: six pixels in 16 bytes. */
#define VMAFX_V210_PIXELS 6u
#define VMAFX_V210_GROUP 16u

/* How a producer lays out one pixel format vmafx_frame_import() takes. */
typedef struct VmafxImportLayout {
    uint32_t pix_fmt;    /* VmafxPixelFormat the producer hands over */
    uint32_t planar_fmt; /* VmafxPixelFormat of the frame it makes */
    uint32_t n_planes;   /* planes the producer hands over */
    uint32_t bpc_min;
    uint32_t bpc_max;
    uint32_t shift;       /* right shift of every sample (P010: 6) */
    bool interleaved;     /* plane 1 holds Cb / Cr pairs */
    const char *name;     /* FFmpeg's name, for messages */
    uint32_t packed;      /* VmafxImportPacked: one plane of packed Y, Cb, Cr */
    uint8_t elem[3];      /* packed: element of a group that holds Y, Cb, Cr */
    bool msb;             /* the `bpc` most significant bits of 16-bit words (shift 16 - bpc) */
    uint8_t rgb_elems;    /* RGB: elements per pixel (3, or 4 with an alpha sample) */
    bool needs_statement; /* RGB: accepted only with matrix, range and transfer (ADR-2146) */
} VmafxImportLayout;

/* Packed layouts (one producer plane, ADR-2133): 0 is none. */
typedef enum VmafxImportPacked {
    VMAFX_IMPORT_PACKED_NONE = 0,
    /* Y0 Cb Y1 Cr elements (bytes at 8 bits, else 16-bit words) per two pixels:
     * elem = Y0, Cb, Cr (0, 1, 3). */
    VMAFX_IMPORT_PACKED_YUYV = 1,
    /* Four elements per pixel (bytes at 8 bits, else 16-bit words); elem
     * gives the position of Y, Cb and Cr. */
    VMAFX_IMPORT_PACKED_UYV4 = 2,
    /* One 32-bit word per pixel: Cb | Y << 10 | Cr << 20 (Y410, XV30). */
    VMAFX_IMPORT_PACKED_XVYU2101010 = 3,
    /* V210: 10-bit samples, six pixels in four 32-bit words (ADR-2145). */
    VMAFX_IMPORT_PACKED_V210 = 4,
    /* R'G'B' with elem giving the position of R, G and B among rgb_elems
     * elements per pixel; converted to Y'CbCr by rgb_convert.h (ADR-2146). */
    VMAFX_IMPORT_PACKED_RGB = 5
} VmafxImportPacked;

/* The right shift of every sample of a frame of `bpc` bits in `layout`. */
static inline uint32_t vmafx_import_shift(const VmafxImportLayout *layout, uint32_t bpc)
{
    return layout->msb ? 16u - bpc : layout->shift;
}

/* Bytes of one row of producer plane `i` and its rows, for the planar
 * geometry `pw` / `ph` of the frame. */
static inline void vmafx_import_plane_extent(const VmafxImportLayout *layout, uint32_t bpc,
                                             uint32_t i, const unsigned pw[3], const unsigned ph[3],
                                             uint64_t *row, uint64_t *rows)
{
    const uint64_t bytes = bpc > 8u ? 2u : 1u;
    if (layout->packed == VMAFX_IMPORT_PACKED_V210) {
        /* Four 32-bit words per six pixels; the last group may be partial. */
        *row = ((uint64_t)pw[0] + VMAFX_V210_PIXELS - 1u) / VMAFX_V210_PIXELS * VMAFX_V210_GROUP;
        *rows = ph[0];
        return;
    }
    if (layout->packed == VMAFX_IMPORT_PACKED_RGB) {
        *row = (uint64_t)pw[0] * layout->rgb_elems * bytes;
        *rows = ph[0];
        return;
    }
    if (layout->packed != VMAFX_IMPORT_PACKED_NONE) {
        /* YUYV: four elements per chroma column; UYV4: four per pixel;
         * XV30: a 32-bit word per pixel. */
        const uint64_t elements =
            layout->packed == VMAFX_IMPORT_PACKED_YUYV ?
                4u * pw[1] :
                (layout->packed == VMAFX_IMPORT_PACKED_UYV4 ? 4u * pw[0] : 0u);
        *row = layout->packed == VMAFX_IMPORT_PACKED_XVYU2101010 ? 4u * (uint64_t)pw[0] :
                                                                   elements * bytes;
        *rows = ph[0];
        return;
    }
    const uint64_t pair = layout->interleaved && i == 1u ? 2u : 1u;
    *row = (uint64_t)pw[i] * bytes * pair;
    *rows = ph[i];
}

static inline void vmafx_import_plane_read(const VmafxImportLayout *layout, uint32_t bpc,
                                           uint32_t i, VmafxImportRead *out)
{
    VmafxImportRead rd = {.src_plane = i,
                          .step = 1u,
                          .offset = 0u,
                          .in_bytes = bpc > 8u ? 2u : 1u,
                          .shift = vmafx_import_shift(layout, bpc),
                          .mask = 0u,
                          .period = 0u,
                          .group_elems = 0u,
                          .pattern = 0u};
    if (layout->packed != VMAFX_IMPORT_PACKED_NONE) {
        rd.src_plane = 0u;
    }
    if (layout->packed == VMAFX_IMPORT_PACKED_V210) {
        /* Nibble k of a pattern: element of the group | (shift / 10) << 2. */
        static const uint32_t pattern[3] = {0xB36914u, 0x000A50u, 0x000728u};
        rd.in_bytes = 4u;
        rd.mask = 0x3ffu;
        rd.period = i == 0u ? 6u : 3u;
        rd.group_elems = 4u;
        rd.pattern = pattern[i];
    } else if (layout->packed == VMAFX_IMPORT_PACKED_YUYV) {
        rd.step = i == 0u ? 2u : 4u;
        rd.offset = layout->elem[i];
    } else if (layout->packed == VMAFX_IMPORT_PACKED_UYV4) {
        rd.step = 4u;
        rd.offset = layout->elem[i];
    } else if (layout->packed == VMAFX_IMPORT_PACKED_XVYU2101010) {
        rd.in_bytes = 4u;
        rd.mask = 0x3ffu;
        rd.shift = i == 0u ? 10u : (i == 1u ? 0u : 20u);
    } else if (layout->interleaved && i > 0u) {
        rd.src_plane = 1u;
        rd.step = 2u;
        rd.offset = i - 1u;
    }
    *out = rd;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAF_SRC_VMAFX_IMPORT_LAYOUT_H_ */
