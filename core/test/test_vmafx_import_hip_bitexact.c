/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1829 exit evidence for the HIP lane (RC4 WP3, ADR-2092): frames
 * imported on a HIP device score bit for bit as the same frames uploaded
 * from the host, for every HIP twin declared exact (the fragments
 * scripts/ci/exact_twins.d/<cell>.hip, vmafx_device_cells.h), on the Netflix
 * 576x324 pair, both 1080p checkerboards, the 10-bit Sparks pair and frames
 * of the 3840x2160 testdata/bbb pair.
 *
 * Each clip is imported in three layouts: planar device pointers whose planes
 * start at an odd byte with an odd pitch (bound where they are: the twins
 * read through device copies that take any layout), semi-planar device
 * pointers (NV12, or P010 at 10 bits, planarised on the device), and the
 * semi-planar frame in a Linux dma-buf (both planes in one buffer object of
 * the device's render node, written through its GBM mapping, imported with
 * the dma-buf's sync_file as the acquire fence through
 * vmafx_context_import_frame()). Every value of every feature of every cell
 * at every frame is compared, and the host-copy counter stays 0.
 *
 * Needs a HIP device (77 without one) and the fixtures (each clip that is
 * missing is skipped; a partial Netflix fixture set fails). The dma-buf
 * layout needs libgbm at build time and the render node at run time.
 * VMAFX_TEST_CLIPS=<n>: only fixture clip n (0..3) or 4 for the 4K pair.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_device_cells.h"
#include "vmafx_fixture_util.h"
#include "vmafx_hip_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#ifndef VMAFX_TEST_BBB_DIR
#error "VMAFX_TEST_BBB_DIR: the 4K fixture directory, set by core/test/meson.build"
#endif

/* Frames of the 4K pair compared (of its 200). */
#define BBB_FRAMES 6u
/* Padding after every producer row (odd: odd pitches). */
#define ROW_PAD 37u
/* Bytes the planar planes start past an aligned address. */
#define SKEW 3u
/* Attempts at a cell whose values differ (the platform defect below). */
#define CELL_ATTEMPTS 4u

typedef enum Layout {
    LAYOUT_PLANAR_SKEWED,
    LAYOUT_SEMI,
    LAYOUT_DMABUF_SEMI,
    N_LAYOUTS,
} Layout;

static const char *const layout_names[N_LAYOUTS] = {"planar, odd offsets and pitches",
                                                    "semi-planar", "semi-planar dma-buf"};

static VhGpu gpu;
static bool have_gpu;
#ifdef VMAFX_TEST_HAVE_GBM
static VhGbm gbm;
static bool have_gbm;
#endif
static unsigned long compared;
static unsigned long differing; /* cells that differed in every attempt */
static unsigned long retried_attempts;
static unsigned long retried_values;
static unsigned long cells_run;
static uint64_t imports;

/* ---- Clips ------------------------------------------------------------------------ */

/* The first `frames` frames of the 3840x2160 testdata/bbb pair. */
static bool clip_bbb(VtClip *clip, unsigned frames)
{
    clip->desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, 3840u, 2160u);
    clip->n_frames = frames;
    const size_t bytes = vt_frame_bytes(&clip->desc) * frames;
    static const char *const names[2] = {"ref_3840x2160_200f.yuv", "dis_3840x2160_200f.yuv"};
    uint8_t **const out[2] = {&clip->ref, &clip->dist};
    bool ok = true;
    for (unsigned s = 0; s < 2u; s++) {
        char path[4096];
        (void)snprintf(path, sizeof(path), "%s/%s", VMAFX_TEST_BBB_DIR, names[s]);
        FILE *const file = fopen(path, "rb");
        *out[s] = file ? malloc(bytes) : NULL;
        ok = ok && *out[s] && fread(*out[s], 1, bytes, file) == bytes;
        if (file) {
            (void)fclose(file);
        }
    }
    return ok;
}

/* VMAFX_TEST_CLIPS=<n>: run clip n only (-1: every clip). */
static int clip_filter(void)
{
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    const char *const v = getenv("VMAFX_TEST_CLIPS");
    return v && v[0] >= '0' && v[0] <= '9' ? v[0] - '0' : -1;
}

