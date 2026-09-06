/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * test_pelorus_interop.c — vmafx side of the SHARED Pelorus interop ABI
 * conformance fixture (VMAFx/pelorus@818d844 test/interop_test.c, ABI 1.3).
 *
 * Both repos run byte-for-byte the same checks against their own copy of
 * interop.c. A green run here proves vmafx's vendored parser (ADR-1113) is
 * byte-identical to pelorus's writer: a blob packed by pelorus parses in vmafx
 * and vice versa, and the forward/back-compat rules (R3, R4, R6) hold. Keep
 * this file in sync with the pelorus original via
 * scripts/sync-pelorus-interop.sh; the only intended local edit is the include
 * path rewrite ("pelorus/<x>.h" -> "libvmaf/pelorus/<x>.h").
 *
 * No external test framework — exit non-zero on first failure (does NOT use the
 * libvmaf minunit harness in test.c; it carries its own main()).
 */

#include "libvmaf/pelorus/deband.h"
#include "libvmaf/pelorus/interop.h"
#include "libvmaf/pelorus/pelorus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

static int g_fail;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

static void fill_meta(PelorusSideData *m)
{
    memset(m, 0, sizeof(*m));
    m->frame_pts = 123456;
    m->plane_layout = PEL_LAYOUT_420;
    m->bit_depth = 10;
    m->grid_cols = 16;
    m->grid_rows = 9;
    m->producer_id = PEL_FOURCC('P', 'L', 'R', 'S');
}

/* Round-trip: pack three sections, parse them back, verify scalars + framing. */
static void test_roundtrip(void)
{
    PelorusSideData meta;
    PelorusBandingSection band;
    PelorusVarianceSection var;
    PelorusFilmGrainSection grain;
    PelorusPackSection secs[3];
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;

    fill_meta(&meta);

    memset(&band, 0, sizeof(band));
    band.global_banding_risk = 0.42f;
    band.flat_area_fraction = 0.61f;
    band.contour_strength_mean = 0.03f;
    band.dominant_band_luma = 0.18f;

    memset(&var, 0, sizeof(var));
    var.global_variance = 0.25f;
    var.edge_density = 0.10f;
    var.texture_energy = 0.33f;

    memset(&grain, 0, sizeof(grain));
    grain.apply = 1;
    grain.seed = 0xDEADBEEFCAFEULL; /* exercises 8-byte alignment of u64 */
    grain.num_y_points = 3;
    grain.scaling_shift = 8;
    grain.ar_coeff_lag = 2;

    secs[0].id = PEL_SEC_BANDING;
    secs[0].data = &band;
    secs[0].size = (uint32_t)sizeof(band);
    secs[1].id = PEL_SEC_VARIANCE;
    secs[1].data = &var;
    secs[1].size = (uint32_t)sizeof(var);
    secs[2].id = PEL_SEC_FILMGRAIN;
    secs[2].data = &grain;
    secs[2].size = (uint32_t)sizeof(grain);

    CHECK(pel_blob_pack(&meta, secs, 3, &blob, &len) == PEL_OK);
    CHECK(blob != NULL);
    CHECK(pel_blob_is_present(blob, len) == 1);

    /* banding */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_BANDING, sizeof(PelorusBandingSection), &p,
                                &got) == PEL_OK);
    CHECK(got == sizeof(PelorusBandingSection));
    {
        const PelorusBandingSection *b = p;
        CHECK(b->global_banding_risk == 0.42f);
        CHECK(b->flat_area_fraction == 0.61f);
    }

    /* variance */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_VARIANCE, sizeof(PelorusVarianceSection), &p,
                                &got) == PEL_OK);
    {
        const PelorusVarianceSection *v = p;
        CHECK(v->texture_energy == 0.33f);
    }

    /* film grain — verify the 64-bit seed survived (alignment) */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_FILMGRAIN, sizeof(PelorusFilmGrainSection), &p,
                                &got) == PEL_OK);
    {
        const PelorusFilmGrainSection *g = p;
        CHECK(g->seed == 0xDEADBEEFCAFEULL);
        CHECK(g->num_y_points == 3);
        CHECK(g->apply == 1);
    }

    /* a section we did not write is absent (R3 back-compat fallback) */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p,
                                &got) == PEL_ERR_ABSENT);

    pel_blob_free(blob);
}

/* Forward-compat (R4): a consumer that knows a SMALLER struct than the
 * producer wrote must get min(producer, consumer) readable bytes. */
