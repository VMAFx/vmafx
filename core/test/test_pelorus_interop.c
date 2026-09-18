/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
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

/* Build + pack the three-section round-trip fixture (banding + variance +
 * film grain) and check the pack itself succeeded. Split out of
 * test_roundtrip so that function stays within the readability-function-size
 * branch budget: CHECK expands to a do-while plus an if, i.e. two branches
 * each (ADR-0141). */
static uint8_t *build_roundtrip_blob(size_t *out_len)
{
    PelorusSideData meta;
    PelorusBandingSection band;
    PelorusVarianceSection var;
    PelorusFilmGrainSection grain;
    PelorusPackSection secs[3];
    uint8_t *blob = NULL;

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

    CHECK(pel_blob_pack(&meta, secs, 3, &blob, out_len) == PEL_OK);
    CHECK(blob != NULL);
    CHECK(pel_blob_is_present(blob, *out_len) == 1);
    return blob;
}

/* The four per-section checks below are each their own function for the same
 * branch-budget reason as build_roundtrip_blob above. */
static void check_roundtrip_banding(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_BANDING, sizeof(PelorusBandingSection), &p,
                                &got) == PEL_OK);
    CHECK(got == sizeof(PelorusBandingSection));
    {
        const PelorusBandingSection *b = p;
        CHECK(b->global_banding_risk == 0.42f);
        CHECK(b->flat_area_fraction == 0.61f);
    }
}

static void check_roundtrip_variance(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_VARIANCE, sizeof(PelorusVarianceSection), &p,
                                &got) == PEL_OK);
    {
        const PelorusVarianceSection *v = p;
        CHECK(v->texture_energy == 0.33f);
    }
}

/* film grain — verify the 64-bit seed survived (alignment) */
static void check_roundtrip_filmgrain(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_FILMGRAIN, sizeof(PelorusFilmGrainSection), &p,
                                &got) == PEL_OK);
    {
        const PelorusFilmGrainSection *g = p;
        CHECK(g->seed == 0xDEADBEEFCAFEULL);
        CHECK(g->num_y_points == 3);
        CHECK(g->apply == 1);
    }
}

/* a section we did not write is absent (R3 back-compat fallback) */
static void check_roundtrip_absent_motion(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p,
                                &got) == PEL_ERR_ABSENT);
}

/* Round-trip: pack three sections, parse them back, verify scalars + framing. */
static void test_roundtrip(void)
{
    size_t len = 0;
    uint8_t *blob = build_roundtrip_blob(&len);

    check_roundtrip_banding(blob, len);
    check_roundtrip_variance(blob, len);
    check_roundtrip_filmgrain(blob, len);
    check_roundtrip_absent_motion(blob, len);

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
    PelorusSideData hdr;

    fill_meta(&meta);
    memset(&band, 0, sizeof(band));
    sec.id = PEL_SEC_BANDING;
    sec.data = &band;
    sec.size = (uint32_t)sizeof(band);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);

    /* Patch through a copy: the blob is a byte buffer, so reading it through a
     * struct pointer would be an aliasing access (CERT EXP39-C). */
    (void)memcpy(&hdr, blob + PELORUS_SIDEDATA_UUID_LEN, sizeof(hdr));
    hdr.abi_major = (uint16_t)(PELORUS_ABI_MAJOR + 1u);
    (void)memcpy(blob + PELORUS_SIDEDATA_UUID_LEN, &hdr, sizeof(hdr));

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
    PelorusSideData hdr;
    PelorusSectionDir dir0;
    uint8_t *dir0_bytes;

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
    (void)memcpy(&hdr, blob + PELORUS_SIDEDATA_UUID_LEN, sizeof(hdr));
    dir0_bytes = blob + PELORUS_SIDEDATA_UUID_LEN + hdr.header_size;
    (void)memcpy(&dir0, dir0_bytes, sizeof(dir0));
    dir0.offset += 4u;
    (void)memcpy(dir0_bytes, &dir0, sizeof(dir0));

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

