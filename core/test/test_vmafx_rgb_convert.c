/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The RGB input conversion (RC4 WP13, ADR-2146) against an exact-rational
 * oracle (vmafx_rgb_oracle.h, scripts/dev/gen_rgb_oracle.py) and its refusals.
 *
 * Positive: for every matrix, input range, output range and bit depth of the
 * oracle, the plan's Y', Cb', Cr' of each pixel equal the oracle's (within
 * one step where the exact value is next to a rounding tie); the same pixels
 * imported as RGB, RGBA and BGRA from host memory make the same frame;
 * R = G = B maps to Cb' = Cr' = mid at every code value; the alpha sample is
 * not read. Negative: a missing matrix, input range, transfer or output range
 * is refused naming the field and the format, BT.2020 constant luminance,
 * ICtCp and linear light are refused as unsupported, a value that is not an
 * enumerator is invalid; a layout that is not RGB ignores the statement.
 * Boundary: the code range edges and the limited-range black and white of every
 * depth are in the oracle's pixels.
 */

#include <stdint.h>
#include <string.h>

#include "mu_table.h"
#include "picture.h"
#include "test.h"
#include "vmafx/import_layouts_gen.h"
#include "vmafx/internal.h"
#include "vmafx/rgb_convert.h"
#include "vmafx/vmafx.h"
#include "vmafx_rgb_oracle.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

static const VmafxImportLayout *layout_of(uint32_t pix_fmt)
{
    for (size_t i = 0; i < VMAFX_N_IMPORT_LAYOUTS; i++) {
        if (vmafx_import_layouts[i].pix_fmt == pix_fmt) {
            return &vmafx_import_layouts[i];
        }
    }
    return NULL;
}

/* |a - b| <= 1 when `slack`, else equal. */
static bool matches(uint32_t got, uint32_t want, bool slack)
{
    return got == want || (slack && (got + 1u == want || want + 1u == got));
}

/* Positive: the plan evaluates the oracle. */
static char *test_plan_matches_the_oracle(void)
{
    const VmafxImportLayout *const rgba = layout_of(VMAFX_PIXEL_FORMAT_RGBA);
    mu_assert("RGBA is in the table", rgba != NULL);
    unsigned checked = 0;
    unsigned slack_used = 0;
    for (size_t c = 0; c < VRO_N_CONFIGS; c++) {
        const VroConfig *const cfg = &vro_configs[c];
        VmafxRgbPlan plan;
        mu_assert(
            "the plan of a supported matrix",
            vmafx_rgb_plan_init(&plan, rgba, cfg->bpc, cfg->matrix, cfg->in_range, cfg->out_range));
        for (size_t i = 0; i < cfg->n; i++) {
            const VroCase *const px = &cfg->cases[i];
            for (uint32_t plane = 0; plane < 3u; plane++) {
                const uint32_t want = plane == 0u ? px->y : (plane == 1u ? px->cb : px->cr);
                const bool slack = (px->slack >> plane) & 1u;
                const uint32_t got = vmafx_rgb_value(&plan, plane, px->r, px->g, px->b);
                if (!matches(got, want, slack)) {
                    (void)fprintf(stderr,
                                  "\n  matrix %u in %u out %u bpc %u rgb (%u,%u,%u) plane %u: got "
                                  "%u, oracle %u",
                                  (unsigned)cfg->matrix, (unsigned)cfg->in_range,
                                  (unsigned)cfg->out_range, (unsigned)cfg->bpc, px->r, px->g, px->b,
                                  (unsigned)plane, got, want);
                }
                mu_assert("the conversion equals the oracle", matches(got, want, slack));
                slack_used += got != want;
                checked++;
            }
        }
    }
    mu_assert("the oracle was read", checked > 3000u);
    mu_assert("a tie that moved is rare", slack_used * 100u < checked);
    return NULL;
}

/* R = G = B: Cb' and Cr' are exactly mid, at every code of 8 and 10 bits and
 * on a stride of the 16-bit range, whatever the matrix and range. */