static void test_forward_compat(void)
{
    PelorusSideData meta;
    PelorusVarianceSection var;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;
    const size_t older_consumer_size = 12; /* knew only the first 3 floats */

    fill_meta(&meta);
    memset(&var, 0, sizeof(var));
    var.global_variance = 1.0f;
    sec.id = PEL_SEC_VARIANCE;
    sec.data = &var;
    sec.size = (uint32_t)sizeof(var);

    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_VARIANCE, older_consumer_size, &p, &got) ==
          PEL_OK);
    CHECK(got == older_consumer_size); /* clamped to what the consumer knows */
    pel_blob_free(blob);
}

/* R6: an ABI-major mismatch is detected and rejected, not misread. */
static void test_abi_major_mismatch(void)
{
    PelorusSideData meta;
    PelorusBandingSection band;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;
    PelorusSideData *hdr;

    fill_meta(&meta);
    memset(&band, 0, sizeof(band));
    sec.id = PEL_SEC_BANDING;
    sec.data = &band;
    sec.size = (uint32_t)sizeof(band);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);

    hdr = (PelorusSideData *)(void *)(blob + PELORUS_SIDEDATA_UUID_LEN);
    hdr->abi_major = (uint16_t)(PELORUS_ABI_MAJOR + 1u);

    CHECK(pel_blob_is_present(blob, len) == 0);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_BANDING, sizeof(PelorusBandingSection), &p,
                                &got) == PEL_ERR_ABI);
    pel_blob_free(blob);
}

/* A non-Pelorus buffer (e.g. an x264 user-data SEI) is cleanly ignored. */
static void test_foreign_buffer(void)
{
    uint8_t foreign[64];
    const void *p = NULL;
    size_t got = 0;

    memset(foreign, 0xAB, sizeof(foreign));
    CHECK(pel_blob_is_present(foreign, sizeof(foreign)) == 0);
    CHECK(pel_blob_find_section(foreign, sizeof(foreign), PEL_SEC_BANDING,
                                sizeof(PelorusBandingSection), &p, &got) == PEL_ERR_ABSENT);
}

/* A header-only blob (no sections) is valid and parses. */
static void test_header_only(void)
{
    PelorusSideData meta;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;

    fill_meta(&meta);
    CHECK(pel_blob_pack(&meta, NULL, 0, &blob, &len) == PEL_OK);
    CHECK(pel_blob_is_present(blob, len) == 1);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_BANDING, sizeof(PelorusBandingSection), &p,
                                &got) == PEL_ERR_ABSENT);
    pel_blob_free(blob);
}

/* Truncating the buffer is detected, not read out of bounds. */
static void test_truncation(void)
{
    PelorusSideData meta;
    PelorusMotionSection mv;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;

    fill_meta(&meta);
    memset(&mv, 0, sizeof(mv));
    sec.id = PEL_SEC_MOTION;
    sec.data = &mv;
    sec.size = (uint32_t)sizeof(mv);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);

    /* Lie about the length: claim only the uuid + header are present. */
    CHECK(pel_blob_find_section(blob, (size_t)PELORUS_SIDEDATA_UUID_LEN + sizeof(PelorusSideData),
                                PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p,
                                &got) == PEL_ERR_TRUNCATED);
    pel_blob_free(blob);
}

/* Defensive parser (R5): a crafted/corrupt blob whose section payload offset is
 * NOT 8-byte aligned must be rejected, not cast to a struct at an unaligned
 * address (the packer always 8-aligns, so this only arises from foreign/corrupt
 * framing). The current suite never patches dir[i].offset, so exercise it here. */
static void test_misaligned_offset(void)
{
    PelorusSideData meta;
    PelorusFilmGrainSection grain; /* has a u64 at offset 0 -> alignment matters */
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;
    PelorusSideData *hdr;
    PelorusSectionDir *dir;

    fill_meta(&meta);
    memset(&grain, 0, sizeof(grain));
    grain.seed = 0xDEADBEEFCAFEULL;
    sec.id = PEL_SEC_FILMGRAIN;
    sec.data = &grain;
    sec.size = (uint32_t)sizeof(grain);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);

    /* Well-formed first: the 8-aligned offset parses. */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_FILMGRAIN, sizeof(PelorusFilmGrainSection), &p,
                                &got) == PEL_OK);

    /* Now hand-patch dir[0].offset to a misaligned value (+4). The section then
     * still fits the buffer but its start is no longer 8-aligned. */
    hdr = (PelorusSideData *)(void *)(blob + PELORUS_SIDEDATA_UUID_LEN);
    dir = (PelorusSectionDir *)(void *)(blob + PELORUS_SIDEDATA_UUID_LEN + hdr->header_size);
    dir[0].offset += 4u;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_FILMGRAIN, sizeof(PelorusFilmGrainSection), &p,
                                &got) == PEL_ERR_ABI);
    pel_blob_free(blob);
}

