/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The CPU reference of the import conversions (core/src/vmafx/
 * import_convert.h, ADR-2133) on hand-computed bytes: semi-planar 4:2:2 and
 * 4:4:4 (NV16, P210, NV24, P416), packed Y210 and Y410 / XV30, and the read
 * plan every layout of the import table makes (vmafx_import_plane_read()).
 * Positive: each output sample is the named input sample, shifted and
 * masked. Negative: the unused alpha bits of XV30 and the tail of an odd
 * Y210 row never reach a plane; the table refuses a bit depth it does not
 * hold. Boundary: odd widths, a 16-bit sample at its extremes.
 */

#include <stdint.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/import_convert.h"
#include "vmafx/internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static void put16(uint8_t *at, uint16_t v)
{
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8u);
}

static uint16_t get16(const uint8_t *at, unsigned i)
{
    return (uint16_t)(at[(size_t)2u * i] | (at[(size_t)2u * i + 1u] << 8u));
}

/* Plane `i` of `layout` at `bpc` read from `src` (`pitch` bytes per row) into
 * `out`, as the host import reads it. */
static void read_plane(const VmafxImportLayout *layout, uint32_t bpc, uint32_t i,
                       const uint8_t *src, size_t pitch, unsigned w, unsigned h, uint8_t *out)
{
    VmafxImportRead rd;
    vmafx_import_plane_read(layout, bpc, i, &rd);
    vmafx_import_read_plane(out, (size_t)w * (bpc > 8u ? 2u : 1u), bpc > 8u ? 2u : 1u, src, pitch,
                            w, h, &rd);
}

static const VmafxImportLayout nv16 = {VMAFX_PIXEL_FORMAT_NV16,
                                       VMAFX_PIXEL_FORMAT_YUV422P,
                                       2u,
                                       8u,
                                       8u,
                                       0u,
                                       true,
                                       "nv16",
                                       VMAFX_IMPORT_PACKED_NONE,
                                       {0u, 0u, 0u},
                                       false};
static const VmafxImportLayout p416 = {VMAFX_PIXEL_FORMAT_P416,
                                       VMAFX_PIXEL_FORMAT_YUV444P,
                                       2u,
                                       16u,
                                       16u,
                                       0u,
                                       true,
                                       "p416",
                                       VMAFX_IMPORT_PACKED_NONE,
                                       {0u, 0u, 0u},
                                       false};
static const VmafxImportLayout y210 = {VMAFX_PIXEL_FORMAT_Y210,
                                       VMAFX_PIXEL_FORMAT_YUV422P,
                                       1u,
                                       10u,
                                       10u,
                                       6u,
                                       false,
                                       "y210",
                                       VMAFX_IMPORT_PACKED_YUYV,
                                       {0u, 1u, 3u},
                                       false};
static const VmafxImportLayout yuy2 = {VMAFX_PIXEL_FORMAT_YUYV422,
                                       VMAFX_PIXEL_FORMAT_YUV422P,
                                       1u,
                                       8u,
                                       8u,
                                       0u,
                                       false,
                                       "yuyv422",
                                       VMAFX_IMPORT_PACKED_YUYV,
                                       {0u, 1u, 3u},
                                       false};
static const VmafxImportLayout xv30 = {VMAFX_PIXEL_FORMAT_Y410,
                                       VMAFX_PIXEL_FORMAT_YUV444P,
                                       1u,
                                       10u,
                                       10u,
                                       0u,
                                       false,
                                       "xv30",
                                       VMAFX_IMPORT_PACKED_XVYU2101010,
                                       {0u, 0u, 0u},
                                       false};
static const VmafxImportLayout xv36 = {VMAFX_PIXEL_FORMAT_XV36,
                                       VMAFX_PIXEL_FORMAT_YUV444P,
                                       1u,
                                       12u,
                                       12u,
                                       4u,
                                       false,
                                       "xv36",
                                       VMAFX_IMPORT_PACKED_UYV4,
                                       {1u, 0u, 2u},
                                       false};
static const VmafxImportLayout vuyx = {VMAFX_PIXEL_FORMAT_VUYX,
                                       VMAFX_PIXEL_FORMAT_YUV444P,
                                       1u,
                                       8u,
                                       8u,
                                       0u,
                                       false,
                                       "vuyx",
                                       VMAFX_IMPORT_PACKED_UYV4,
                                       {2u, 1u, 0u},
                                       false};
static const VmafxImportLayout msb = {VMAFX_PIXEL_FORMAT_YUV444P_MSB,
                                      VMAFX_PIXEL_FORMAT_YUV444P,
                                      3u,
                                      9u,
                                      16u,
                                      0u,
                                      false,
                                      "yuv444p16msb",
                                      VMAFX_IMPORT_PACKED_NONE,
                                      {0u, 0u, 0u},
                                      true};