static char *test_gray_has_no_chroma(void)
{
    const VmafxImportLayout *const rgba = layout_of(VMAFX_PIXEL_FORMAT_RGBA);
    static const uint32_t depths[3] = {8u, 10u, 16u};
    static const uint32_t matrices[3] = {VMAFX_COLOR_MATRIX_BT601, VMAFX_COLOR_MATRIX_BT709,
                                         VMAFX_COLOR_MATRIX_BT2020_NCL};
    for (unsigned mi = 0; mi < 3u; mi++) {
        const uint32_t m = matrices[mi];
        for (uint32_t rin = VMAFX_COLOR_RANGE_LIMITED; rin <= VMAFX_COLOR_RANGE_FULL; rin++) {
            for (uint32_t rout = VMAFX_COLOR_RANGE_LIMITED; rout <= VMAFX_COLOR_RANGE_FULL;
                 rout++) {
                for (unsigned d = 0; d < 3u; d++) {
                    VmafxRgbPlan plan;
                    mu_assert("plan", vmafx_rgb_plan_init(&plan, rgba, depths[d], m, rin, rout));
                    const uint32_t mid = 1u << (depths[d] - 1u);
                    const uint32_t step = depths[d] == 16u ? 251u : 1u;
                    for (uint32_t v = 0; v <= plan.max; v += step) {
                        mu_assert("Cb' of a gray is mid",
                                  vmafx_rgb_value(&plan, 1u, v, v, v) == mid);
                        mu_assert("Cr' of a gray is mid",
                                  vmafx_rgb_value(&plan, 2u, v, v, v) == mid);
                    }
                }
            }
        }
    }
    return NULL;
}

/* Description of an RGB host frame: `n` pixels of one row. */
static VmafxFrameImport describe(uint32_t pix_fmt, uint32_t bpc, unsigned n, const uint8_t *data,
                                 size_t pitch)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_HOST;
    imp.pix_fmt = pix_fmt;
    imp.bpc = bpc;
    imp.w = n;
    imp.h = 1u;
    imp.n_planes = 1u;
    imp.plane[0].handle = (uintptr_t)data;
    imp.plane[0].pitch = pitch;
    imp.rgb_matrix = VMAFX_COLOR_MATRIX_BT709;
    imp.rgb_range = VMAFX_COLOR_RANGE_FULL;
    imp.rgb_transfer = VMAFX_COLOR_TRC_SRGB;
    imp.rgb_out_range = VMAFX_COLOR_RANGE_LIMITED;
    return imp;
}

/* One pixel's element `e` of `elems` in a row of `bytes`-byte samples. */
static void put_sample(uint8_t *row, unsigned x, unsigned elems, unsigned e, size_t bytes,
                       uint32_t v)
{
    uint8_t *const at = row + ((size_t)x * elems + e) * bytes;
    at[0] = (uint8_t)v;
    if (bytes == 2u) {
        at[1] = (uint8_t)(v >> 8u);
    }
}

static uint32_t get_sample(const uint8_t *plane, unsigned x, size_t bytes)
{
    return bytes == 2u ? (uint32_t)(plane[2u * x] | (plane[2u * x + 1u] << 8u)) : plane[x];
}

/* Import `cfg`'s pixels as `pix_fmt` and compare the three planes. 0: all equal. */
static unsigned import_config(const VroConfig *cfg, uint32_t pix_fmt, uint32_t alpha_marker)
{
    const VmafxImportLayout *const layout = layout_of(pix_fmt);
    const size_t bytes = cfg->bpc > 8u ? 2u : 1u;
    uint8_t row[512 * 4 * 2];
    for (size_t i = 0; i < cfg->n; i++) {
        const VroCase *const px = &cfg->cases[i];
        const unsigned elems = layout->rgb_elems;
        put_sample(row, (unsigned)i, elems, layout->elem[0], bytes, px->r);
        put_sample(row, (unsigned)i, elems, layout->elem[1], bytes, px->g);
        put_sample(row, (unsigned)i, elems, layout->elem[2], bytes, px->b);
        if (elems == 4u) {
            unsigned alpha = 0u;
            while (alpha == layout->elem[0] || alpha == layout->elem[1] ||
                   alpha == layout->elem[2]) {
                alpha++;
            }
            put_sample(row, (unsigned)i, elems, alpha, bytes, alpha_marker);
        }
    }
    VmafxFrameImport imp = describe(pix_fmt, cfg->bpc, (unsigned)cfg->n, row, sizeof(row));
    imp.rgb_matrix = cfg->matrix;
    imp.rgb_range = cfg->in_range;
    imp.rgb_out_range = cfg->out_range;
    VmafxFrame *frame = NULL;
    if (vmafx_frame_import(NULL, &imp, &frame, NULL) != VMAFX_OK) {
        return 1u;
    }
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    unsigned bad = vmafx_frame_planes(frame, &planes, NULL) == VMAFX_OK ? 0u : 2u;
    bad |= planes.pix_fmt == VMAFX_PIXEL_FORMAT_YUV444P ? 0u : 4u;
    for (size_t i = 0; i < cfg->n && bad == 0u; i++) {
        const VroCase *const px = &cfg->cases[i];
        const uint32_t want[3] = {px->y, px->cb, px->cr};
        for (unsigned p = 0; p < 3u; p++) {
            if (!matches(get_sample(planes.data[p], (unsigned)i, bytes), want[p],
                         (px->slack >> p) & 1u)) {
                bad |= 8u;
            }
        }
    }
    vmafx_frame_unref(frame);
    return bad;
}