/* Defensive packer (R5 overflow guard): a section whose declared size, once
 * 8-aligned and summed, would overflow the uint32 total_size wire field must be
 * rejected before allocating (otherwise the alloc undersizes and the copy
 * overflows the heap). The size check runs before any deref of sec.data, so a
 * tiny dummy data pointer with a huge declared size is safe to pass here. */
static void test_pack_size_overflow(void)
{
    PelorusSideData meta;
    PelorusPackSection sec;
    uint8_t dummy = 0;
    uint8_t *blob = NULL;
    size_t len = 0;

    fill_meta(&meta);
    /* size near UINT32_MAX: 8-aligning it wraps a 32-bit accumulator to ~0 but the
     * 64-bit guard catches need > UINT32_MAX and rejects. data is never read. */
    sec.id = PEL_SEC_BANDING;
    sec.data = &dummy;
    sec.size = 0xFFFFFFF9u;
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_ERR_RANGE);
    CHECK(blob == NULL);
}

/* PEL_SEC_QPREPORT (f): pack the encoder-honored QP readback with a per-cell
 * QP map appended after the blob, parse it back, verify scalars + the map. */
static void test_qp_report_roundtrip(void)
{
    PelorusSideData meta;
    PelorusQpReportSection qp;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;
    const uint16_t cells = 16 * 9; /* matches fill_meta grid */
    int8_t cellmap[16 * 9];
    int i;

    fill_meta(&meta);

    memset(&qp, 0, sizeof(qp));
    qp.avg_qp = 27.5f;
    qp.psnr_y = 41.2f;
    qp.total_bits = 1234567ULL; /* exercises the 64-bit field alignment */
    qp.num_intra_blocks = 40;
    qp.num_inter_blocks = 200;
    qp.num_skipped_blocks = 16;
    qp.honored_fraction = 0.75f;
    qp.report_source = PEL_QPSRC_QSV;
    qp.block_size_log2 = 4; /* 16x16 */
    qp.qp_valid = 1;
    qp.qp_cell_size = cells; /* int8 per cell */
    /* qp_cell_offset is set below once we know the section's blob offset. */

    for (i = 0; i < (int)cells; i++) {
        cellmap[i] = (int8_t)(20 + (i % 12)); /* a recognizable QP ramp */
    }

    sec.id = PEL_SEC_QPREPORT;
    sec.data = &qp;
    sec.size = (uint32_t)sizeof(qp);

    /* Pack the section and verify the scalars + the map offset/size fields
     * round-trip (the per-cell map payload itself is appended by the producer
     * after pack, like every other map section — see docs/api/interop-abi.md;
     * the fold path is exercised separately in test_qp_report_fold). */
    qp.qp_cell_offset = 0; /* producer sets this when it appends cellmap */
    (void)cellmap;
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);
    CHECK(blob != NULL);
    CHECK(pel_blob_is_present(blob, len) == 1);

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_QPREPORT, sizeof(PelorusQpReportSection), &p,
                                &got) == PEL_OK);
    CHECK(got == sizeof(PelorusQpReportSection));
    {
        const PelorusQpReportSection *r = p;
        CHECK(r->avg_qp == 27.5f);
        CHECK(r->psnr_y == 41.2f);
        CHECK(r->total_bits == 1234567ULL);
        CHECK(r->num_inter_blocks == 200);
        CHECK(r->honored_fraction == 0.75f);
        CHECK(r->report_source == PEL_QPSRC_QSV);
        CHECK(r->block_size_log2 == 4);
        CHECK(r->qp_valid == 1);
        CHECK(r->qp_cell_size == cells);
        CHECK(r->num_intra_blocks == 40);
        CHECK(r->num_skipped_blocks == 16);
        CHECK(r->psnr_u == 0.0f);
        CHECK(r->psnr_v == 0.0f);
    }

    /* An older consumer (knows only the first two floats) still parses (R4). */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_QPREPORT, 8, &p, &got) == PEL_OK);
    CHECK(got == 8);

    pel_blob_free(blob);
}