/* NV16: chroma rows are full height and hold one Cb Cr pair per column. */
static char *test_nv16_pairs(void)
{
    const uint8_t uv[2][6] = {{1, 2, 3, 4, 5, 6}, {7, 8, 9, 10, 11, 12}};
    uint8_t cb[2][3];
    uint8_t cr[2][3];
    read_plane(&nv16, 8u, 1u, &uv[0][0], 6u, 3u, 2u, &cb[0][0]);
    read_plane(&nv16, 8u, 2u, &uv[0][0], 6u, 3u, 2u, &cr[0][0]);
    const uint8_t want_cb[2][3] = {{1, 3, 5}, {7, 9, 11}};
    const uint8_t want_cr[2][3] = {{2, 4, 6}, {8, 10, 12}};
    mu_assert("cb", memcmp(cb, want_cb, sizeof(cb)) == 0);
    mu_assert("cr", memcmp(cr, want_cr, sizeof(cr)) == 0);
    return NULL;
}

/* P416: 16-bit samples are not shifted, the extremes survive. */
static char *test_p416_extremes(void)
{
    uint8_t uv[8];
    put16(uv + 0, 0xffffu);
    put16(uv + 2, 0x0000u);
    put16(uv + 4, 0x8001u);
    put16(uv + 6, 0x7ffeu);
    uint8_t cb[4];
    uint8_t cr[4];
    read_plane(&p416, 16u, 1u, uv, 8u, 2u, 1u, cb);
    read_plane(&p416, 16u, 2u, uv, 8u, 2u, 1u, cr);
    mu_assert("cb", get16(cb, 0) == 0xffffu && get16(cb, 1) == 0x8001u);
    mu_assert("cr", get16(cr, 0) == 0x0000u && get16(cr, 1) == 0x7ffeu);
    return NULL;
}

/* Y210: Y0 Cb Y1 Cr words, the 10 bits in the top of each; an odd width's
 * last group has no second pixel and its Y1 slot never reaches the plane. */
static char *test_y210_groups(void)
{
    uint8_t row[16];
    const uint16_t words[8] = {100, 200, 300, 400, 500, 600, 700, 800};
    for (unsigned k = 0; k < 8u; k++) {
        put16(row + (size_t)2u * k, (uint16_t)(words[k] << 6u));
    }
    uint8_t y[6];
    uint8_t cb[4];
    uint8_t cr[4];
    read_plane(&y210, 10u, 0u, row, 16u, 3u, 1u, y); /* odd width 3 */
    read_plane(&y210, 10u, 1u, row, 16u, 2u, 1u, cb);
    read_plane(&y210, 10u, 2u, row, 16u, 2u, 1u, cr);
    mu_assert("y", get16(y, 0) == 100u && get16(y, 1) == 300u && get16(y, 2) == 500u);
    mu_assert("cb", get16(cb, 0) == 200u && get16(cb, 1) == 600u);
    mu_assert("cr", get16(cr, 0) == 400u && get16(cr, 1) == 800u);
    return NULL;
}

/* XV30: Cb in bits 0..9, Y in 10..19, Cr in 20..29; the top two bits are not
 * read, whatever they hold. */
static char *test_xv30_fields(void)
{
    const uint32_t words[2] = {1023u | (0u << 10u) | (512u << 20u) | (3u << 30u),
                               5u | (1023u << 10u) | (1u << 20u) | (0u << 30u)};
    uint8_t row[8];
    memcpy(row, words, sizeof(row));
    uint8_t y[4];
    uint8_t cb[4];
    uint8_t cr[4];
    read_plane(&xv30, 10u, 0u, row, 8u, 2u, 1u, y);
    read_plane(&xv30, 10u, 1u, row, 8u, 2u, 1u, cb);
    read_plane(&xv30, 10u, 2u, row, 8u, 2u, 1u, cr);
    mu_assert("y", get16(y, 0) == 0u && get16(y, 1) == 1023u);
    mu_assert("cb", get16(cb, 0) == 1023u && get16(cb, 1) == 5u);
    mu_assert("cr", get16(cr, 0) == 512u && get16(cr, 1) == 1u);
    return NULL;
}

