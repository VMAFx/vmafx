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
    uint32_t pix_fmt;    /* what the producer holds: planar_fmt, NVxx, Pxxx, Y210 or Y410 */
    unsigned shift;      /* left shift of the producer's samples */
} VfFormat;

static const VfFormat vf_formats[] = {
    {"yuv422p", VMAFX_PIXEL_FORMAT_YUV422P, 8u, VMAFX_PIXEL_FORMAT_YUV422P, 0u},
    {"yuv444p", VMAFX_PIXEL_FORMAT_YUV444P, 8u, VMAFX_PIXEL_FORMAT_YUV444P, 0u},
    {"nv16", VMAFX_PIXEL_FORMAT_YUV422P, 8u, VMAFX_PIXEL_FORMAT_NV16, 0u},
    {"nv24", VMAFX_PIXEL_FORMAT_YUV444P, 8u, VMAFX_PIXEL_FORMAT_NV24, 0u},
    {"yuyv422", VMAFX_PIXEL_FORMAT_YUV422P, 8u, VMAFX_PIXEL_FORMAT_YUYV422, 0u},
    {"vuyx", VMAFX_PIXEL_FORMAT_YUV444P, 8u, VMAFX_PIXEL_FORMAT_VUYX, 0u},
    {"yuv422p10", VMAFX_PIXEL_FORMAT_YUV422P, 10u, VMAFX_PIXEL_FORMAT_YUV422P, 0u},
    {"yuv444p10", VMAFX_PIXEL_FORMAT_YUV444P, 10u, VMAFX_PIXEL_FORMAT_YUV444P, 0u},
    {"p210", VMAFX_PIXEL_FORMAT_YUV422P, 10u, VMAFX_PIXEL_FORMAT_P210, 6u},
    {"p410", VMAFX_PIXEL_FORMAT_YUV444P, 10u, VMAFX_PIXEL_FORMAT_P410, 6u},
    {"y210", VMAFX_PIXEL_FORMAT_YUV422P, 10u, VMAFX_PIXEL_FORMAT_Y210, 6u},
    {"y410", VMAFX_PIXEL_FORMAT_YUV444P, 10u, VMAFX_PIXEL_FORMAT_Y410, 0u},
    {"yuv444p10msb", VMAFX_PIXEL_FORMAT_YUV444P, 10u, VMAFX_PIXEL_FORMAT_YUV444P_MSB, 0u},
    {"yuv444p12", VMAFX_PIXEL_FORMAT_YUV444P, 12u, VMAFX_PIXEL_FORMAT_YUV444P, 0u},
    {"y212", VMAFX_PIXEL_FORMAT_YUV422P, 12u, VMAFX_PIXEL_FORMAT_Y212, 4u},
    {"xv36", VMAFX_PIXEL_FORMAT_YUV444P, 12u, VMAFX_PIXEL_FORMAT_XV36, 4u},
    {"yuv444p12msb", VMAFX_PIXEL_FORMAT_YUV444P, 12u, VMAFX_PIXEL_FORMAT_YUV444P_MSB, 0u},
    {"yuv422p16", VMAFX_PIXEL_FORMAT_YUV422P, 16u, VMAFX_PIXEL_FORMAT_YUV422P, 0u},
    {"yuv444p16", VMAFX_PIXEL_FORMAT_YUV444P, 16u, VMAFX_PIXEL_FORMAT_YUV444P, 0u},
    {"p216", VMAFX_PIXEL_FORMAT_YUV422P, 16u, VMAFX_PIXEL_FORMAT_P216, 0u},
    {"p416", VMAFX_PIXEL_FORMAT_YUV444P, 16u, VMAFX_PIXEL_FORMAT_P416, 0u},
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
static inline int vf_attempt(const VfOps *ops, const VtClip *clip, const VfFormat *f,
                             void *held[][2], const VcCell *cell, VfResult *r)
{
    unsigned long compared = 0;
    unsigned long differing = 0;
    VmafxContext *const host = vf_host_session(ops->device, clip, cell);
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

/* Every cell of the table over one format; fills `r`. NULL when every cell
 * scored the same bits, else a message. */
static inline char *vf_run_format(const VfOps *ops, const VfFormat *f, VfResult *r)
{
    VtClip clip;
    if (!vf_clip(f, &clip)) {
        vt_clip_close(&clip);
        return "format clip";
    }
    void *(*held)[2] = calloc(clip.n_frames, sizeof(*held));
    const size_t frame = vt_frame_bytes(&clip.desc);
    bool ok = held != NULL;
    for (unsigned i = 0; i < clip.n_frames && ok; i++) {
        held[i][0] = ops->hold(ops->self, &clip.desc, clip.ref + i * frame, f->pix_fmt, f->shift);
        held[i][1] = ops->hold(ops->self, &clip.desc, clip.dist + i * frame, f->pix_fmt, f->shift);
        ok = held[i][0] && held[i][1];
    }
    char *msg = ok ? NULL : "hold";
    const unsigned cw = (clip.desc.w + 1u) / 2u;
    for (size_t c = 0; c < VF_N_CELLS && !msg; c++) {
        const VcCell *const cell = &VF_CELLS[c];
        const unsigned chroma_w = f->planar_fmt == VMAFX_PIXEL_FORMAT_YUV444P ? clip.desc.w : cw;
        if (cell->min_chroma != 0u &&
            (chroma_w < cell->min_chroma || clip.desc.h < cell->min_chroma)) {
            continue;
        }
        int verdict = -1;
        for (unsigned a = 0; a < VF_ATTEMPTS && verdict != 1; a++) {
            verdict = vf_attempt(ops, &clip, f, held, cell, r);
            if (verdict == -2) {
                (void)fprintf(stderr, "\n  %s cell %s: refused for host and imported frames",
                              f->name, cell->name);
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
        if (verdict == -2) {
            continue;
        }
        r->cells++;
        r->differing += verdict == 1 ? 0u : 1u;
        msg = verdict == 1 ? NULL : (verdict < 0 ? "cell sessions" : "values differ");
    }
    for (unsigned i = 0; i < clip.n_frames && held; i++) {
        if (held[i][0]) {
            ops->release(ops->self, held[i][0]);
        }
        if (held[i][1]) {
            ops->release(ops->self, held[i][1]);
        }
    }
    free(held);
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
