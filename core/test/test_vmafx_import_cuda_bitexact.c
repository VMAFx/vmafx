/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1829 exit evidence for the CUDA lane (RC4 WP3, ADR-2023): frames
 * imported as CUDA device pointers score bit for bit as the same frames
 * uploaded from the host, for every CUDA twin declared exact
 * (the fragments scripts/ci/exact_twins.d/<cell>.cuda, vmafx_cuda_cells.h), on the Netflix
 * 576x324 pair, both 1080p checkerboards, the 10-bit Sparks pair and frames
 * of the 3840x2160 testdata/bbb pair.
 *
 * Each clip is imported twice: planar (YUV420P, the producer's planes bound
 * with no copy) and semi-planar (NV12, or P010 at 10 bits, planarised on the
 * device). The producer's planes start at an odd byte (8-bit) and every row
 * carries padding, so no alignment of the producer's memory is assumed.
 * Every value of every feature of every cell at every frame is compared,
 * and the host-copy counter stays 0.
 *
 * Needs a CUDA device (77 without one) and the fixtures (each clip that is
 * missing is skipped; a partial Netflix fixture set fails).
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
#include "vmafx_cuda_cells.h"
#include "vmafx_cuda_test_util.h"
#include "vmafx_fixture_util.h"
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
/* Padding after every producer row. */
#define ROW_PAD 37u

static VcGpu gpu;
static bool have_gpu;
static unsigned long compared;
static unsigned long differing;
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

/* ---- Sessions ----------------------------------------------------------------------- */

/* Device planes of every frame of one side of the clip in `layout`. */
typedef struct Uploads {
    VcPlanes *ref;
    VcPlanes *dist;
    uint32_t pix_fmt; /* YUV420P, NV12 or P010 */
    unsigned n;
} Uploads;

static bool upload_clip(const VtClip *clip, uint32_t pix_fmt, Uploads *u)
{
    const size_t frame = vt_frame_bytes(&clip->desc);
    const unsigned shift = pix_fmt == VMAFX_PIXEL_FORMAT_P010 ? 6u : 0u;
    u->pix_fmt = pix_fmt;
    u->n = clip->n_frames;
    u->ref = calloc(clip->n_frames, sizeof(*u->ref));
    u->dist = calloc(clip->n_frames, sizeof(*u->dist));
    bool ok = u->ref && u->dist;
    for (unsigned i = 0; i < clip->n_frames && ok; i++) {
        ok = vc_upload(&gpu, &clip->desc, clip->ref + i * frame, pix_fmt, shift, ROW_PAD,
                       &u->ref[i]) &&
             vc_upload(&gpu, &clip->desc, clip->dist + i * frame, pix_fmt, shift, ROW_PAD,
                       &u->dist[i]);
    }
    return ok && vc_push(&gpu) && gpu.f->cuStreamSynchronize(gpu.producer) == CUDA_SUCCESS &&
           (vc_pop(&gpu), true);
}

static void free_uploads(Uploads *u)
{
    for (unsigned i = 0; i < u->n && u->ref && u->dist; i++) {
        vc_free(&gpu, &u->ref[i]);
        vc_free(&gpu, &u->dist[i]);
    }
    free(u->ref);
    free(u->dist);
    memset(u, 0, sizeof(*u));
}

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

static VmafxFrame *import_one(const VtClip *clip, const Uploads *u, const VcPlanes *p)
{
    const uint32_t bpc = clip->desc.bpc;
    const VmafxFrameImport imp = vc_import_desc(&clip->desc, u->pix_fmt, bpc, p);
    VmafxFrame *frame = NULL;
    if (vmafx_frame_import(gpu.device, &imp, &frame, NULL) != VMAFX_OK) {
        return NULL;
    }
    imports++;
    return frame;
}