/* Fill the per-cell QP map with a recognizable ramp. This map is never read
 * back by this test (the fold path is exercised separately in
 * test_qp_report_fold); split out of test_qp_report_roundtrip purely to keep
 * that function's line count within the readability-function-size budget
 * (ADR-0141). */
static void fill_qp_cellmap(int8_t *cellmap, uint16_t cells)
{
    for (int i = 0; i < (int)cells; i++) {
        cellmap[i] = (int8_t)(20 + (i % 12)); /* a recognizable QP ramp */
    }
}

/* Build the PEL_SEC_QPREPORT scalars for test_qp_report_roundtrip. Split out
 * for the same line-budget reason as fill_qp_cellmap above. */
static PelorusQpReportSection build_qp_report_section(uint16_t cells)
{
    PelorusQpReportSection qp;

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
    qp.qp_cell_offset = 0;   /* producer sets this when it appends cellmap */
    return qp;
}

/* Pack `qp` as the sole PEL_SEC_QPREPORT section and check the pack itself
 * succeeded. Split out for the same branch-budget reason documented on
 * build_roundtrip_blob above (CHECK is two branches per call, ADR-0141). */
static uint8_t *pack_qp_report_blob(PelorusQpReportSection *qp, size_t *out_len)
{
    PelorusSideData meta;
    PelorusPackSection sec;
    uint8_t *blob = NULL;

    fill_meta(&meta);
    sec.id = PEL_SEC_QPREPORT;
    sec.data = qp;
    sec.size = (uint32_t)sizeof(*qp);

    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, out_len) == PEL_OK);
    CHECK(blob != NULL);
    CHECK(pel_blob_is_present(blob, *out_len) == 1);
    return blob;
}

/* The bulk of the parsed-section scalar checks, split into two functions
 * (core / extra) for the same branch-budget reason as pack_qp_report_blob
 * above. */
static void check_qp_report_core_fields(const PelorusQpReportSection *r)
{
    CHECK(r->avg_qp == 27.5f);
    CHECK(r->psnr_y == 41.2f);
    CHECK(r->total_bits == 1234567ULL);
    CHECK(r->num_inter_blocks == 200);
    CHECK(r->honored_fraction == 0.75f);
    CHECK(r->report_source == PEL_QPSRC_QSV);
    CHECK(r->block_size_log2 == 4);
}

static void check_qp_report_extra_fields(const PelorusQpReportSection *r, uint16_t cells)
{
    CHECK(r->qp_valid == 1);
    CHECK(r->qp_cell_size == cells);
    CHECK(r->num_intra_blocks == 40);
    CHECK(r->num_skipped_blocks == 16);
    CHECK(r->psnr_u == 0.0f);
    CHECK(r->psnr_v == 0.0f);
}

/* PEL_SEC_QPREPORT (f): pack the encoder-honored QP readback with a per-cell
 * QP map appended after the blob, parse it back, verify scalars + the map. */
static void test_qp_report_roundtrip(void)
{
    const uint16_t cells = 16 * 9; /* matches fill_meta grid */
    int8_t cellmap[16 * 9];
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;

    fill_qp_cellmap(cellmap, cells);
    (void)cellmap;

    /* Pack the section and verify the scalars + the map offset/size fields
     * round-trip (the per-cell map payload itself is appended by the producer
     * after pack, like every other map section — see docs/api/interop-abi.md;
     * the fold path is exercised separately in test_qp_report_fold). */
    PelorusQpReportSection qp = build_qp_report_section(cells);
    uint8_t *blob = pack_qp_report_blob(&qp, &len);

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_QPREPORT, sizeof(PelorusQpReportSection), &p,
                                &got) == PEL_OK);
    CHECK(got == sizeof(PelorusQpReportSection));
    {
        const PelorusQpReportSection *r = p;
        check_qp_report_core_fields(r);
        check_qp_report_extra_fields(r, cells);
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
/* Pack `conf` as the sole PEL_SEC_MOTION_CONF section and check the pack
 * itself succeeded. Split out of test_motion_conf_roundtrip for the same
 * branch-budget reason documented on build_roundtrip_blob above (CHECK is
 * two branches per call, ADR-0141). */
static uint8_t *pack_motion_conf_blob(const PelorusMotionConfSection *conf, size_t *out_len)
{
    PelorusSideData meta;
    PelorusPackSection sec;
    uint8_t *blob = NULL;

    fill_meta(&meta);
    sec.id = PEL_SEC_MOTION_CONF;
    sec.data = conf;
    sec.size = (uint32_t)sizeof(*conf);

    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, out_len) == PEL_OK);
    CHECK(blob != NULL);
    CHECK(pel_blob_is_present(blob, *out_len) == 1);
    return blob;
}