/* PEL_SEC_MOTION_CONF (g): pack the per-block MV confidence section, parse it
 * back, verify the offset/size/metric fields round-trip; a consumer that knows
 * only the offset/size (not conf_metric) still parses (R4); and a section we did
 * not write is absent (R3). */
static void test_motion_conf_roundtrip(void)
{
    PelorusSideData meta;
    PelorusMotionConfSection conf;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;

    fill_meta(&meta);
    memset(&conf, 0, sizeof(conf));
    conf.conf_field_offset = 0;    /* producer patches once it appends the map */
    conf.conf_field_size = 16 * 9; /* matches fill_meta grid (uint8 per cell)  */
    conf.conf_metric = PEL_MOTION_CONF_SAD;

    sec.id = PEL_SEC_MOTION_CONF;
    sec.data = &conf;
    sec.size = (uint32_t)sizeof(conf);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);
    CHECK(blob != NULL);
    CHECK(pel_blob_is_present(blob, len) == 1);

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION_CONF, sizeof(PelorusMotionConfSection),
                                &p, &got) == PEL_OK);
    CHECK(got == sizeof(PelorusMotionConfSection));
    {
        const PelorusMotionConfSection *r = p;
        CHECK(r->conf_field_size == 16 * 9);
        CHECK(r->conf_metric == PEL_MOTION_CONF_SAD);
    }

    /* R4: a consumer that knows only conf_field_offset+conf_field_size (8 bytes,
     * the meaning that predates conf_metric) still parses. */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION_CONF, 8, &p, &got) == PEL_OK);
    CHECK(got == 8);

    /* R3: the plain motion section we did NOT write is absent. */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p,
                                &got) == PEL_ERR_ABSENT);

    pel_blob_free(blob);

    /* Production usage: vf_pelorus_mc writes MOTION and MOTION_CONF together —
     * both must round-trip from one blob regardless of dir[] ordering. */
    {
        PelorusMotionSection mo;
        PelorusPackSection both[2];
        uint8_t *b2 = NULL;
        size_t l2 = 0;

        memset(&mo, 0, sizeof(mo));
        both[0].id = PEL_SEC_MOTION;
        both[0].data = &mo;
        both[0].size = (uint32_t)sizeof(mo);
        both[1].id = PEL_SEC_MOTION_CONF;
        both[1].data = &conf;
        both[1].size = (uint32_t)sizeof(conf);
        CHECK(pel_blob_pack(&meta, both, 2, &b2, &l2) == PEL_OK);
        CHECK(pel_blob_find_section(b2, l2, PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p,
                                    &got) == PEL_OK);
        CHECK(pel_blob_find_section(b2, l2, PEL_SEC_MOTION_CONF, sizeof(PelorusMotionConfSection),
                                    &p, &got) == PEL_OK);
        CHECK(got == sizeof(PelorusMotionConfSection));
        pel_blob_free(b2);
    }
}

/* PEL_SEC_COMPLEXITY (h): pack the per-frame complexity scalar, round-trip the
 * fields, and confirm R4 (an older consumer that knows only the first float
 * still parses). */
static void test_complexity_roundtrip(void)
{
    PelorusSideData meta;
    PelorusComplexitySection cx;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;

    fill_meta(&meta);
    memset(&cx, 0, sizeof(cx));
    cx.complexity = 0.625f;
    cx.texture_energy = 0.5f;
    cx.motion_component = 0.25f;
    cx.has_scene_cut = 1;

    sec.id = PEL_SEC_COMPLEXITY;
    sec.data = &cx;
    sec.size = (uint32_t)sizeof(cx);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);
    CHECK(blob != NULL);

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_COMPLEXITY, sizeof(PelorusComplexitySection), &p,
                                &got) == PEL_OK);
    CHECK(got == sizeof(PelorusComplexitySection));
    {
        const PelorusComplexitySection *r = p;
        CHECK(r->complexity == 0.625f);
        CHECK(r->texture_energy == 0.5f);
        CHECK(r->motion_component == 0.25f);
        CHECK(r->has_scene_cut == 1);
    }
    /* R4: a consumer that knows only `complexity` (first 4 bytes) still parses. */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_COMPLEXITY, 4, &p, &got) == PEL_OK);
    CHECK(got == 4);
    pel_blob_free(blob);
}

