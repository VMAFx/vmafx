/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The YUV fixture pairs the VMAFx bit-exactness tests score (RC4 WP2 and
 * WP3): the Netflix 576x324 golden pair, both 1080p checkerboard pairs and
 * the 10-bit sparks pair, read whole from python/test/resource/yuv
 * (VMAFX_TEST_YUV_DIR, set by core/test/meson.build).
 */

#ifndef VMAFX_FIXTURE_UTIL_H
#define VMAFX_FIXTURE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr` and the required
 * Windows builds compile the tests with cl.exe (C2065). ADR-1138. */

#ifndef VMAFX_TEST_YUV_DIR
#error "VMAFX_TEST_YUV_DIR: the fixture directory, set by core/test/meson.build"
#endif

typedef struct VtInput {
    const char *ref;
    const char *dist;
    uint32_t w, h, bpc;
} VtInput;

static const VtInput vt_inputs[] = {
    {"src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, 8},
    {"checkerboard_1920_1080_10_3_0_0.yuv", "checkerboard_1920_1080_10_3_1_0.yuv", 1920, 1080, 8},
    {"checkerboard_1920_1080_10_3_0_0.yuv", "checkerboard_1920_1080_10_3_10_0.yuv", 1920, 1080, 8},
    {"sparks_ref_480x270.yuv42010le.yuv", "sparks_dis_480x270.yuv42010le.yuv", 480, 270, 10},
};

#define VT_N_INPUTS (sizeof(vt_inputs) / sizeof(vt_inputs[0]))

/* A fixture pair: tightly packed YUV420P frames, reference and distorted. */
typedef struct VtClip {
    VmafxFrameDesc desc;
    uint8_t *ref;
    uint8_t *dist;
    unsigned n_frames;
} VtClip;

/* The whole file `name` of the fixture directory, or NULL. */
static inline uint8_t *vt_read_fixture(const char *name, size_t *size)
{
    char path[4096];
    const int n = snprintf(path, sizeof(path), "%s/%s", VMAFX_TEST_YUV_DIR, name);
    FILE *file = n > 0 && (size_t)n < sizeof(path) ? fopen(path, "rb") : NULL;
    if (!file) {
        return NULL;
    }
    uint8_t *data = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long end = ftell(file);
        *size = end > 0 ? (size_t)end : 0u;
        data = *size && fseek(file, 0, SEEK_SET) == 0 ? malloc(*size) : NULL;
    }
    if (data && fread(data, 1, *size, file) != *size) {
        free(data);
        data = NULL;
    }
    (void)fclose(file);
    return data;
}

/* Read a pair; false when a file is missing or the pair is not whole frames
 * of equal length (vt_clip_close() releases what was read either way). */
static inline bool vt_clip_open(VtClip *clip, const VtInput *in)
{
    clip->desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, in->bpc, in->w, in->h);
    size_t ref_size = 0;
    size_t dist_size = 0;
    clip->ref = vt_read_fixture(in->ref, &ref_size);
    clip->dist = vt_read_fixture(in->dist, &dist_size);
    const size_t frame = vt_frame_bytes(&clip->desc);
    clip->n_frames = (unsigned)(ref_size / frame);
    return clip->ref && clip->dist && ref_size == dist_size && clip->n_frames > 0;
}

static inline void vt_clip_close(VtClip *clip)
{
    free(clip->ref);
    free(clip->dist);
}

/* Chroma plane `c` (1 or 2) of one frame: the 4:2:0 plane `ip` (`sw` x `sh`
 * samples, `srow` bytes per row) repeated into `dw` x `dh` samples at `op`. */
static inline void vt_chroma_plane(uint8_t *op, size_t drow, unsigned dw, unsigned dh,
                                   const uint8_t *ip, size_t srow, unsigned sw, unsigned sh,
                                   size_t bytes)
{
    for (unsigned y = 0; y < dh; y++) {
        const unsigned sy = y / 2u < sh ? y / 2u : sh - 1u;
        for (unsigned x = 0; x < dw; x++) {
            const unsigned sx = dw == sw ? x : x / 2u;
            memcpy(op + y * drow + (size_t)x * bytes, ip + sy * srow + (size_t)sx * bytes, bytes);
        }
    }
}

/* One frame of `src` widened to the geometry of `dst` (the planes' sizes in
 * the two clips' descriptors), luma copied. */
static inline void vt_frame_to_chroma(const VtClip *src, const VtClip *dst, const uint8_t *ip,
                                      uint8_t *op)
{
    unsigned sw[3];
    unsigned sh[3];
    size_t srow[3];
    unsigned dw[3];
    unsigned dh[3];
    size_t drow[3];
    vt_plane_geometry(&src->desc, sw, sh, srow);
    vt_plane_geometry(&dst->desc, dw, dh, drow);
    const size_t bytes = src->desc.bpc > 8u ? 2u : 1u;
    memcpy(op, ip, srow[0] * sh[0]);
    ip += srow[0] * sh[0];
    op += drow[0] * dh[0];
    for (unsigned c = 1; c < 3u; c++) {
        vt_chroma_plane(op, drow[c], dw[c], dh[c], ip, srow[c], sw[c], sh[c], bytes);
        ip += srow[c] * sh[c];
        op += drow[c] * dh[c];
    }
}

/* The chroma of `src` (YUV420P frames) as `planar_fmt` (YUV422P or YUV444P):
 * every chroma sample repeated into the positions it covers, luma and depth
 * unchanged, so the content is the fixture's (ADR-2133). `dst` owns new
 * buffers (vt_clip_close()); false without memory or for another format. */
static inline bool vt_clip_to_chroma(const VtClip *src, uint32_t planar_fmt, VtClip *dst)
{
    memset(dst, 0, sizeof(*dst));
    if (planar_fmt != VMAFX_PIXEL_FORMAT_YUV422P && planar_fmt != VMAFX_PIXEL_FORMAT_YUV444P) {
        return false;
    }
    dst->desc = vt_desc(planar_fmt, src->desc.bpc, src->desc.w, src->desc.h);
    dst->n_frames = src->n_frames;
    const size_t in_frame = vt_frame_bytes(&src->desc);
    const size_t out_frame = vt_frame_bytes(&dst->desc);
    dst->ref = malloc(out_frame * src->n_frames);
    dst->dist = malloc(out_frame * src->n_frames);
    for (unsigned f = 0; f < src->n_frames && dst->ref && dst->dist; f++) {
        vt_frame_to_chroma(src, dst, src->ref + f * in_frame, dst->ref + f * out_frame);
        vt_frame_to_chroma(src, dst, src->dist + f * in_frame, dst->dist + f * out_frame);
    }
    return dst->ref && dst->dist;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_FIXTURE_UTIL_H */
