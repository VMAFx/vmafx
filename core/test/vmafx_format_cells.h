/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The 4:2:2 and 4:4:4 import formats on a device (RC4 WP3, ADR-2133), shared
 * by the CUDA, HIP and SYCL tests: a fixture's chroma repeated into the wider
 * layouts (vt_clip_to_chroma()), the clip held by the producer in each
 * format (planar device pointers, semi-planar NVxx / Pxxx, packed Y210 /
 * Y410), imported, and every cell of vmafx_device_cells.h scored as host
 * frames and as imports, bit for bit. The backend supplies how a frame is
 * held and imported (VfOps); this header runs the sessions and the table.
 *
 * VMAFX_TEST_FORMAT_GROUP=<8|10|12|16>: the formats of that bit depth only;
 * VMAFX_TEST_FORMATS=<name>[,<name>...]: only those formats (names of
 * vf_formats).
 */

#ifndef VMAFX_FORMAT_CELLS_H
#define VMAFX_FORMAT_CELLS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmafx/vmafx.h"
/* The cell table: the CUDA / HIP lanes' (vmafx_device_cells.h); a lane with its
 * own (the SYCL lane's vmafx_sycl_cells.h) defines VF_CELLS_HEADER, VF_CELLS
 * and VF_N_CELLS before including this header. */
#ifndef VF_CELLS_HEADER
#define VF_CELLS_HEADER "vmafx_device_cells.h"
#define VF_CELLS vc_cells
#define VF_N_CELLS VC_N_CELLS
#endif
#include VF_CELLS_HEADER
#include "vmafx_fixture_util.h"
#include "vmafx_import_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr`. ADR-1138. */

/* Frames of each clip compared. */
#define VF_FRAMES 3u
/* Attempts at a cell whose values differ (the HIP platform defect,
 * T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01). */
#define VF_ATTEMPTS 4u

typedef struct VfFormat {
    const char *name;
    uint32_t planar_fmt; /* YUV422P or YUV444P: the frame the import makes */
    uint32_t bpc;        /* 8, 10 or 16 */
    uint32_t pix_fmt;    /* what the producer holds: planar_fmt, NVxx, Pxxx, Y210, Y410, RGB ... */
    unsigned shift;      /* left shift of the producer's samples */
    /* RGB layouts (ADR-2146): the statement of the frames. The producer holds
     * the clip's three planes as R, G, B; the host session scores what the
     * CPU import makes of the same bytes, the device import is compared to it. */
    uint32_t matrix;
    uint32_t range;
    uint32_t out_range;
} VfFormat;

static const VfFormat vf_formats[] = {
    {.name = "yuv422p",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .shift = 0u},
    {.name = "yuv444p",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .shift = 0u},
    {.name = "nv16",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_NV16,
     .shift = 0u},
    {.name = "nv24",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_NV24,
     .shift = 0u},
    {.name = "yuyv422",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUYV422,
     .shift = 0u},
    {.name = "vuyx",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_VUYX,
     .shift = 0u},
    {.name = "yuv422p10",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 10u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .shift = 0u},
    {.name = "yuv444p10",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 10u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .shift = 0u},
    {.name = "p210",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 10u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_P210,
     .shift = 6u},
    {.name = "p410",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 10u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_P410,
     .shift = 6u},
    {.name = "y210",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 10u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_Y210,
     .shift = 6u},
    {.name = "y410",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 10u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_Y410,
     .shift = 0u},
    {.name = "yuv444p10msb",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 10u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUV444P_MSB,
     .shift = 0u},
    {.name = "yuv444p12",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 12u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .shift = 0u},
    {.name = "y212",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 12u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_Y212,
     .shift = 4u},
    {.name = "xv36",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 12u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_XV36,
     .shift = 4u},
    {.name = "yuv444p12msb",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 12u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUV444P_MSB,
     .shift = 0u},
    {.name = "yuv422p16",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 16u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .shift = 0u},
    {.name = "yuv444p16",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 16u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .shift = 0u},
    {.name = "p216",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 16u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_P216,
     .shift = 0u},
    {.name = "p416",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 16u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_P416,
     .shift = 0u},
    {.name = "uyvy422",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_UYVY422,
     .shift = 0u},
    {.name = "ayuv",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_AYUV,
     .shift = 0u},
    {.name = "v210",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV422P,
     .bpc = 10u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_V210,
     .shift = 0u},
    {.name = "rgb24_bt709_full_limited",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_RGB,
     .shift = 0u,
     .matrix = VMAFX_COLOR_MATRIX_BT709,
     .range = VMAFX_COLOR_RANGE_FULL,
     .out_range = VMAFX_COLOR_RANGE_LIMITED},
    {.name = "rgba_bt601_limited_full",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 8u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_RGBA,
     .shift = 0u,
     .matrix = VMAFX_COLOR_MATRIX_BT601,
     .range = VMAFX_COLOR_RANGE_LIMITED,
     .out_range = VMAFX_COLOR_RANGE_FULL},
    {.name = "bgra10_bt2020_full_full",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 10u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_BGRA,
     .shift = 0u,
     .matrix = VMAFX_COLOR_MATRIX_BT2020_NCL,
     .range = VMAFX_COLOR_RANGE_FULL,
     .out_range = VMAFX_COLOR_RANGE_FULL},
    {.name = "rgb12_bt709_limited_limited",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 12u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_RGB,
     .shift = 0u,
     .matrix = VMAFX_COLOR_MATRIX_BT709,
     .range = VMAFX_COLOR_RANGE_LIMITED,
     .out_range = VMAFX_COLOR_RANGE_LIMITED},
    {.name = "rgb48_bt601_full_limited",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 16u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_RGB,
     .shift = 0u,
     .matrix = VMAFX_COLOR_MATRIX_BT601,
     .range = VMAFX_COLOR_RANGE_FULL,
     .out_range = VMAFX_COLOR_RANGE_LIMITED},
    {.name = "rgba64_bt2020_full_limited",
     .planar_fmt = VMAFX_PIXEL_FORMAT_YUV444P,
     .bpc = 16u,
     .pix_fmt = VMAFX_PIXEL_FORMAT_RGBA,
     .shift = 0u,
     .matrix = VMAFX_COLOR_MATRIX_BT2020_NCL,
     .range = VMAFX_COLOR_RANGE_FULL,
     .out_range = VMAFX_COLOR_RANGE_LIMITED},
};