/* The row extent of each layout and the refused depth. */
static char *test_extents_and_table(void)
{
    unsigned pw[3] = {5u, 3u, 3u};
    unsigned ph[3] = {4u, 4u, 4u};
    uint64_t row = 0;
    uint64_t rows = 0;
    vmafx_import_plane_extent(&y210, 10u, 0u, pw, ph, &row, &rows);
    mu_assert("y210 row: a group per chroma column", row == 24u && rows == 4u);
    vmafx_import_plane_extent(&yuy2, 8u, 0u, pw, ph, &row, &rows);
    mu_assert("yuyv422 row: bytes", row == 12u && rows == 4u);
    pw[1] = 5u;
    vmafx_import_plane_extent(&xv36, 12u, 0u, pw, ph, &row, &rows);
    mu_assert("xv36 row: four words per pixel", row == 40u && rows == 4u);
    vmafx_import_plane_extent(&vuyx, 8u, 0u, pw, ph, &row, &rows);
    mu_assert("vuyx row: four bytes per pixel", row == 20u && rows == 4u);
    pw[1] = 3u;
    pw[1] = 5u;
    vmafx_import_plane_extent(&xv30, 10u, 0u, pw, ph, &row, &rows);
    mu_assert("xv30 row: a word per pixel", row == 20u && rows == 4u);
    vmafx_import_plane_extent(&nv16, 8u, 1u, pw, ph, &row, &rows);
    mu_assert("nv16 chroma row: a pair per column", row == 10u && rows == 4u);
    return NULL;
}

/* YUYV422 bytes, VUYX byte order, XV36 words (the top 12 bits) and MSB planar
 * words (the top `bpc` bits): each plane's samples from hand-written bytes. */
static char *test_other_packed_layouts(void)
{
    const uint8_t yuy2_row[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    uint8_t y[4];
    uint8_t cb[2];
    uint8_t cr[2];
    read_plane(&yuy2, 8u, 0u, yuy2_row, 8u, 4u, 1u, y);
    read_plane(&yuy2, 8u, 1u, yuy2_row, 8u, 2u, 1u, cb);
    read_plane(&yuy2, 8u, 2u, yuy2_row, 8u, 2u, 1u, cr);
    mu_assert("yuyv422 y", y[0] == 10u && y[1] == 30u && y[2] == 50u && y[3] == 70u);
    mu_assert("yuyv422 c", cb[0] == 20u && cb[1] == 60u && cr[0] == 40u && cr[1] == 80u);
    const uint8_t vuyx_row[8] = {1, 2, 3, 0xee, 4, 5, 6, 0xee}; /* V Cb Y X */
    uint8_t vy[2];
    uint8_t vcb[2];
    uint8_t vcr[2];
    read_plane(&vuyx, 8u, 0u, vuyx_row, 8u, 2u, 1u, vy);
    read_plane(&vuyx, 8u, 1u, vuyx_row, 8u, 2u, 1u, vcb);
    read_plane(&vuyx, 8u, 2u, vuyx_row, 8u, 2u, 1u, vcr);
    mu_assert("vuyx", vy[0] == 3u && vy[1] == 6u && vcb[0] == 2u && vcb[1] == 5u && vcr[0] == 1u &&
                          vcr[1] == 4u);
    uint8_t xv36_row[8];
    put16(xv36_row + 0, (uint16_t)(0x123u << 4u)); /* Cb */
    put16(xv36_row + 2, (uint16_t)(0xabcu << 4u)); /* Y */
    put16(xv36_row + 4, (uint16_t)(0x456u << 4u)); /* Cr */
    put16(xv36_row + 6, 0xffffu);                  /* X */
    uint8_t xy[2];
    uint8_t xcb[2];
    uint8_t xcr[2];
    read_plane(&xv36, 12u, 0u, xv36_row, 8u, 1u, 1u, xy);
    read_plane(&xv36, 12u, 1u, xv36_row, 8u, 1u, 1u, xcb);
    read_plane(&xv36, 12u, 2u, xv36_row, 8u, 1u, 1u, xcr);
    mu_assert("xv36", get16(xy, 0) == 0xabcu && get16(xcb, 0) == 0x123u && get16(xcr, 0) == 0x456u);
    uint8_t words[4];
    put16(words, (uint16_t)(1023u << 6u));
    put16(words + 2, (uint16_t)(5u << 6u));
    uint8_t m[4];
    read_plane(&msb, 10u, 1u, words, 4u, 2u, 1u, m);
    mu_assert("msb 10 bits", get16(m, 0) == 1023u && get16(m, 1) == 5u);
    read_plane(&msb, 12u, 0u, words, 4u, 2u, 1u, m);
    mu_assert("msb 12 bits shifts by 4", get16(m, 0) == (1023u << 6u >> 4u));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_other_packed_layouts), MU_TEST(test_nv16_pairs),
        MU_TEST(test_p416_extremes),        MU_TEST(test_y210_groups),
        MU_TEST(test_xv30_fields),          MU_TEST(test_extents_and_table),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