/* ---- Producer frames ------------------------------------------------------------------ */

/* One side of one frame as the producer holds it. */
typedef struct Held {
    VhPlanes planes;
#ifdef VMAFX_TEST_HAVE_GBM
    VhDmabuf dmabuf;
#endif
} Held;

/* Every frame of both sides of the clip in one layout. */
typedef struct Uploads {
    Held *ref;
    Held *dist;
    Layout layout;
    uint32_t pix_fmt; /* YUV420P, NV12 or P010 */
    unsigned n;
} Uploads;

static uint32_t semi_format(const VtClip *clip)
{
    return clip->desc.bpc == 8u ? VMAFX_PIXEL_FORMAT_NV12 : VMAFX_PIXEL_FORMAT_P010;
}

/* One side of frame `i` into the producer's memory. */
static bool hold(const VtClip *clip, const Uploads *u, const uint8_t *frame, Held *h)
{
    const unsigned shift = u->pix_fmt == VMAFX_PIXEL_FORMAT_P010 ? 6u : 0u;
    switch (u->layout) {
    case LAYOUT_PLANAR_SKEWED:
        return vh_upload_skewed(&gpu, &clip->desc, frame, u->pix_fmt, 0u, ROW_PAD, SKEW,
                                &h->planes);
    case LAYOUT_SEMI:
        return vh_upload(&gpu, &clip->desc, frame, u->pix_fmt, shift, ROW_PAD, &h->planes);
    default:
#ifdef VMAFX_TEST_HAVE_GBM
        return vh_dmabuf_write(&gbm, &clip->desc, frame, u->pix_fmt, shift, ROW_PAD, &h->dmabuf);
#else
        return false;
#endif
    }
}

static void release_held(Held *h)
{
    vh_free(&gpu, &h->planes);
#ifdef VMAFX_TEST_HAVE_GBM
    vh_dmabuf_free(&h->dmabuf);
#endif
}

static bool upload_clip(const VtClip *clip, Layout layout, Uploads *u)
{
    const size_t frame = vt_frame_bytes(&clip->desc);
    u->layout = layout;
    u->pix_fmt = layout == LAYOUT_PLANAR_SKEWED ? VMAFX_PIXEL_FORMAT_YUV420P : semi_format(clip);
    u->n = clip->n_frames;
    u->ref = calloc(clip->n_frames, sizeof(*u->ref));
    u->dist = calloc(clip->n_frames, sizeof(*u->dist));
    bool ok = u->ref && u->dist;
    for (unsigned i = 0; i < clip->n_frames && ok; i++) {
        ok = hold(clip, u, clip->ref + i * frame, &u->ref[i]) &&
             hold(clip, u, clip->dist + i * frame, &u->dist[i]);
    }
    return ok;
}

static void free_uploads(Uploads *u)
{
    for (unsigned i = 0; i < u->n && u->ref && u->dist; i++) {
        release_held(&u->ref[i]);
        release_held(&u->dist[i]);
    }
    free(u->ref);
    free(u->dist);
    memset(u, 0, sizeof(*u));
}

/* ---- Sessions ----------------------------------------------------------------------- */

/* The host session of a cell: frames borrowed from the clip, uploaded by
 * the engine. */