/* The parsed-section field checks + R4/R3 fallback checks for
 * test_motion_conf_roundtrip. Split out for the same branch-budget reason as
 * pack_motion_conf_blob above. */
static void check_motion_conf_fields(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

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
}

/* Production usage: vf_pelorus_mc writes MOTION and MOTION_CONF together —
 * both must round-trip from one blob regardless of dir[] ordering. Split out
 * for the same branch-budget reason as pack_motion_conf_blob above. */
static void check_motion_conf_combined_section(const PelorusMotionConfSection *conf,
                                               const PelorusSideData *meta)
{
    PelorusMotionSection mo;
    PelorusPackSection both[2];
    uint8_t *b2 = NULL;
    size_t l2 = 0;
    const void *p = NULL;
    size_t got = 0;

    memset(&mo, 0, sizeof(mo));
    both[0].id = PEL_SEC_MOTION;
    both[0].data = &mo;
    both[0].size = (uint32_t)sizeof(mo);
    both[1].id = PEL_SEC_MOTION_CONF;
    both[1].data = conf;
    both[1].size = (uint32_t)sizeof(*conf);
    CHECK(pel_blob_pack(meta, both, 2, &b2, &l2) == PEL_OK);
    CHECK(pel_blob_find_section(b2, l2, PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p, &got) ==
          PEL_OK);
    CHECK(pel_blob_find_section(b2, l2, PEL_SEC_MOTION_CONF, sizeof(PelorusMotionConfSection), &p,
                                &got) == PEL_OK);
    CHECK(got == sizeof(PelorusMotionConfSection));
    pel_blob_free(b2);
}

/* PEL_SEC_MOTION_CONF (g): pack the per-block MV confidence section, parse it
 * back, verify the offset/size/metric fields round-trip; a consumer that knows
 * only the offset/size (not conf_metric) still parses (R4); and a section we did
 * not write is absent (R3). */
static void test_motion_conf_roundtrip(void)
{
    PelorusSideData meta;
    PelorusMotionConfSection conf;
    size_t len = 0;
    uint8_t *blob;

    fill_meta(&meta);
    memset(&conf, 0, sizeof(conf));
    conf.conf_field_offset = 0;    /* producer patches once it appends the map */
    conf.conf_field_size = 16 * 9; /* matches fill_meta grid (uint8 per cell)  */
    conf.conf_metric = PEL_MOTION_CONF_SAD;

    blob = pack_motion_conf_blob(&conf, &len);
    check_motion_conf_fields(blob, len);
    pel_blob_free(blob);

    check_motion_conf_combined_section(&conf, &meta);
}

/* PEL_SEC_COMPLEXITY (h): pack the per-frame complexity scalar, round-trip the
 * fields, and confirm R4 (an older consumer that knows only the first float
 * still parses). */
/* The four scalar-field checks for test_complexity_roundtrip. Split out for
 * the same branch-budget reason documented on build_roundtrip_blob above
 * (CHECK is two branches per call, ADR-0141). */
static void check_complexity_fields(const PelorusComplexitySection *r)
{
    CHECK(r->complexity == 0.625f);
    CHECK(r->texture_energy == 0.5f);
    CHECK(r->motion_component == 0.25f);
    CHECK(r->has_scene_cut == 1);
}

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
        check_complexity_fields(r);
    }
    /* R4: a consumer that knows only `complexity` (first 4 bytes) still parses. */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_COMPLEXITY, 4, &p, &got) == PEL_OK);
    CHECK(got == 4);
    pel_blob_free(blob);
}