/* The reader stub: fold a per-block QP grid onto the cell grid. With a block
 * grid that is an integer multiple of the cell grid, each cell averages a clean
 * block tile, so a uniform block QP folds to the same per-cell QP. */
static void test_qp_report_fold(void)
{
    PelorusQpReportInput in;
    PelorusQpReportSection out;
    int8_t blocks[8 * 4];
    int8_t cells[4 * 2];
    int i;

    memset(&in, 0, sizeof(in));
    for (i = 0; i < (int)(sizeof(blocks)); i++) {
        blocks[i] = 30; /* uniform actual QP across all blocks */
    }
    in.block_qp = blocks;
    in.blk_cols = 8;
    in.blk_rows = 4;
    in.block_size_log2 = 4;
    in.report_source = PEL_QPSRC_QSV;
    in.avg_qp = 30.0f;
    in.num_inter_blocks = 32;

    CHECK(pel_qp_report_from_blocks(&in, 4, 2, &out, cells, sizeof(cells)) == PEL_OK);
    CHECK(out.qp_valid == 1);
    CHECK(out.qp_cell_size == 4u * 2u);
    CHECK(out.report_source == PEL_QPSRC_QSV);
    CHECK(out.avg_qp == 30.0f);
    for (i = 0; i < (int)(sizeof(cells)); i++) {
        CHECK(cells[i] == 30); /* uniform block QP -> uniform cell QP */
    }

    /* Frame-stats-only path: no block grid -> qp_valid 0, scalars preserved. */
    in.block_qp = NULL;
    CHECK(pel_qp_report_from_blocks(&in, 4, 2, &out, NULL, 0) == PEL_OK);
    CHECK(out.qp_valid == 0);
    CHECK(out.qp_cell_size == 0u);
    CHECK(out.avg_qp == 30.0f);

    /* Too-small output buffer is rejected, not overrun. */
    in.block_qp = blocks;
    CHECK(pel_qp_report_from_blocks(&in, 4, 2, &out, cells, 3) == PEL_ERR_RANGE);

    /* NULL inputs / zero grid are rejected. */
    CHECK(pel_qp_report_from_blocks(NULL, 4, 2, &out, cells, sizeof(cells)) == PEL_ERR_INVALID);
    CHECK(pel_qp_report_from_blocks(&in, 0, 2, &out, cells, sizeof(cells)) == PEL_ERR_INVALID);
}

/* The x265 CSV reader (ADR-0122): write a minimal x265-shaped CSV to a temp
 * file, parse it, fold it into a PEL_SEC_QPREPORT, and verify the aggregated
 * scalars + the requested-vs-honored honored_fraction. This is the runnable
 * closed-loop surface; the fixture exercises it without invoking x265. */
