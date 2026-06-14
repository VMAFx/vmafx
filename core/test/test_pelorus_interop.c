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
 * conformance fixture (VMAFx/pelorus@835e097 test/interop_test.c).
 *
 * Both repos run byte-for-byte the same 7 checks against their own copy of
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

static int g_fail;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
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
    test_deband_params();

    if (g_fail != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return EXIT_FAILURE;
    }
    printf("interop: all checks passed (libpelorus %s, ABI %u.%u)\n", pelorus_version_string(),
           PELORUS_ABI_MAJOR, PELORUS_ABI_MINOR);
    return EXIT_SUCCESS;
}