/* Fill every element of `arr` with `value`. Split out of test_qp_report_fold
 * so that function stays within the readability-function-size branch budget:
 * a `for` loop is one branch on top of whatever runs inside it (ADR-0141). */
static void fill_uniform_i8(int8_t *arr, size_t n, int8_t value)
{
    for (size_t i = 0; i < n; i++) {
        arr[i] = value;
    }
}

/* Assert every element of `arr` equals `expect`. Split out for the same
 * branch-budget reason as fill_uniform_i8 above. */
static void check_uniform_i8(const int8_t *arr, size_t n, int8_t expect)
{
    for (size_t i = 0; i < n; i++) {
        CHECK(arr[i] == expect); /* uniform block QP -> uniform cell QP */
    }
}

/* The block-grid fold path: uniform block QP folds to a uniform per-cell QP.
 * Split out for the same branch-budget reason as fill_uniform_i8 above. */
static void check_qp_fold_from_blocks(const PelorusQpReportInput *in, int8_t *cells,
                                      size_t cells_len)
{
    PelorusQpReportSection out;

    CHECK(pel_qp_report_from_blocks(in, 4, 2, &out, cells, cells_len) == PEL_OK);
    CHECK(out.qp_valid == 1);
    CHECK(out.qp_cell_size == 4u * 2u);
    CHECK(out.report_source == PEL_QPSRC_QSV);
    CHECK(out.avg_qp == 30.0f);
    check_uniform_i8(cells, cells_len, 30);
}

/* Frame-stats-only path: no block grid -> qp_valid 0, scalars preserved.
 * Split out for the same branch-budget reason as fill_uniform_i8 above. */
static void check_qp_fold_frame_stats_only(PelorusQpReportInput *in)
{
    PelorusQpReportSection out;

    in->block_qp = NULL;
    CHECK(pel_qp_report_from_blocks(in, 4, 2, &out, NULL, 0) == PEL_OK);
    CHECK(out.qp_valid == 0);
    CHECK(out.qp_cell_size == 0u);
    CHECK(out.avg_qp == 30.0f);
}

/* Too-small buffer / NULL-input / zero-grid guards. Split out for the same
 * branch-budget reason as fill_uniform_i8 above. */
static void check_qp_fold_error_guards(const PelorusQpReportInput *in, int8_t *cells,
                                       size_t cells_len)
{
    PelorusQpReportSection out;

    /* Too-small output buffer is rejected, not overrun. */
    CHECK(pel_qp_report_from_blocks(in, 4, 2, &out, cells, 3) == PEL_ERR_RANGE);

    /* NULL inputs / zero grid are rejected. */
    CHECK(pel_qp_report_from_blocks(NULL, 4, 2, &out, cells, cells_len) == PEL_ERR_INVALID);
    CHECK(pel_qp_report_from_blocks(in, 0, 2, &out, cells, cells_len) == PEL_ERR_INVALID);
}

/* The reader stub: fold a per-block QP grid onto the cell grid. With a block
 * grid that is an integer multiple of the cell grid, each cell averages a clean
 * block tile, so a uniform block QP folds to the same per-cell QP. */
static void test_qp_report_fold(void)
{
    PelorusQpReportInput in;
    int8_t blocks[8 * 4];
    int8_t cells[4 * 2];

    memset(&in, 0, sizeof(in));
    fill_uniform_i8(blocks, sizeof(blocks), 30); /* uniform actual QP across all blocks */
    in.block_qp = blocks;
    in.blk_cols = 8;
    in.blk_rows = 4;
    in.block_size_log2 = 4;
    in.report_source = PEL_QPSRC_QSV;
    in.avg_qp = 30.0f;
    in.num_inter_blocks = 32;

    check_qp_fold_from_blocks(&in, cells, sizeof(cells));
    check_qp_fold_frame_stats_only(&in);

    in.block_qp = blocks;
    check_qp_fold_error_guards(&in, cells, sizeof(cells));
}

/* Write `csv` to `path`. Returns 1 on success, 0 if fopen failed (in which
 * case the caller must not proceed — matches the bare `return` the original,
 * unsplit test_x265_csv_reader took on this path). Split out of
 * test_x265_csv_reader so that function stays within the
 * readability-function-size line/branch budget (CHECK is two branches per
 * call, ADR-0141). */