#define VF_N_FORMATS (sizeof(vf_formats) / sizeof(vf_formats[0]))

/* How a backend holds and imports the producer's frames. `held` is the
 * backend's own object for one frame. */
typedef struct VfOps {
    VmafxDevice *device;
    void *self;
    /* Put `planar` (geometry `d`) into the producer's memory as `pix_fmt`
     * (samples shifted left by `shift`); NULL when it cannot. */
    void *(*hold)(void *self, const VmafxFrameDesc *d, const uint8_t *planar, uint32_t pix_fmt,
                  unsigned shift);
    /* Import it for `context`; NULL on a refusal. */
    VmafxFrame *(*import)(void *self, VmafxContext *context, const VmafxFrameDesc *d,
                          uint32_t pix_fmt, void *held);
    void (*release)(void *self, void *held);
} VfOps;

typedef struct VfResult {
    unsigned long cells;
    unsigned long values;
    unsigned long differing; /* cells that differed in every attempt */
    unsigned long reruns;
    unsigned long refused; /* cells the extractor refuses for the host frames too */
} VfResult;

/* The format's clip: the Netflix pair at 8 bits, else the 10-bit sparks pair
 * with its samples scaled up to the format's depth; the first VF_FRAMES
 * frames. */
static inline bool vf_clip(const VfFormat *f, VtClip *out)
{
    VtClip base;
    memset(out, 0, sizeof(*out));
    const bool open = vt_clip_open(&base, &vt_inputs[f->bpc == 8u ? 0u : 3u]);
    bool ok = open && vt_clip_to_chroma(&base, f->planar_fmt, out);
    if (ok && f->bpc > 10u) {
        const VmafxFrameDesc scaled = vt_desc(f->planar_fmt, f->bpc, out->desc.w, out->desc.h);
        const size_t frame = vt_frame_bytes(&out->desc);
        for (unsigned i = 0; i < out->n_frames; i++) {
            vt_shift_up(&scaled, out->ref + (size_t)i * frame, f->bpc - 10u);
            vt_shift_up(&scaled, out->dist + (size_t)i * frame, f->bpc - 10u);
        }
        out->desc = scaled;
    }
    if (ok && out->n_frames > VF_FRAMES) {
        out->n_frames = VF_FRAMES;
    }
    vt_clip_close(&base);
    return ok;
}

/* The frame `data` of `clip` (R, G, B planes) imported by the CPU device as
 * `f`'s RGB layout, written back as the planar Y'CbCr frame it makes: what a
 * host session scores. False when the import is refused. */