/* Positive: the imported frame is the oracle's, for the three layouts, and the
 * alpha sample is never read (two different markers give the same frame). */
static char *test_import_makes_the_oracle_frame(void)
{
    static const uint32_t formats[3] = {VMAFX_PIXEL_FORMAT_RGB, VMAFX_PIXEL_FORMAT_RGBA,
                                        VMAFX_PIXEL_FORMAT_BGRA};
    for (size_t c = 0; c < VRO_N_CONFIGS; c++) {
        const VroConfig *const cfg = &vro_configs[c];
        mu_assert("the oracle row fits the scratch row", cfg->n <= 512u);
        for (unsigned f = 0; f < 3u; f++) {
            mu_assert("an imported RGB frame is the oracle's, alpha 0",
                      import_config(cfg, formats[f], 0u) == 0u);
            mu_assert("... and with another alpha",
                      import_config(cfg, formats[f], (1u << cfg->bpc) - 1u) == 0u);
        }
    }
    return NULL;
}

/* A refusal of one field: status, the field named, the format named. */
static bool refused(const VmafxFrameImport *imp, VmafxStatus status, const char *field,
                    const char *needle)
{
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    const VmafxStatus got = vmafx_frame_import(NULL, imp, &frame, &error);
    const bool ok = got == status && frame == NULL && error != NULL &&
                    strcmp(vmafx_error_subject(error), field) == 0 &&
                    strstr(vmafx_error_message(error), needle) != NULL;
    if (!ok && error) {
        (void)fprintf(stderr, "\n  refusal: status %d, subject %s, message %s", (int)got,
                      vmafx_error_subject(error), vmafx_error_message(error));
    }
    vmafx_error_free(error);
    return ok;
}

/* Negative: nothing is guessed; each statement missing is refused by name. */
static char *test_a_missing_statement_is_refused_by_name(void)
{
    static const uint8_t row[16] = {0};
    static const uint32_t formats[3] = {VMAFX_PIXEL_FORMAT_RGB, VMAFX_PIXEL_FORMAT_RGBA,
                                        VMAFX_PIXEL_FORMAT_BGRA};
    static const char *const names[3] = {"rgb", "rgba", "bgra"};
    for (unsigned f = 0; f < 3u; f++) {
        VmafxFrameImport imp = describe(formats[f], 8u, 2u, row, sizeof(row));
        imp.rgb_matrix = VMAFX_COLOR_MATRIX_UNKNOWN;
        mu_assert("no matrix", refused(&imp, VMAFX_E_INVALID, "desc.rgb_matrix", names[f]));
        mu_assert("no matrix: the message says it is not guessed",
                  refused(&imp, VMAFX_E_INVALID, "desc.rgb_matrix", "none is assumed"));
        imp = describe(formats[f], 8u, 2u, row, sizeof(row));
        imp.rgb_range = VMAFX_COLOR_RANGE_UNKNOWN;
        mu_assert("no range", refused(&imp, VMAFX_E_INVALID, "desc.rgb_range", names[f]));
        imp = describe(formats[f], 8u, 2u, row, sizeof(row));
        imp.rgb_transfer = VMAFX_COLOR_TRC_UNKNOWN;
        mu_assert("no transfer", refused(&imp, VMAFX_E_INVALID, "desc.rgb_transfer", names[f]));
        imp = describe(formats[f], 8u, 2u, row, sizeof(row));
        imp.rgb_out_range = VMAFX_COLOR_RANGE_UNKNOWN;
        mu_assert("no output range",
                  refused(&imp, VMAFX_E_INVALID, "desc.rgb_out_range", names[f]));
    }
    /* A descriptor of the shape a caller of ABI 0.1.5 passes has no statement. */
    VmafxFrameImport old = VMAFX_FRAME_IMPORT_INIT;
    old.memory = VMAFX_MEMORY_HOST;
    old.pix_fmt = VMAFX_PIXEL_FORMAT_RGBA;
    old.bpc = 8u;
    old.w = 2u;
    old.h = 1u;
    old.n_planes = 1u;
    old.plane[0].handle = (uintptr_t)row;
    old.plane[0].pitch = sizeof(row);
    mu_assert("an old descriptor", refused(&old, VMAFX_E_INVALID, "desc.rgb_matrix", "rgba"));
    return NULL;
}