static VmafxContext *run_host(const VtClip *clip, const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
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

/* One side of a frame imported for `context`: a dma-buf with its sync_file
 * through the import rule (waited on the host, D8), device pointers
 * directly. */
static VmafxFrame *import_one(VmafxContext *context, const VtClip *clip, const Uploads *u,
                              const Held *h)
{
    const uint32_t bpc = clip->desc.bpc;
    VmafxFrame *frame = NULL;
#ifdef VMAFX_TEST_HAVE_GBM
    if (u->layout == LAYOUT_DMABUF_SEMI) {
        VmafxFrameImport imp = vh_dmabuf_desc(&clip->desc, u->pix_fmt, bpc, &h->dmabuf);
        imp.acquire.kind = VMAFX_FENCE_SYNC_FILE;
        imp.acquire.fd = vh_dmabuf_sync_file(&h->dmabuf);
        const VmafxStatus status =
            vmafx_context_import_frame(context, gpu.device, &imp, "main", &frame, NULL);
        if (imp.acquire.fd >= 0) {
            (void)close(imp.acquire.fd);
        }
        imports += status == VMAFX_OK;
        return status == VMAFX_OK ? frame : NULL;
    }
#else
    (void)context;
#endif
    const VmafxFrameImport imp = vh_import_desc(&clip->desc, u->pix_fmt, bpc, &h->planes);
    if (vmafx_frame_import(gpu.device, &imp, &frame, NULL) != VMAFX_OK) {
        return NULL;
    }
    imports++;
    return frame;
}

/* The import session of a cell over the producer's frames. */
static VmafxContext *run_import(const VtClip *clip, const Uploads *u, const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < u->n && ok; i++) {
        VmafxFrame *const ref = import_one(context, clip, u, &u->ref[i]);
        VmafxFrame *const dist = import_one(context, clip, u, &u->dist[i]);
        if (!ref || !dist) {
            /* vmafx_submit() consumes both references; not reached here. */
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

/* ---- Comparison ------------------------------------------------------------------------ */

static bool cell_applies(const VtClip *clip, const VcCell *cell)
{
    const unsigned cw = (clip->desc.w + 1u) / 2u;
    const unsigned ch = (clip->desc.h + 1u) / 2u;
    return cell->min_chroma == 0u || (cw >= cell->min_chroma && ch >= cell->min_chroma);
}

/* VMAFX_TEST_IMPORT_ONLY=1: import sessions only, nothing compared, for a
 * profiler trace that shows the import's copies alone (the host sessions
 * upload every frame by design). */
static bool import_only(void)
{
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    const char *const v = getenv("VMAFX_TEST_IMPORT_ONLY");
    return v && v[0] == '1';
}

static char *import_cell_only(const VtClip *clip, const Uploads *u, const VcCell *cell)
{
    VmafxContext *const imp = run_import(clip, u, cell);
    mu_assert("import session", imp != NULL);
    mu_assert("destroy", vmafx_context_destroy(imp, NULL) == VMAFX_OK);
    cells_run++;
    return NULL;
}

/* One attempt at a cell: a host session and an import session, compared.
 * 1: bit-identical, 0: values differ, -1: a session failed. */
static int attempt_cell(const VtClip *clip, const Uploads *u, const VcCell *cell,
                        unsigned long *n_compared, unsigned long *n_differing)
{
    VmafxContext *const host = run_host(clip, cell);
    VmafxContext *const imp = host ? run_import(clip, u, cell) : NULL;
    const bool same = host && imp && vc_compare(host, imp, clip->n_frames, n_compared, n_differing);
    const bool destroyed = (!host || vmafx_context_destroy(host, NULL) == VMAFX_OK) &&
                           (!imp || vmafx_context_destroy(imp, NULL) == VMAFX_OK);
    if (!same || !destroyed) {
        return -1;
    }
    return *n_differing == 0u ? 1 : 0;
}

/* Every value of the cell bit-identical. The gfx1036 this lane is verified
 * on now and then never runs a run of a stream's commands (a lost
 * accumulator clear: one wrong frame in roughly 10^3 to 10^4 under host
 * load, host frames included, T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01),
 * so a cell whose attempt differs is run again, up to CELL_ATTEMPTS times,
 * and every such attempt is counted and reported. A defect of the import
 * (a wrong conversion, a missing wait) differs in every attempt and
 * fails. */
static char *compare_cell(const VtClip *clip, const Uploads *u, const VcCell *cell)
{
    if (import_only()) {
        return import_cell_only(clip, u, cell);
    }
    int verdict = -1;
    for (unsigned attempt = 0; attempt < CELL_ATTEMPTS && verdict != 1; attempt++) {
        unsigned long n_compared = 0;
        unsigned long n_differing = 0;
        verdict = attempt_cell(clip, u, cell, &n_compared, &n_differing);
        if (verdict == 1) {
            compared += n_compared;
            break;
        }
        retried_values += n_differing;
        retried_attempts++;
        (void)fprintf(stderr, "\n  cell %s, %ux%u %u-bit, %s, attempt %u: %s (%lu differing)\n",
                      cell->name, clip->desc.w, clip->desc.h, clip->desc.bpc,
                      layout_names[u->layout], attempt + 1u,
                      verdict == 0 ? "values differ" : "session failed", n_differing);
        if (verdict < 0) {
            break;
        }
    }
    differing += verdict == 1 ? 0u : 1u;
    mu_assert("cell sessions", verdict >= 0);
    mu_assert("bit-identical in one of the attempts", verdict == 1);
    cells_run++;
    return NULL;
}

/* Every cell over one clip in one layout. */
static char *compare_layout(const VtClip *clip, Layout layout)
{
    Uploads u;
    memset(&u, 0, sizeof(u));
    char *msg = upload_clip(clip, layout, &u) ? NULL : "upload";
    for (size_t c = 0; c < VC_N_CELLS && !msg; c++) {
        msg = cell_applies(clip, &vc_cells[c]) ? compare_cell(clip, &u, &vc_cells[c]) : NULL;
    }
    free_uploads(&u);
    return msg;
}

static char *compare_clip(const VtClip *clip)
{
    for (unsigned l = 0; l < N_LAYOUTS; l++) {
#ifdef VMAFX_TEST_HAVE_GBM
        if (l == LAYOUT_DMABUF_SEMI && !have_gbm) {
            continue;
        }
#else
        if (l == LAYOUT_DMABUF_SEMI) {
            continue;
        }
#endif
        mu_assert_msg(compare_layout(clip, (Layout)l));
    }
    return NULL;
}

static char *test_fixtures(void)
{
    if (!have_gpu) {
        return NULL;
    }
    const int only = clip_filter();
    unsigned present = 0;
    unsigned wanted = 0;
    for (size_t i = 0; i < VT_N_INPUTS; i++) {
        if (only >= 0 && (size_t)only != i) {
            continue;
        }
        wanted++;
        VtClip clip;
        memset(&clip, 0, sizeof(clip));
        const bool open = vt_clip_open(&clip, &vt_inputs[i]);
        char *const msg = open ? compare_clip(&clip) : NULL;
        vt_clip_close(&clip);
        mu_assert_msg(msg);
        present += open ? 1u : 0u;
    }
    (void)fprintf(stderr, "[%u fixture clips] ", present);
    mu_assert("every Netflix input present or none", present == 0u || present == wanted);
    return NULL;
}

static char *test_bbb_4k(void)
{
    const int only = clip_filter();
    if (!have_gpu || (only >= 0 && (size_t)only != VT_N_INPUTS)) {
        return NULL;
    }
    VtClip clip;
    memset(&clip, 0, sizeof(clip));
    char *const msg = clip_bbb(&clip, BBB_FRAMES) ? compare_clip(&clip) : NULL;
    vt_clip_close(&clip);
    return msg;
}

static char *test_counters(void)
{
    if (!have_gpu) {
        mu_skipped = 1;
        return NULL;
    }
    (void)fprintf(stderr,
                  "[%lu cells, %lu values, %lu cells differing in every attempt, %lu attempts "
                  "rerun (%lu values), %llu imports, %llu conversions, %llu host copies] ",
                  cells_run, compared, differing, retried_attempts, retried_values,
                  (unsigned long long)imports, (unsigned long long)vmafx_test_conversions(),
                  (unsigned long long)vmafx_test_host_copies());
    mu_assert("cells ran", cells_run > 0u && (compared > 0u || import_only()));
    mu_assert("no host copy of an imported frame", vmafx_test_host_copies() == 0u);
    mu_assert("semi-planar imports converted on the device", vmafx_test_conversions() > 0u);
    return NULL;
}

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vh_open(&gpu);
#ifdef VMAFX_TEST_HAVE_GBM
    have_gbm = have_gpu && vh_gbm_open(&gpu, &gbm);
    (void)fprintf(stderr, "[dma-buf layout %s] ", have_gbm ? "on" : "off (no render node)");
#endif
    static const MuTest tests[] = {
        MU_TEST(test_fixtures),
        MU_TEST(test_bbb_4k),
        MU_TEST(test_counters),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
#ifdef VMAFX_TEST_HAVE_GBM
    if (have_gbm) {
        vh_gbm_close(&gbm);
    }
#endif
    vh_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