static int write_x265_csv_fixture(const char *path, const char *csv)
{
    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL);
    if (fp == NULL) {
        return 0;
    }
    CHECK(fputs(csv, fp) >= 0);
    CHECK(fclose(fp) == 0);
    return 1;
}

/* Parse `path` into `frames` (capacity `cap`) and check it succeeded with
 * exactly the 3 coded rows. Split out for the same reason as
 * write_x265_csv_fixture above. */
static size_t parse_x265_csv_ok(const char *path, PelorusX265Frame *frames, size_t cap)
{
    size_t count = 0;

    CHECK(pel_x265_csv_parse(path, frames, cap, &count) == PEL_OK);
    CHECK(count == 3);
    return count;
}

/* The per-frame field checks on the 3 parsed rows. Split out for the same
 * reason as write_x265_csv_fixture above. */
static void check_x265_frames(const PelorusX265Frame *frames)
{
    CHECK(frames[0].slice_type == 'I');
    CHECK(frames[0].qp == 26.0f);
    CHECK(frames[0].bits == 24000ULL);
    CHECK(frames[1].slice_type == 'P');
    CHECK(frames[2].qp == 34.0f);
    CHECK(frames[2].psnr_v == 35.5f);
}

/* Fold, no requested map: bit-weighted mean QP, summed bits, qp_valid 0.
 * Split out for the same reason as write_x265_csv_fixture above. */
static void check_x265_fold_no_request(const PelorusX265Frame *frames, size_t count)
{
    PelorusQpReportSection qp;

    CHECK(pel_qp_report_from_x265_frames(frames, count, NULL, &qp) == PEL_OK);
    CHECK(qp.qp_valid == 0);
    CHECK(qp.report_source == PEL_QPSRC_NONE);
    CHECK(qp.total_bits == 35000ULL);
    /* bit-weighted: (26*24000 + 30*8000 + 34*3000)/35000 = 27.6 exactly. */
    CHECK(qp.avg_qp > 27.55f && qp.avg_qp < 27.65f);
    CHECK(qp.psnr_y > 41.0f && qp.psnr_y < 43.2f); /* I-frame-dominated */
    CHECK(qp.honored_fraction == 0.0f);            /* no request to compare */
}

/* The two honored_fraction cases (flat request vs shaped request). Split out
 * for the same reason as write_x265_csv_fixture above. */
static void check_x265_honored_fraction(const PelorusX265Frame *frames, size_t count)
{
    PelorusQpReportSection qp;

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
}

/* Truncation / missing-file / NULL-argument guards. Split out for the same
 * reason as write_x265_csv_fixture above. */
static void check_x265_error_guards(const char *path, PelorusX265Frame *frames)
{
    size_t count = 0;
    PelorusQpReportSection qp;

    /* A capacity smaller than the row count truncates and reports RANGE. */
    CHECK(pel_x265_csv_parse(path, frames, 2, &count) == PEL_ERR_RANGE);
    CHECK(count == 2);

    /* A missing file is ABSENT, not a crash. */
    CHECK(pel_x265_csv_parse("pelorus_no_such_file.csv", frames, 8, &count) == PEL_ERR_ABSENT);

    /* NULL guards. */
    CHECK(pel_x265_csv_parse(NULL, frames, 8, &count) == PEL_ERR_INVALID);
    CHECK(pel_qp_report_from_x265_frames(NULL, 1, NULL, &qp) == PEL_ERR_INVALID);
    CHECK(pel_qp_report_from_x265_frames(frames, 0, NULL, &qp) == PEL_ERR_INVALID);
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
    const char *path = "pelorus_x265_csv_test.csv";

    if (!write_x265_csv_fixture(path, csv)) {
        return;
    }

    /* Parse: 3 coded frames, the "Total frames" aggregate row dropped. */
    size_t count = parse_x265_csv_ok(path, frames, 8);
    check_x265_frames(frames);

    check_x265_fold_no_request(frames, count);
    check_x265_honored_fraction(frames, count);
    check_x265_error_guards(path, frames);

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