static inline bool vf_rgb_frame_to_ycbcr(const VfFormat *f, const VmafxFrameDesc *d,
                                         const uint8_t *data, uint8_t *out)
{
    const size_t bytes = d->bpc > 8u ? 2u : 1u;
    const size_t pixels = (size_t)d->w * d->h;
    const size_t elems = f->pix_fmt == VMAFX_PIXEL_FORMAT_RGB ? 3u : 4u;
    uint8_t *const rgb = malloc(pixels * elems * bytes);
    if (!rgb) {
        return false;
    }
    VmafxFrameDesc one = *d;
    one.pix_fmt = f->pix_fmt;
    vt_to_packed(d, data, f->pix_fmt, rgb);
    VmafxFrameImport imp = vt_import_packed(&one, f->pix_fmt, rgb);
    vt_apply_rgb_statement(&imp);
    VmafxFrame *frame = NULL;
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    bool ok = vmafx_frame_import(NULL, &imp, &frame, NULL) == VMAFX_OK &&
              vmafx_frame_planes(frame, &planes, NULL) == VMAFX_OK;
    for (unsigned p = 0; p < 3u && ok; p++) {
        for (unsigned y = 0; y < planes.h[p]; y++) {
            memcpy(out, (const uint8_t *)planes.data[p] + (size_t)y * planes.stride[p],
                   (size_t)planes.w[p] * bytes);
            out += (size_t)planes.w[p] * bytes;
        }
    }
    vmafx_frame_unref(frame);
    free(rgb);
    return ok;
}

/* The clip the host session scores: `src` itself, or for an RGB layout the
 * Y'CbCr frames the CPU import makes of it (`*out` owns new buffers). */
static inline bool vf_scored_clip(const VfFormat *f, const VtClip *src, VtClip *out)
{
    *out = *src;
    if (!vt_is_rgb(f->pix_fmt)) {
        return true;
    }
    const size_t frame = vt_frame_bytes(&src->desc);
    out->ref = malloc(frame * src->n_frames);
    out->dist = malloc(frame * src->n_frames);
    bool ok = out->ref && out->dist;
    for (unsigned i = 0; i < src->n_frames && ok; i++) {
        ok = vf_rgb_frame_to_ycbcr(f, &src->desc, src->ref + i * frame, out->ref + i * frame) &&
             vf_rgb_frame_to_ycbcr(f, &src->desc, src->dist + i * frame, out->dist + i * frame);
    }
    return ok;
}

static inline void vf_scored_close(const VfFormat *f, VtClip *scored)
{
    if (vt_is_rgb(f->pix_fmt)) {
        free(scored->ref);
        free(scored->dist);
    }
}

