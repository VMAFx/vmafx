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

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_FIXTURE_UTIL_H */
