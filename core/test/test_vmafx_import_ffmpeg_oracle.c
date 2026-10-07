/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Every input layout FFmpeg writes, imported from host memory and compared
 * with the planar frame FFmpeg started from (RC4 WP13, ADR-2145). The bytes
 * are in vmafx_format_fixtures.h, made by scripts/dev/gen_format_fixtures.py
 * with FFmpeg: where a sample lives in NV12 .. P416, UYVY, YUY2, V210, Y210,
 * Y212, AYUV, VUYX, XV30, XV36 and the MSB planar words is FFmpeg's answer,
 * not this code's. Positive: the frame an import makes equals the planar
 * frame sample for sample, at the depth and chroma layout of the table row.
 * Negative: a plane one row short is refused naming the plane; a bit depth
 * the layout does not hold is refused naming the field. Boundary: a width
 * that is not a multiple of the V210 group, odd chroma rows (4:2:0, 5 rows).
 */

#include <stdint.h>
#include <string.h>

#include "mu_table.h"
#include "picture.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_fixture_util.h"
#include "vmafx_format_fixtures.h"
#include "vmafx_test_util.h"
#include "vmafx/internal.h"
#include "vmafx/import_layouts_gen.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

typedef struct Geometry {
    unsigned pw[3];
    unsigned ph[3];
} Geometry;

static void planar_geometry(uint32_t planar_fmt, Geometry *g)
{
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(planar_fmt), VFX_WIDTH, VFX_HEIGHT, g->pw,
                               g->ph);
}

/* The import descriptor of a fixture: plane 0 at the start with the
 * fixture's pitch, the rest tightly packed after it. */
static VmafxFrameImport describe(const VfxFixture *f, const VmafxImportLayout *layout,
                                 const Geometry *g)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_HOST;
    imp.pix_fmt = f->pix_fmt;
    imp.bpc = f->bpc;
    imp.w = VFX_WIDTH;
    imp.h = VFX_HEIGHT;
    imp.n_planes = layout->n_planes;
    size_t offset = 0;
    for (uint32_t i = 0; i < layout->n_planes; i++) {
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(layout, f->bpc, i, g->pw, g->ph, &row, &rows);
        const size_t pitch = i == 0u ? f->pitch : (size_t)row;
        imp.plane[i].handle = (uintptr_t)(f->bytes + offset);
        imp.plane[i].pitch = pitch;
        offset += pitch * (size_t)rows;
    }
    return imp;
}

static const VmafxImportLayout *layout_of(uint32_t pix_fmt)
{
    for (size_t i = 0; i < VMAFX_N_IMPORT_LAYOUTS; i++) {
        if (vmafx_import_layouts[i].pix_fmt == pix_fmt) {
            return &vmafx_import_layouts[i];
        }
    }
    return NULL;
}

/* Planes of `frame` against the planar samples `want`; 0 or the plane + 1 that differs. */
static unsigned compare_planes(const VmafxFrame *frame, const uint8_t *want, const Geometry *g,
                               uint32_t bpc)
{
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    if (vmafx_frame_planes(frame, &planes, NULL) != VMAFX_OK) {
        return 4u;
    }
    const size_t bytes = bpc > 8u ? 2u : 1u;
    for (unsigned i = 0; i < 3u; i++) {
        const uint8_t *const data = planes.data[i];
        for (unsigned y = 0; y < g->ph[i]; y++) {
            const size_t row = (size_t)g->pw[i] * bytes;
            if (memcmp(data + (size_t)y * planes.stride[i], want + (size_t)y * row, row) != 0) {
                return i + 1u;
            }
        }
        want += (size_t)g->pw[i] * g->ph[i] * bytes;
    }
    return 0u;
}

/* Positive and boundary: every fixture, imported, is FFmpeg's planar frame. */
static char *test_every_layout_is_ffmpegs(void)
{
    for (size_t k = 0; k < VFX_N_FIXTURES; k++) {
        const VfxFixture *const f = &vfx_fixtures[k];
        const VmafxImportLayout *const layout = layout_of(f->pix_fmt);
        mu_assert("the fixture's layout is in the table", layout != NULL);
        Geometry g;
        planar_geometry(layout->planar_fmt, &g);
        const VmafxFrameImport imp = describe(f, layout, &g);
        VmafxFrame *frame = NULL;
        VmafxError *error = NULL;
        const VmafxStatus status = vmafx_frame_import(NULL, &imp, &frame, &error);
        vmafx_error_free(error);
        mu_assert("the import succeeds", status == VMAFX_OK && frame != NULL);
        const unsigned bad = compare_planes(frame, f->planar, &g, f->bpc);
        vmafx_frame_unref(frame);
        if (bad != 0u) {
            (void)fprintf(stderr, "\n  %s: plane %u differs from FFmpeg's frame", f->ffmpeg_name,
                          bad - 1u);
        }
        mu_assert("every plane equals FFmpeg's planar frame", bad == 0u);
    }
    return NULL;
}

/* Negative: the last row missing from a plane is refused naming it. */
static char *test_short_plane_is_refused(void)
{
    for (size_t k = 0; k < VFX_N_FIXTURES; k++) {
        const VfxFixture *const f = &vfx_fixtures[k];
        const VmafxImportLayout *const layout = layout_of(f->pix_fmt);
        Geometry g;
        planar_geometry(layout->planar_fmt, &g);
        VmafxFrameImport imp = describe(f, layout, &g);
        const uint32_t last = layout->n_planes - 1u;
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(layout, f->bpc, last, g.pw, g.ph, &row, &rows);
        imp.plane[last].size = imp.plane[last].pitch * (rows - 1u) + row - 1u;
        VmafxFrame *frame = NULL;
        VmafxError *error = NULL;
        const VmafxStatus status = vmafx_frame_import(NULL, &imp, &frame, &error);
        mu_assert("a plane one byte short is refused", status == VMAFX_E_RANGE && frame == NULL);
        vmafx_error_free(error);
    }
    return NULL;
}

/* Negative: a depth the layout does not hold is refused naming `desc.bpc`. */
static char *test_wrong_depth_is_refused(void)
{
    for (size_t k = 0; k < VFX_N_FIXTURES; k++) {
        const VfxFixture *const f = &vfx_fixtures[k];
        const VmafxImportLayout *const layout = layout_of(f->pix_fmt);
        if (layout->bpc_min == 8u && layout->bpc_max == 16u) {
            continue;
        }
        Geometry g;
        planar_geometry(layout->planar_fmt, &g);
        VmafxFrameImport imp = describe(f, layout, &g);
        imp.bpc = layout->bpc_max == 16u ? layout->bpc_min - 1u : layout->bpc_max + 1u;
        VmafxFrame *frame = NULL;
        VmafxError *error = NULL;
        const VmafxStatus status = vmafx_frame_import(NULL, &imp, &frame, &error);
        mu_assert("a depth outside the row is refused", status == VMAFX_E_INVALID && frame == NULL);
        mu_assert("the error names desc.bpc",
                  error && strcmp(vmafx_error_subject(error), "desc.bpc") == 0);
        vmafx_error_free(error);
    }
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_every_layout_is_ffmpegs),
        MU_TEST(test_short_plane_is_refused),
        MU_TEST(test_wrong_depth_is_refused),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