static inline VmafxContext *vf_host_session(VmafxDevice *device, const VtClip *clip,
                                            const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(device, cell);
    const size_t frame = vt_frame_bytes(&clip->desc);
    bool ok = context != NULL;
    for (unsigned i = 0; i < clip->n_frames && ok; i++) {
        VmafxFrame *ref = vt_wrap_frame(&clip->desc, clip->ref + i * frame, NULL);
        VmafxFrame *dist = vt_wrap_frame(&clip->desc, clip->dist + i * frame, NULL);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    return ok ? context : NULL;
}

/* The import session: every frame held and imported through `ops`. */
static inline VmafxContext *vf_import_session(const VfOps *ops, const VtClip *clip,
                                              const VfFormat *f, void *held[][2],
                                              const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(ops->device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < clip->n_frames && ok; i++) {
        VmafxFrame *const ref =
            ops->import(ops->self, context, &clip->desc, f->pix_fmt, held[i][0]);
        VmafxFrame *const dist =
            ops->import(ops->self, context, &clip->desc, f->pix_fmt, held[i][1]);
        if (!ref || !dist) {
            vmafx_frame_unref(ref);
            vmafx_frame_unref(dist);
        }
        ok = ref && dist && vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    return ok ? context : NULL;
}

/* One attempt: 1 bit-identical, 0 values differ, -1 a session failed, -2
 * the extractor refuses the frames on both sides (a feature that does not
 * take the depth or chroma layout; named by the caller, never scored). */
static inline int vf_attempt(const VfOps *ops, const VtClip *scored, const VtClip *clip,
                             const VfFormat *f, void *held[][2], const VcCell *cell, VfResult *r)
{
    unsigned long compared = 0;
    unsigned long differing = 0;
    VmafxContext *const host = vf_host_session(ops->device, scored, cell);
    VmafxContext *const imp = vf_import_session(ops, clip, f, held, cell);
    if (!host && !imp) {
        return -2;
    }
    const bool same = host && imp && vc_compare(host, imp, clip->n_frames, &compared, &differing);
    const bool destroyed = (!host || vmafx_context_destroy(host, NULL) == VMAFX_OK) &&
                           (!imp || vmafx_context_destroy(imp, NULL) == VMAFX_OK);
    if (!same || !destroyed) {
        return -1;
    }
    r->values += compared;
    return differing == 0u ? 1 : 0;
}

/* One cell over one format: up to VF_ATTEMPTS attempts. 1 bit-identical, 0 values differ in every
 * attempt, -1 a session failed, -2 refused for both sides. Updates the counters of `r`. */
static inline int vf_run_cell(const VfOps *ops, const VtClip *scored, const VtClip *clip,
                              const VfFormat *f, void *held[][2], const VcCell *cell, VfResult *r)
{
    int verdict = -1;
    for (unsigned a = 0; a < VF_ATTEMPTS && verdict != 1; a++) {
        verdict = vf_attempt(ops, scored, clip, f, held, cell, r);
        if (verdict == -2) {
            (void)fprintf(stderr, "\n  %s cell %s: refused for host and imported frames", f->name,
                          cell->name);
            r->refused++;
            break;
        }
        if (verdict != 1) {
            r->reruns++;
            (void)fprintf(stderr, "\n  %s cell %s attempt %u: %s", f->name, cell->name, a + 1u,
                          verdict == 0 ? "values differ" : "session failed");
        }
        if (verdict < 0) {
            break;
        }
    }
    return verdict;
}

/* Every cell of the table over one held clip. NULL when every cell scored the same bits. */
static inline char *vf_run_cells(const VfOps *ops, const VtClip *scored, const VtClip *clip,
                                 const VfFormat *f, void *held[][2], VfResult *r)
{
    char *msg = NULL;
    const unsigned cw = (clip->desc.w + 1u) / 2u;
    for (size_t c = 0; c < VF_N_CELLS && !msg; c++) {
        const VcCell *const cell = &VF_CELLS[c];
        const unsigned chroma_w = f->planar_fmt == VMAFX_PIXEL_FORMAT_YUV444P ? clip->desc.w : cw;
        if (cell->min_chroma != 0u &&
            (chroma_w < cell->min_chroma || clip->desc.h < cell->min_chroma)) {
            continue;
        }
        const int verdict = vf_run_cell(ops, scored, clip, f, held, cell, r);
        if (verdict == -2) {
            continue;
        }
        r->cells++;
        r->differing += verdict == 1 ? 0u : 1u;
        msg = verdict == 1 ? NULL : (verdict < 0 ? "cell sessions" : "values differ");
    }
    return msg;
}

/* Every cell of the table over one format; fills `r`. NULL when every cell
 * scored the same bits, else a message. */
static inline char *vf_run_format(const VfOps *ops, const VfFormat *f, VfResult *r)
{
    VtClip clip;
    VtClip scored;
    if (!vf_clip(f, &clip)) {
        vt_clip_close(&clip);
        return "format clip";
    }
    *vt_rgb_statement() = (VtRgbStatement){.matrix = f->matrix,
                                           .range = f->range,
                                           .transfer = VMAFX_COLOR_TRC_SRGB,
                                           .out_range = f->out_range};
    if (!vf_scored_clip(f, &clip, &scored)) {
        vf_scored_close(f, &scored);
        vt_clip_close(&clip);
        return "rgb clip";
    }
    void *(*held)[2] = calloc(clip.n_frames, sizeof(*held));
    const size_t frame = vt_frame_bytes(&clip.desc);
    bool ok = held != NULL;
    for (unsigned i = 0; i < clip.n_frames && ok; i++) {
        held[i][0] = ops->hold(ops->self, &clip.desc, clip.ref + i * frame, f->pix_fmt, f->shift);
        held[i][1] = ops->hold(ops->self, &clip.desc, clip.dist + i * frame, f->pix_fmt, f->shift);
        ok = held[i][0] && held[i][1];
    }
    char *const msg = ok ? vf_run_cells(ops, &scored, &clip, f, held, r) : "hold";
    for (unsigned i = 0; i < clip.n_frames && held; i++) {
        for (unsigned k = 0; k < 2u; k++) {
            if (held[i][k]) {
                ops->release(ops->self, held[i][k]);
            }
        }
    }
    free(held);
    vf_scored_close(f, &scored);
    vt_clip_close(&clip);
    return msg;
}

/* Whether format `f` is selected by VMAFX_TEST_FORMAT_GROUP and
 * VMAFX_TEST_FORMATS. */
static inline bool vf_selected(const VfFormat *f)
{
    /* NOLINTBEGIN(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    const char *const group = getenv("VMAFX_TEST_FORMAT_GROUP");
    const char *const names = getenv("VMAFX_TEST_FORMATS");
    /* NOLINTEND(concurrency-mt-unsafe) */
    if (group && group[0] != '\0' && strtoul(group, NULL, 10) != (unsigned long)f->bpc) {
        return false;
    }
    if (!names || names[0] == '\0') {
        return true;
    }
    const size_t n = strlen(f->name);
    for (const char *at = names; at && *at; at = strchr(at, ',') ? strchr(at, ',') + 1 : NULL) {
        if (strncmp(at, f->name, n) == 0 && (at[n] == ',' || at[n] == '\0')) {
            return true;
        }
    }
    return false;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_FORMAT_CELLS_H */