/* The import session of a cell over the uploaded frames. */
static VmafxContext *run_import(const VtClip *clip, const Uploads *u, const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < u->n && ok; i++) {
        VmafxFrame *const ref = import_one(clip, u, &u->ref[i]);
        VmafxFrame *const dist = import_one(clip, u, &u->dist[i]);
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

static char *compare_cell(const VtClip *clip, const Uploads *u, const VcCell *cell)
{
    if (import_only()) {
        return import_cell_only(clip, u, cell);
    }
    VmafxContext *const host = run_host(clip, cell);
    VmafxContext *const imp = host ? run_import(clip, u, cell) : NULL;
    const unsigned long before = differing;
    const bool same = host && imp && vc_compare(host, imp, clip->n_frames, &compared, &differing);
    const bool destroyed = (!host || vmafx_context_destroy(host, NULL) == VMAFX_OK) &&
                           (!imp || vmafx_context_destroy(imp, NULL) == VMAFX_OK);
    if (!same || differing != before) {
        (void)fprintf(stderr, "\n  cell %s, %ux%u %u-bit, layout %u: %s (%lu differing)\n",
                      cell->name, clip->desc.w, clip->desc.h, clip->desc.bpc, u->pix_fmt,
                      host && imp ? "values differ" : "session failed", differing - before);
    }
    mu_assert("cell sessions", host && imp && same);
    mu_assert("bit-identical", differing == before);
    mu_assert("destroy", destroyed);
    cells_run++;
    return NULL;
}

/* Every cell over one clip in one layout. */
static char *compare_layout(const VtClip *clip, uint32_t pix_fmt)
{
    Uploads u;
    memset(&u, 0, sizeof(u));
    char *msg = upload_clip(clip, pix_fmt, &u) ? NULL : "upload";
    for (size_t c = 0; c < VC_N_CELLS && !msg; c++) {
        msg = cell_applies(clip, &vc_cells[c]) ? compare_cell(clip, &u, &vc_cells[c]) : NULL;
    }
    free_uploads(&u);
    return msg;
}

static char *compare_clip(const VtClip *clip)
{
    const uint32_t semi = clip->desc.bpc == 8u ? VMAFX_PIXEL_FORMAT_NV12 : VMAFX_PIXEL_FORMAT_P010;
    mu_assert_msg(compare_layout(clip, VMAFX_PIXEL_FORMAT_YUV420P));
    mu_assert_msg(compare_layout(clip, semi));
    return NULL;
}

static char *test_fixtures(void)
{
    if (!have_gpu) {
        return NULL;
    }
    unsigned present = 0;
    for (size_t i = 0; i < VT_N_INPUTS; i++) {
        VtClip clip;
        memset(&clip, 0, sizeof(clip));
        const bool open = vt_clip_open(&clip, &vt_inputs[i]);
        char *const msg = open ? compare_clip(&clip) : NULL;
        vt_clip_close(&clip);
        mu_assert_msg(msg);
        present += open ? 1u : 0u;
    }
    (void)fprintf(stderr, "[%u fixture clips] ", present);
    mu_assert("every Netflix input present or none", present == 0u || present == VT_N_INPUTS);
    return NULL;
}

static char *test_bbb_4k(void)
{
    if (!have_gpu) {
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
    (void)fprintf(stderr, "[%lu cells, %lu values, %lu differing, %llu imports, %llu conversions] ",
                  cells_run, compared, differing, (unsigned long long)imports,
                  (unsigned long long)vmafx_test_conversions());
    mu_assert("cells ran", cells_run > 0u && (compared > 0u || import_only()));
    mu_assert("no host copy of an imported frame", vmafx_test_host_copies() == 0u);
    mu_assert("semi-planar imports converted on the device", vmafx_test_conversions() > 0u);
    return NULL;
}

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vc_open(&gpu);
    static const MuTest tests[] = {
        MU_TEST(test_fixtures),
        MU_TEST(test_bbb_4k),
        MU_TEST(test_counters),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    vc_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