static void test_x265_csv_reader(void)
{
    /* Three coded frames + a trailing aggregate row x265 appends. Columns match
     * x265 --csv-log-level 2 (subset; the reader locates by header name). The
     * honored QP differs from a flat requested QP per slice type, exactly the
     * signal honored_fraction measures. */
    static const char *csv = "Encode Order, Type, POC, QP, Bits, Y PSNR, U PSNR, V PSNR\n"
                             "0, I-SLICE, 0, 26.00, 24000, 43.1, 40.5, 40.4\n"
                             "1, P-SLICE, 2, 30.00, 8000, 38.0, 37.2, 36.8\n"
                             "2, B-SLICE, 1, 34.00, 3000, 37.1, 36.6, 35.5\n"
                             "Total frames, 3, , 30.00, , , , \n";
    PelorusX265Frame frames[8];
    size_t count = 0;
    PelorusQpReportSection qp;
    const char *path = "pelorus_x265_csv_test.csv";
    FILE *fp;

    fp = fopen(path, "w");
    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    CHECK(fputs(csv, fp) >= 0);
    CHECK(fclose(fp) == 0);

    /* Parse: 3 coded frames, the "Total frames" aggregate row dropped. */
    CHECK(pel_x265_csv_parse(path, frames, 8, &count) == PEL_OK);
    CHECK(count == 3);
    CHECK(frames[0].slice_type == 'I');
    CHECK(frames[0].qp == 26.0f);
    CHECK(frames[0].bits == 24000ULL);
    CHECK(frames[1].slice_type == 'P');
    CHECK(frames[2].qp == 34.0f);
    CHECK(frames[2].psnr_v == 35.5f);

    /* Fold, no requested map: bit-weighted mean QP, summed bits, qp_valid 0. */
    CHECK(pel_qp_report_from_x265_frames(frames, count, NULL, &qp) == PEL_OK);
    CHECK(qp.qp_valid == 0);
    CHECK(qp.report_source == PEL_QPSRC_NONE);
    CHECK(qp.total_bits == 35000ULL);
    /* bit-weighted: (26*24000 + 30*8000 + 34*3000)/35000 = 27.6 exactly. */
    CHECK(qp.avg_qp > 27.55f && qp.avg_qp < 27.65f);
    CHECK(qp.psnr_y > 41.0f && qp.psnr_y < 43.2f); /* I-frame-dominated */
    CHECK(qp.honored_fraction == 0.0f);            /* no request to compare */

    /* honored_fraction: a downstream pass requested a FLAT QP 28 on every
     * frame; the encoder spread it (26/30/34). Frame 0 moved DOWN, frames 1+2
     * moved UP from the achieved mean (30). A flat request has zero per-frame
     * delta, so every frame's requested delta is "flat" while the achieved
     * deltas are not -> agreement only where the achieved delta is also flat.
     * Achieved mean = 30: frame1 (30) is flat -> agrees with flat request;
     * frames 0,2 moved -> disagree. Expect 1/3. */
    {
        const float requested_flat[3] = {28.0f, 28.0f, 28.0f};
        CHECK(pel_qp_report_from_x265_frames(frames, count, requested_flat, &qp) == PEL_OK);
        CHECK(qp.honored_fraction > 0.33f && qp.honored_fraction < 0.34f);
    }

    /* honored_fraction: a request that mirrors the achieved shape (low for the
     * I frame, high for the B frame) should score 1.0 (every sign agrees). */
    {
        const float requested_shaped[3] = {24.0f, 30.0f, 36.0f};
        CHECK(pel_qp_report_from_x265_frames(frames, count, requested_shaped, &qp) == PEL_OK);
        CHECK(qp.honored_fraction == 1.0f);
    }

    /* A capacity smaller than the row count truncates and reports RANGE. */
    CHECK(pel_x265_csv_parse(path, frames, 2, &count) == PEL_ERR_RANGE);
    CHECK(count == 2);

    /* A missing file is ABSENT, not a crash. */
    CHECK(pel_x265_csv_parse("pelorus_no_such_file.csv", frames, 8, &count) == PEL_ERR_ABSENT);

    /* NULL guards. */
    CHECK(pel_x265_csv_parse(NULL, frames, 8, &count) == PEL_ERR_INVALID);
    CHECK(pel_qp_report_from_x265_frames(NULL, 1, NULL, &qp) == PEL_ERR_INVALID);
    CHECK(pel_qp_report_from_x265_frames(frames, 0, NULL, &qp) == PEL_ERR_INVALID);

    (void)remove(path);
}

/* The deband param contract: defaults validate, out-of-range is rejected. */
static void test_deband_params(void)
{
    PelorusDebandParams pp;
    const char *what = NULL;

    pel_deband_params_default(&pp);
    CHECK(pel_deband_params_validate(&pp, &what) == PEL_OK);

    pp.range = 99;
    CHECK(pel_deband_params_validate(&pp, &what) == PEL_ERR_RANGE);
    CHECK(what != NULL && strcmp(what, "range") == 0);
}

int main(void)
{
    test_roundtrip();
    test_forward_compat();
    test_abi_major_mismatch();
    test_foreign_buffer();
    test_header_only();
    test_truncation();
    test_misaligned_offset();
    test_pack_size_overflow();
    test_qp_report_roundtrip();
    test_motion_conf_roundtrip();
    test_complexity_roundtrip();
    test_qp_report_fold();
    test_x265_csv_reader();
    test_deband_params();

    if (g_fail != 0) {
        (void)fprintf(stderr, "%d check(s) failed\n", g_fail);
        return EXIT_FAILURE;
    }
    (void)printf("interop: all checks passed (libpelorus %s, ABI %u.%u)\n",
                 pelorus_version_string(), PELORUS_ABI_MAJOR, PELORUS_ABI_MINOR);
    return EXIT_SUCCESS;
}

/* NOLINTEND(modernize-use-nullptr) */