/* Negative: declared and not converted; and values that are no enumerator. */
static char *test_unsupported_and_invalid_values(void)
{
    static const uint8_t row[16] = {0};
    VmafxFrameImport imp = describe(VMAFX_PIXEL_FORMAT_RGBA, 8u, 2u, row, sizeof(row));
    imp.rgb_matrix = VMAFX_COLOR_MATRIX_BT2020_CL;
    mu_assert("BT.2020 constant luminance",
              refused(&imp, VMAFX_E_NOTSUP, "desc.rgb_matrix", "BT2020_CL"));
    imp.rgb_matrix = VMAFX_COLOR_MATRIX_ICTCP;
    mu_assert("ICtCp", refused(&imp, VMAFX_E_NOTSUP, "desc.rgb_matrix", "ICTCP"));
    imp.rgb_matrix = 99u;
    mu_assert("a matrix value that is none",
              refused(&imp, VMAFX_E_INVALID, "desc.rgb_matrix", "99"));
    imp = describe(VMAFX_PIXEL_FORMAT_RGBA, 8u, 2u, row, sizeof(row));
    imp.rgb_transfer = VMAFX_COLOR_TRC_LINEAR;
    mu_assert("linear light", refused(&imp, VMAFX_E_NOTSUP, "desc.rgb_transfer", "LINEAR"));
    imp.rgb_transfer = 77u;
    mu_assert("a transfer value that is none",
              refused(&imp, VMAFX_E_INVALID, "desc.rgb_transfer", "77"));
    imp = describe(VMAFX_PIXEL_FORMAT_RGBA, 8u, 2u, row, sizeof(row));
    imp.rgb_range = 5u;
    mu_assert("a range value that is none", refused(&imp, VMAFX_E_INVALID, "desc.rgb_range", "5"));
    imp = describe(VMAFX_PIXEL_FORMAT_RGBA, 8u, 2u, row, sizeof(row));
    imp.bpc = 7u;
    mu_assert("below 8 bits", refused(&imp, VMAFX_E_INVALID, "desc.bpc", "rgba"));
    return NULL;
}

/* Boundary: a layout that is not RGB takes no statement and ignores one. */
static char *test_other_layouts_ignore_the_statement(void)
{
    static const uint8_t luma[4] = {1, 2, 3, 4};
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_HOST;
    imp.pix_fmt = VMAFX_PIXEL_FORMAT_YUV400P;
    imp.bpc = 8u;
    imp.w = 2u;
    imp.h = 2u;
    imp.n_planes = 1u;
    imp.plane[0].handle = (uintptr_t)luma;
    imp.plane[0].pitch = 2u;
    imp.rgb_matrix = 99u; /* not read */
    VmafxFrame *frame = NULL;
    mu_assert("a gray frame imports with garbage in rgb_matrix",
              vmafx_frame_import(NULL, &imp, &frame, NULL) == VMAFX_OK && frame != NULL);
    vmafx_frame_unref(frame);
    return NULL;
}

/* The row extent: RGB and RGBA rows hold the elements of their pixels. */
static char *test_rgb_row_extent(void)
{
    unsigned pw[3] = {5u, 5u, 5u};
    unsigned ph[3] = {3u, 3u, 3u};
    uint64_t row = 0;
    uint64_t rows = 0;
    vmafx_import_plane_extent(layout_of(VMAFX_PIXEL_FORMAT_RGB), 8u, 0u, pw, ph, &row, &rows);
    mu_assert("rgb24: three bytes a pixel", row == 15u && rows == 3u);
    vmafx_import_plane_extent(layout_of(VMAFX_PIXEL_FORMAT_BGRA), 16u, 0u, pw, ph, &row, &rows);
    mu_assert("bgra64: four words a pixel", row == 40u && rows == 3u);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_plan_matches_the_oracle),
        MU_TEST(test_gray_has_no_chroma),
        MU_TEST(test_import_makes_the_oracle_frame),
        MU_TEST(test_a_missing_statement_is_refused_by_name),
        MU_TEST(test_unsupported_and_invalid_values),
        MU_TEST(test_other_layouts_ignore_the_statement),
        MU_TEST(test_rgb_row_extent),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
