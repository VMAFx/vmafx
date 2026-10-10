/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1829 exit evidence for the Vulkan import lane (RC4 WP3): frames a
 * Vulkan producer wrote into exportable memory and imported as
 * VMAFX_MEMORY_VULKAN score bit for bit as the same frames uploaded from the
 * host, for every twin declared exact on the lane's backend (the cells of
 * vmafx_device_cells.h or vmafx_sycl_cells.h), on the Netflix 576x324 pair,
 * both 1080p checkerboards, the 10-bit Sparks pair and frames of the
 * 3840x2160 testdata/bbb pair.
 *
 * Built once per lane (VMAFX_VK_LANE, vmafx_vulkan_test_util.h): each clip is
 * imported in each of the lane's layouts, planar (YUV420P) and semi-planar
 * (NV12, or P010 at 10 bits, planarised on the device). Every value of every
 * feature of every cell at every frame is compared, and the host-copy counter
 * stays 0.
 *
 * Needs the lane's device and a Vulkan driver on the same GPU (77 without
 * one) and the fixtures (each clip that is missing is skipped; a partial
 * Netflix fixture set fails). VMAFX_TEST_ONLY=netflix|bbb runs one half;
 * VMAFX_TEST_IMPORT_ONLY=1 runs the import sessions alone, for a profiler
 * trace.
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
#include "vmafx_vulkan_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#ifndef VMAFX_TEST_BBB_DIR
#error "VMAFX_TEST_BBB_DIR: the 4K fixture directory, set by core/test/meson.build"
#endif

/* Frames of the 4K pair compared (of its 200). */
#define BBB_FRAMES 6u
/* Attempts at a cell: on the gfx1036 the platform now and then drops a run of
 * a stream's commands (T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01), so a cell
 * that differs is run again there and every repeat is printed; a defect of
 * the import differs in every attempt. One attempt elsewhere. */
#define CELL_ATTEMPTS (VMAFX_VK_LANE == 4 ? 4u : 1u)

static VkGpu gpu;
static bool have_gpu;
static unsigned long compared;
static unsigned long differing; /* cells that differed in every attempt */
static unsigned long cells_run;
static unsigned long repeated; /* attempts that differed and were run again */
static uint64_t imports;

/* ---- Clips ------------------------------------------------------------------------ */

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

static bool run_half(const char *half)
{
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    const char *const only = getenv("VMAFX_TEST_ONLY");
    return !only || strcmp(only, half) == 0;
}

/* ---- Producer frames ------------------------------------------------------------------ */

/* Both sides of a clip in Vulkan memory. */
typedef struct Uploads {
    VkFrame *ref;
    VkFrame *dist;
    unsigned n;
    uint32_t pix_fmt;
    const VkLayoutCase *lay;
} Uploads;

static void free_uploads(Uploads *u)
{
    for (unsigned i = 0; i < u->n && u->ref && u->dist; i++) {
        vk_frame_free(&u->ref[i]);
        vk_frame_free(&u->dist[i]);
    }
    free(u->ref);
    free(u->dist);
    memset(u, 0, sizeof(*u));
}

static bool upload_clip(const VtClip *clip, uint32_t pix_fmt, const VkLayoutCase *lay, Uploads *u)
{
    const size_t frame = vt_frame_bytes(&clip->desc);
    u->pix_fmt = pix_fmt;
    u->lay = lay;
    u->ref = calloc(clip->n_frames, sizeof(*u->ref));
    u->dist = calloc(clip->n_frames, sizeof(*u->dist));
    bool ok = u->ref && u->dist;
    for (unsigned i = 0; i < clip->n_frames && ok; i++) {
        u->n = i + 1u;
        ok = vk_frame_make(&gpu, &clip->desc, clip->ref + i * frame, pix_fmt, lay, &u->ref[i]) &&
             vk_frame_make(&gpu, &clip->desc, clip->dist + i * frame, pix_fmt, lay, &u->dist[i]);
    }
    return ok;
}

/* ---- Sessions --------------------------------------------------------------------------- */

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

