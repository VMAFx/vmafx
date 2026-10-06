/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The CPU reference of the import conversions (RC4 WP3, ADR-1929 item 6,
 * ADR-2133): how a plane of the planar frame an import makes is read out of
 * the producer's planes, for every layout vmafx_frame_import() takes beyond
 * planar: semi-planar chroma (NV12 / NV16 / NV24 and the P0xx / P2xx / P4xx
 * family: one luma plane and one plane of interleaved Cb / Cr), packed 4:2:2
 * (Y210 / Y216: Y0 Cb Y1 Cr in 16-bit words) and packed 4:4:4 (Y410 / XV30:
 * one 32-bit word per pixel holding Cb, Y, Cr in bits 0 to 9, 10 to 19 and
 * 20 to 29). Every output sample is one input sample, shifted right and
 * masked: nothing is rounded, scaled or filtered, so the result is bit for
 * bit the planar frame a host upload of the same samples makes. The CUDA, HIP
 * and SYCL kernels (core/src/vmafx/import_convert_kernels.h, the SYCL import
 * kernels) do the same reads on the device; a change here changes them in the
 * same PR (core/test/test_vmafx_import_convert.c and the device bit-exactness
 * tests hold them together). The host import (frame_import.c) and the Metal
 * IOSurface import (metal/iosurface_layout.h) call vmafx_import_read_plane():
 * one implementation (HISS-19), reusable by any host input of these layouts.
 *
 * Plain C, header only, no engine dependency.
 */

#ifndef VMAF_SRC_VMAFX_IMPORT_CONVERT_H_
#define VMAF_SRC_VMAFX_IMPORT_CONVERT_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* NOLINTBEGIN(modernize-use-nullptr): C header included by C translation
 * units; MSVC's C mode has no `nullptr`. ADR-1138. */

/* How a plane of the frame is read: sample `x` of a row is element
 * `x * step + offset` of the producer's row (elements of `in_bytes` bytes,
 * little-endian), shifted right by `shift`, then masked with `mask` (0: no
 * mask). Output samples are 1 byte (`out_bytes` 1) or 2 bytes. */
typedef struct VmafxImportRead {
    uint32_t src_plane; /* the producer plane the row is in */
    uint32_t step;      /* elements per output sample */
    uint32_t offset;    /* element within the step */
    uint32_t in_bytes;  /* 1, 2 or 4 */
    uint32_t shift;
    uint32_t mask;
} VmafxImportRead;

/* One input element at byte `at` of `row`, little-endian. */
static inline uint32_t vmafx_import_load(const uint8_t *row, size_t at, uint32_t in_bytes)
{
    uint32_t v = row[at];
    if (in_bytes == 2u) {
        v |= (uint32_t)row[at + 1u] << 8u;
    } else if (in_bytes == 4u) {
        v |= ((uint32_t)row[at + 1u] << 8u) | ((uint32_t)row[at + 2u] << 16u) |
             ((uint32_t)row[at + 3u] << 24u);
    }
    return v;
}

/* One output sample of the row `s`. */
static inline uint32_t vmafx_import_sample(const uint8_t *s, uint32_t x, const VmafxImportRead *rd)
{
    const size_t element = (size_t)x * rd->step + rd->offset;
    uint32_t v = vmafx_import_load(s, element * rd->in_bytes, rd->in_bytes) >> rd->shift;
    if (rd->mask != 0u) {
        v &= rd->mask;
    }
    return v;
}

/* `w` x `h` samples of the planned read `rd` from `src` (`src_pitch` bytes
 * per row) into `dst` (`dst_stride` bytes per row, `out_bytes` per sample). */
static inline void vmafx_import_read_plane(uint8_t *dst, size_t dst_stride, uint32_t out_bytes,
                                           const uint8_t *src, size_t src_pitch, unsigned w,
                                           unsigned h, const VmafxImportRead *rd)
{
    for (unsigned y = 0u; y < h; y++) {
        uint8_t *const d = dst + (size_t)y * dst_stride;
        const uint8_t *const s = src + (size_t)y * src_pitch;
        if (out_bytes == 1u && rd->step == 1u && rd->in_bytes == 1u && rd->shift == 0u &&
            rd->mask == 0u && rd->offset == 0u) {
            memcpy(d, s, (size_t)w);
            continue;
        }
        for (unsigned x = 0u; x < w; x++) {
            const uint32_t v = vmafx_import_sample(s, x, rd);
            if (out_bytes == 1u) {
                d[x] = (uint8_t)v;
            } else {
                d[2u * (size_t)x] = (uint8_t)v;
                d[2u * (size_t)x + 1u] = (uint8_t)(v >> 8u);
            }
        }
    }
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAF_SRC_VMAFX_IMPORT_CONVERT_H_ */