static VmafxContext *run_import(const Uploads *u, const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < u->n && ok; i++) {
        VmafxFrame *const ref = vk_import(&gpu, context, &u->ref[i]);
        VmafxFrame *const dist = vk_import(&gpu, context, &u->dist[i]);
        imports += (ref != NULL) + (dist != NULL);
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

/* One attempt: 1 bit-identical, 0 values differ, -1 a session failed. */
static int attempt_cell(const VtClip *clip, const Uploads *u, const VcCell *cell,
                        unsigned long *n_compared, unsigned long *n_differing)
{
    VmafxContext *const host = run_host(clip, cell);
    VmafxContext *const imp = host ? run_import(u, cell) : NULL;
    const bool same = host && imp && vc_compare(host, imp, clip->n_frames, n_compared, n_differing);
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    if (imp) {
        (void)vmafx_context_destroy(imp, NULL);
    }
    return !same ? -1 : *n_differing == 0u ? 1 : 0;
}

/* VMAFX_TEST_IMPORT_ONLY=1: the import session alone (vc_import_only()). */
static char *import_cell_only(const Uploads *u, const VcCell *cell)
{
    VmafxContext *const imp = run_import(u, cell);
    mu_assert("import session", imp != NULL);
    mu_assert("destroy", vmafx_context_destroy(imp, NULL) == VMAFX_OK);
    cells_run++;
    return NULL;
}

static char *compare_cell(const VtClip *clip, const Uploads *u, const VcCell *cell)
{
    if (vc_import_only()) {
        return import_cell_only(u, cell);
    }
    int verdict = -1;
    for (unsigned attempt = 0; attempt < CELL_ATTEMPTS; attempt++) {
        unsigned long n_compared = 0;
        unsigned long n_differing = 0;
        verdict = attempt_cell(clip, u, cell, &n_compared, &n_differing);
        if (verdict == 1) {
            compared += n_compared;
            break;
        }
        (void)fprintf(
            stderr, "\n  cell %s, %ux%u %u-bit, %s as %u, attempt %u: %s (%lu differing)\n",
            cell->name, clip->desc.w, clip->desc.h, clip->desc.bpc, u->lay->name, u->pix_fmt,
            attempt + 1u, verdict == 0 ? "values differ" : "session failed", n_differing);
        repeated += attempt + 1u < CELL_ATTEMPTS ? 1u : 0u;
        if (verdict < 0) {
            break;
        }
    }
    differing += verdict == 1 ? 0u : 1u;
    mu_assert("cell sessions", verdict >= 0);
    mu_assert("bit-identical (in one of the attempts on the gfx1036)", verdict == 1);
    cells_run++;
    return NULL;
}

/* Every cell over one clip in one layout and pixel format. */
static char *compare_layout(const VtClip *clip, uint32_t pix_fmt, const VkLayoutCase *lay)
{
    Uploads u;
    memset(&u, 0, sizeof(u));
    char *msg = upload_clip(clip, pix_fmt, lay, &u) ? NULL : "upload";
    for (size_t c = 0; c < VK_N_CELLS && !msg; c++) {
        msg = cell_applies(clip, &VK_CELLS[c]) ? compare_cell(clip, &u, &VK_CELLS[c]) : NULL;
    }
    free_uploads(&u);
    return msg;
}

static char *compare_clip(const VtClip *clip)
{
    const uint32_t semi = clip->desc.bpc == 8u ? VMAFX_PIXEL_FORMAT_NV12 : VMAFX_PIXEL_FORMAT_P010;
    for (size_t l = 0; l < VK_N_LAYOUTS; l++) {
        mu_assert_msg(compare_layout(clip, VMAFX_PIXEL_FORMAT_YUV420P, &vk_layouts[l]));
        mu_assert_msg(compare_layout(clip, semi, &vk_layouts[l]));
    }
    return NULL;
}

static char *test_fixtures(void)
{
    if (!have_gpu || !run_half("netflix")) {
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
    if (!have_gpu || !run_half("bbb")) {
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
                  "[%lu cells, %lu values, %lu cells differing, %lu repeated attempts, %llu "
                  "imports, %llu conversions] ",
                  cells_run, compared, differing, repeated, (unsigned long long)imports,
                  (unsigned long long)vmafx_test_conversions());
    mu_assert("cells ran", cells_run > 0u && (compared > 0u || vc_import_only()));
    mu_assert("no host copy of an imported frame", vmafx_test_host_copies() == 0u);
    mu_assert("semi-planar imports converted on the device", vmafx_test_conversions() > 0u);
    return NULL;
}

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vk_open(&gpu);
    mu_skipped = !have_gpu; /* no case runs, so none reports a pass (Q-346) */
    static const MuTest tests[] = {
        MU_TEST(test_fixtures),
        MU_TEST(test_bbb_4k),
        MU_TEST(test_counters),
    };
    char *const msg = have_gpu ? mu_run_table(tests, MU_TABLE_LEN(tests)) : NULL;
    vk_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
