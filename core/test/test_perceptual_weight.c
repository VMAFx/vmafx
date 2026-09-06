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
 * test_perceptual_weight.c — golden-gate isolation proof for the Pelorus-driven
 * perceptual spatial-pooling weighting (ADR-1118, integration plan B).
 *
 * The #1 requirement is golden-gate isolation: the weighting must be INERT
 * unless BOTH (a) perceptual weighting is enabled AND (b) Pelorus side-data is
 * present for the frame. The Netflix golden 3 pairs carry no side-data and MUST
 * score bit-exact. These tests pin that:
 *
 *   1. test_pooling_unweighted_without_sidedata — inject per-frame feature
 *      scores via vmaf_import_feature_score and pool them; WITHOUT registering
 *      any side-data the pooled result is BIT-EXACT to the hand-computed
 *      unweighted mean / harmonic-mean. (Proves the no-side-data path is
 *      byte-identical to upstream.)
 *
 *   2. test_enabled_but_no_sidedata_is_inert — enabling weighting alone, with
 *      no side-data registered, leaves pooling bit-exact (condition (b) false).
 *
 *   3. test_sidedata_but_disabled_is_inert — registering side-data while
 *      weighting is disabled leaves pooling bit-exact (condition (a) false).
 *
 *   4. test_weighting_applies_with_sidedata — with weighting enabled AND a
 *      synthetic per-cell banding blob registered for the high-scoring frame,
 *      the weighted mean differs from the unweighted mean in the expected
 *      direction and matches the closed-form weighted average.
 *
 *   5. test_grid_zero_degrades_to_scalar — a blob with grid==0 (today's deband
 *      placeholder) degrades to a frame-level scalar weight (never crashes) and
 *      still shifts the pooled score.
 *
 *   6. test_robustness_foreign_and_bad_abi — a foreign buffer is ignored
 *      (-ENOENT) and an ABI-major-mismatch blob is rejected (-EPROTO); both
 *      leave the frame unweighted.
 *
 * Uses vmaf_import_feature_score + vmaf_feature_score_pooled — no model, no YUV,
 * mirroring test_context.c's test_get_feature_score.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/perceptual_weight.h"
#include "libvmaf/pelorus/interop.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FEAT "feat_pw"

/* Bit-exact equality for doubles produced by the SAME arithmetic on both sides
 * of the comparison (no rounding slack — that is the whole point of the
 * golden-isolation proof). The compared values are integer-valued and exactly
 * representable, so a direct `==` is the precise predicate. */
static int bit_exact(double a, double b)
{
    return a == b;
}

/* Inject three per-frame feature scores. */
static int inject_scores(VmafContext *vmaf, double s0, double s1, double s2)
{
    int err = vmaf_import_feature_score(vmaf, FEAT, s0, 0);
    err |= vmaf_import_feature_score(vmaf, FEAT, s1, 1);
    err |= vmaf_import_feature_score(vmaf, FEAT, s2, 2);
    return err;
}

/*
 * Build a synthetic UUID-prefixed Pelorus blob carrying a banding section with
 * a real per-cell risk map. Layout (manually, matching interop.c's packer):
 *   [uuid 16][header 48][dir[1] 16][banding section 24 (8-aligned)][cell map].
 * `risk_byte` fills every cell, so the per-cell mean is risk_byte/255. Returns a
 * malloc'd blob the caller frees; *out_len is the full length. cols/rows set the
 * grid (0,0 => frame-level scalar path).
 */
static uint8_t *build_banding_blob(uint16_t cols, uint16_t rows, uint8_t risk_byte,
                                   float global_risk, size_t *out_len)
{
    const uint32_t n_cells = (uint32_t)cols * (uint32_t)rows;
    const uint32_t header_size = (uint32_t)sizeof(PelorusSideData);
    const uint32_t dir_size = (uint32_t)sizeof(PelorusSectionDir);
    const uint32_t sect_size = (uint32_t)sizeof(PelorusBandingSection);
    /* header + 1 dir entry, already a multiple of 8. */
    const uint32_t sect_off = header_size + dir_size;
    const uint32_t sect_padded = (sect_size + 7u) & ~7u;
    const uint32_t map_off = sect_off + sect_padded;
    const uint32_t total_size = map_off + n_cells;
    const size_t blob_len = (size_t)PELORUS_SIDEDATA_UUID_LEN + (size_t)total_size;

    uint8_t *blob = calloc(1, blob_len);
    if (blob == NULL) {
        return NULL;
    }

    /* Build each struct in a local and memcpy it into the flat image — the blob
     * is only byte-aligned, so we never cast a byte pointer to a struct (keeps
     * the wire-image construction lint-clean and matches how a real producer
     * pel_blob_pack lays out the bytes). */
    memcpy(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN);

    PelorusSideData hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, PELORUS_MAGIC_STR, PELORUS_MAGIC_LEN);
    hdr.abi_major = (uint16_t)PELORUS_ABI_MAJOR;
    hdr.abi_minor = (uint16_t)PELORUS_ABI_MINOR;
    hdr.total_size = total_size;
    hdr.section_mask = PEL_SEC_BANDING;
    hdr.section_count = 1;
    hdr.header_size = (uint16_t)header_size;
    hdr.frame_pts = 0;
    hdr.plane_layout = PEL_LAYOUT_420;
    hdr.bit_depth = 8;
    hdr.grid_cols = cols;
    hdr.grid_rows = rows;
    hdr.producer_id = PEL_FOURCC('T', 'E', 'S', 'T');
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN, &hdr, sizeof(hdr));

    PelorusSectionDir dir;
    memset(&dir, 0, sizeof(dir));
    dir.section_id = PEL_SEC_BANDING;
    dir.offset = sect_off;
    dir.size = sect_size;
    dir.struct_minor = (uint32_t)PELORUS_ABI_MINOR;
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN + header_size, &dir, sizeof(dir));

    PelorusBandingSection band;
    memset(&band, 0, sizeof(band));
    band.global_banding_risk = global_risk;
    band.flat_area_fraction = 1.0f;
    if (n_cells > 0) {
        band.cell_data_offset = map_off;
        band.cell_data_size = n_cells;
    }
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN + sect_off, &band, sizeof(band));

    if (n_cells > 0) {
        memset(blob + PELORUS_SIDEDATA_UUID_LEN + map_off, risk_byte, n_cells);
    }

    *out_len = blob_len;
    return blob;
}

/*
 * Build a synthetic blob carrying a banding section (per-cell risk map, every
 * cell == risk_byte) AND a PEL_SEC_COMPLEXITY section with the given
 * `complexity` scalar. Layout (matching interop.c's 8-aligned packer):
 *   [uuid 16][header 48][dir[2] 32][banding 24->pad 24][complexity 16][cell map].
 * Both section structs are already 8-aligned in size (24, 16), so no inter-
 * section padding is needed. Returns a malloc'd blob the caller frees.
 *
 * This is the load-bearing fixture: with the SAME banding map, toggling the
 * complexity section (or its value) must change the derived weight, while a blob
 * WITHOUT the complexity section reproduces the legacy banding-only weight.
 */
static uint8_t *build_banding_complexity_blob(uint16_t cols, uint16_t rows, uint8_t risk_byte,
                                              float global_risk, float complexity, size_t *out_len)
{
    const uint32_t n_cells = (uint32_t)cols * (uint32_t)rows;
    const uint32_t header_size = (uint32_t)sizeof(PelorusSideData);
    const uint32_t dir_size = (uint32_t)sizeof(PelorusSectionDir);
    const uint32_t band_size = (uint32_t)sizeof(PelorusBandingSection);
    const uint32_t cx_size = (uint32_t)sizeof(PelorusComplexitySection);
    /* header + 2 dir entries, already a multiple of 8. */
    const uint32_t band_off = header_size + 2u * dir_size;
    const uint32_t band_padded = (band_size + 7u) & ~7u;
    const uint32_t cx_off = band_off + band_padded;
    const uint32_t cx_padded = (cx_size + 7u) & ~7u;
    const uint32_t map_off = cx_off + cx_padded;
    const uint32_t total_size = map_off + n_cells;
    const size_t blob_len = (size_t)PELORUS_SIDEDATA_UUID_LEN + (size_t)total_size;

    uint8_t *blob = calloc(1, blob_len);
    if (blob == NULL) {
        return NULL;
    }

    memcpy(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN);

    PelorusSideData hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, PELORUS_MAGIC_STR, PELORUS_MAGIC_LEN);
    hdr.abi_major = (uint16_t)PELORUS_ABI_MAJOR;
    hdr.abi_minor = (uint16_t)PELORUS_ABI_MINOR;
    hdr.total_size = total_size;
    hdr.section_mask = PEL_SEC_BANDING | PEL_SEC_COMPLEXITY;
    hdr.section_count = 2;
    hdr.header_size = (uint16_t)header_size;
    hdr.plane_layout = PEL_LAYOUT_420;
    hdr.bit_depth = 8;
    hdr.grid_cols = cols;
    hdr.grid_rows = rows;
    hdr.producer_id = PEL_FOURCC('T', 'E', 'S', 'T');
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN, &hdr, sizeof(hdr));

    PelorusSectionDir dir[2];
    memset(dir, 0, sizeof(dir));
    dir[0].section_id = PEL_SEC_BANDING;
    dir[0].offset = band_off;
    dir[0].size = band_size;
    dir[0].struct_minor = (uint32_t)PELORUS_ABI_MINOR;
    dir[1].section_id = PEL_SEC_COMPLEXITY;
    dir[1].offset = cx_off;
    dir[1].size = cx_size;
    dir[1].struct_minor = (uint32_t)PELORUS_ABI_MINOR;
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN + header_size, dir, sizeof(dir));

    PelorusBandingSection band;
    memset(&band, 0, sizeof(band));
    band.global_banding_risk = global_risk;
    band.flat_area_fraction = 1.0f;
    if (n_cells > 0) {
        band.cell_data_offset = map_off;
        band.cell_data_size = n_cells;
    }
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN + band_off, &band, sizeof(band));

    PelorusComplexitySection cx;
    memset(&cx, 0, sizeof(cx));
    cx.complexity = complexity;
    cx.texture_energy = complexity;
    cx.motion_component = 0.0f;
    cx.has_scene_cut = 0;
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN + cx_off, &cx, sizeof(cx));

    if (n_cells > 0) {
        memset(blob + PELORUS_SIDEDATA_UUID_LEN + map_off, risk_byte, n_cells);
    }

    *out_len = blob_len;
    return blob;
}

/* Flip a packed blob's ABI major to force the R6 rejection path, in place.
 * Reads/writes via memcpy so no byte pointer is cast to PelorusSideData*. */
static void corrupt_abi_major(uint8_t *blob)
{
    PelorusSideData hdr;
    memcpy(&hdr, blob + PELORUS_SIDEDATA_UUID_LEN, sizeof(hdr));
    hdr.abi_major = (uint16_t)(PELORUS_ABI_MAJOR + 1u);
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN, &hdr, sizeof(hdr));
}

static char *test_pooling_unweighted_without_sidedata(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);
    mu_assert("inject failed", inject_scores(vmaf, 60.0, 80.0, 100.0) == 0);

    double mean = -1.0;
    mu_assert("pool MEAN failed",
              vmaf_feature_score_pooled(vmaf, FEAT, VMAF_POOL_METHOD_MEAN, &mean, 0, 2) == 0);
    /* Identical arithmetic to the implementation's unweighted branch. */
    double expect_mean = (60.0 + 80.0 + 100.0) / 3u;
    mu_assert("unweighted MEAN must be bit-exact", bit_exact(mean, expect_mean));

    double hmean = -1.0;
    mu_assert("pool HARMONIC failed",
              vmaf_feature_score_pooled(vmaf, FEAT, VMAF_POOL_METHOD_HARMONIC_MEAN, &hmean, 0, 2) ==
                  0);
    double i_sum = 1. / 61. + 1. / 81. + 1. / 101.;
    double expect_h = (double)3u / i_sum - 1.0;
    mu_assert("unweighted HARMONIC must be bit-exact", bit_exact(hmean, expect_h));

    vmaf_close(vmaf);
    return NULL;
}

static char *test_enabled_but_no_sidedata_is_inert(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);
    mu_assert("enable failed", vmaf_set_perceptual_weight_enabled(vmaf, 1) == 0);
    mu_assert("inject failed", inject_scores(vmaf, 60.0, 80.0, 100.0) == 0);

    double mean = -1.0;
    mu_assert("pool MEAN failed",
              vmaf_feature_score_pooled(vmaf, FEAT, VMAF_POOL_METHOD_MEAN, &mean, 0, 2) == 0);
    /* Condition (b) is false (no side-data), so weighting is inert. */
    mu_assert("enabled-but-no-sidedata MEAN must be bit-exact",
              bit_exact(mean, (60.0 + 80.0 + 100.0) / 3u));

    vmaf_close(vmaf);
    return NULL;
}

static char *test_sidedata_but_disabled_is_inert(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);
    mu_assert("inject failed", inject_scores(vmaf, 60.0, 80.0, 100.0) == 0);

    /* Register a high-risk blob for frame 2 but leave weighting DISABLED. */
    size_t blob_len = 0;
    uint8_t *blob = build_banding_blob(4, 4, 255, 1.0f, &blob_len);
    mu_assert("blob alloc", blob != NULL);
    int rc = vmaf_set_perceptual_sidedata(vmaf, blob, blob_len, 2);
    free(blob); /* summary is copied; blob no longer needed */
    mu_assert("register side-data", rc == 0);

    double mean = -1.0;
    mu_assert("pool MEAN failed",
              vmaf_feature_score_pooled(vmaf, FEAT, VMAF_POOL_METHOD_MEAN, &mean, 0, 2) == 0);
    /* Condition (a) is false (disabled), so weighting is inert. */
    mu_assert("sidedata-but-disabled MEAN must be bit-exact",
              bit_exact(mean, (60.0 + 80.0 + 100.0) / 3u));

    vmaf_close(vmaf);
    return NULL;
}

static char *test_weighting_applies_with_sidedata(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);
    mu_assert("enable failed", vmaf_set_perceptual_weight_enabled(vmaf, 1) == 0);
    mu_assert("strength failed", vmaf_set_perceptual_weight_strength(vmaf, 1.0) == 0);
    mu_assert("inject failed", inject_scores(vmaf, 60.0, 80.0, 100.0) == 0);

    /* Frame 2 (score 100) gets a maximal per-cell banding map: every cell 255,
     * flat_area_fraction 1.0. salience = banding(1.0) * modulation. With only a
     * banding section present (no variance), modulation is 1.0, so salience=1.0
     * and weight = 1 + 1.0*1.0 = 2.0. Frames 0 and 1 carry no side-data => w=1. */
    size_t blob_len = 0;
    uint8_t *blob = build_banding_blob(4, 4, 255, 1.0f, &blob_len);
    mu_assert("blob alloc", blob != NULL);
    int rc = vmaf_set_perceptual_sidedata(vmaf, blob, blob_len, 2);
    free(blob); /* summary is copied; blob no longer needed */
    mu_assert("register side-data", rc == 0);

    double mean = -1.0;
    mu_assert("pool MEAN failed",
              vmaf_feature_score_pooled(vmaf, FEAT, VMAF_POOL_METHOD_MEAN, &mean, 0, 2) == 0);

    /* Closed-form weighted mean: (1*60 + 1*80 + 2*100) / (1 + 1 + 2). */
    double expect = (1.0 * 60.0 + 1.0 * 80.0 + 2.0 * 100.0) / (1.0 + 1.0 + 2.0);
    mu_assert("weighted MEAN must match closed form", bit_exact(mean, expect));

    /* And it must differ from (and exceed) the unweighted mean, since the
     * highest-scoring frame is up-weighted. */
    double unweighted = (60.0 + 80.0 + 100.0) / 3.0;
    mu_assert("weighted MEAN must shift up", mean > unweighted);

    vmaf_close(vmaf);
    return NULL;
}

static char *test_grid_zero_degrades_to_scalar(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);
    mu_assert("enable failed", vmaf_set_perceptual_weight_enabled(vmaf, 1) == 0);
    mu_assert("inject failed", inject_scores(vmaf, 60.0, 80.0, 100.0) == 0);

    /* grid 0x0 placeholder (today's deband) — only the frame-level scalar. */
    size_t blob_len = 0;
    uint8_t *blob = build_banding_blob(0, 0, 0, 1.0f, &blob_len);
    mu_assert("blob alloc", blob != NULL);
    int rc = vmaf_set_perceptual_sidedata(vmaf, blob, blob_len, 2);
    free(blob); /* summary is copied; blob no longer needed */
    mu_assert("register side-data (grid0)", rc == 0);

    double mean = -1.0;
    mu_assert("pool MEAN failed",
              vmaf_feature_score_pooled(vmaf, FEAT, VMAF_POOL_METHOD_MEAN, &mean, 0, 2) == 0);
    /* Frame-level scalar salience = 0.75*1.0 + 0.25*(1.0*1.0) = 1.0, so frame 2
     * is up-weighted (weight 2.0). Must exceed the unweighted mean, proving the
     * grid==0 degrade path engages instead of crashing or no-op'ing. */
    mu_assert("grid-zero MEAN must shift up", mean > (60.0 + 80.0 + 100.0) / 3.0);

    vmaf_close(vmaf);
    return NULL;
}

static char *test_robustness_foreign_and_bad_abi(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);

    /* Foreign buffer (e.g. an x264 user-data SEI) => -ENOENT, frame unweighted. */
    uint8_t foreign[64];
    memset(foreign, 0xAB, sizeof(foreign));
    mu_assert("foreign buffer must be ignored",
              vmaf_set_perceptual_sidedata(vmaf, foreign, sizeof(foreign), 0) == -ENOENT);

    /* Corrupt the ABI major to force the R6 rejection path => -EPROTO. */
    size_t blob_len = 0;
    uint8_t *blob = build_banding_blob(4, 4, 255, 1.0f, &blob_len);
    mu_assert("blob alloc", blob != NULL);
    corrupt_abi_major(blob);
    int rc = vmaf_set_perceptual_sidedata(vmaf, blob, blob_len, 1);
    free(blob);
    mu_assert("ABI-major mismatch must be rejected", rc == -EPROTO);

    vmaf_close(vmaf);
    return NULL;
}

static char *test_argument_guards(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    mu_assert("NULL vmaf => -EINVAL",
              vmaf_set_perceptual_sidedata(NULL, buf, sizeof(buf), 0) == -EINVAL);
    mu_assert("NULL blob => -EINVAL", vmaf_set_perceptual_sidedata(vmaf, NULL, 0, 0) == -EINVAL);
    mu_assert("NULL vmaf (enable) => -EINVAL",
              vmaf_set_perceptual_weight_enabled(NULL, 1) == -EINVAL);
    mu_assert("NULL vmaf (strength) => -EINVAL",
              vmaf_set_perceptual_weight_strength(NULL, 1.0) == -EINVAL);
    mu_assert("negative strength => -EINVAL",
              vmaf_set_perceptual_weight_strength(vmaf, -1.0) == -EINVAL);
    mu_assert("NaN strength => -EINVAL", vmaf_set_perceptual_weight_strength(vmaf, NAN) == -EINVAL);
    mu_assert("Inf strength => -EINVAL",
              vmaf_set_perceptual_weight_strength(vmaf, INFINITY) == -EINVAL);
    mu_assert("zero strength ok", vmaf_set_perceptual_weight_strength(vmaf, 0.0) == 0);

    vmaf_close(vmaf);
    return NULL;
}

/*
 * Load-bearing complexity-modulation proof (ADR-1120, ABI 1.3).
 *
 * Same banding map in three pooled runs, distinguished only by the
 * PEL_SEC_COMPLEXITY section:
 *   - NO complexity section          -> salience 1.0, weight 2.0 (legacy)
 *   - complexity == 0.0              -> factor 1.0, weight 2.0 (identical)
 *   - complexity == 1.0             -> factor 0.5, weight 1.5 (attenuated)
 * The pooled mean strictly orders run(complexity=1) < run(no/0 complexity), so
 * the section is load-bearing: removing the consumer's complexity_modulation
 * (or the section) collapses the three runs to one. Closed-form checks pin the
 * exact arithmetic.
 */
static double pool_with_blob(uint8_t *blob, size_t blob_len)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    double mean = -1.0;

    if (vmaf_init(&vmaf, cfg) != 0) {
        return -1.0;
    }
    if (vmaf_set_perceptual_weight_enabled(vmaf, 1) != 0 ||
        vmaf_set_perceptual_weight_strength(vmaf, 1.0) != 0 ||
        inject_scores(vmaf, 60.0, 80.0, 100.0) != 0) {
        vmaf_close(vmaf);
        return -1.0;
    }
    if (blob != NULL && vmaf_set_perceptual_sidedata(vmaf, blob, blob_len, 2) != 0) {
        vmaf_close(vmaf);
        return -1.0;
    }
    if (vmaf_feature_score_pooled(vmaf, FEAT, VMAF_POOL_METHOD_MEAN, &mean, 0, 2) != 0) {
        vmaf_close(vmaf);
        return -1.0;
    }
    vmaf_close(vmaf);
    return mean;
}

static char *test_complexity_modulates_weight(void)
{
    /* Banding-only weight on frame 2: salience 1.0 -> weight 2.0. */
    size_t band_len = 0;
    uint8_t *band_blob = build_banding_blob(4, 4, 255, 1.0f, &band_len);
    mu_assert("banding blob alloc", band_blob != NULL);
    double mean_band_only = pool_with_blob(band_blob, band_len);
    free(band_blob);
    double expect_w2 = (1.0 * 60.0 + 1.0 * 80.0 + 2.0 * 100.0) / 4.0;
    mu_assert("banding-only weighted mean (w=2.0)", bit_exact(mean_band_only, expect_w2));

    /* Same banding map + complexity 0.0: factor 1.0 -> weight 2.0, IDENTICAL. */
    size_t cx0_len = 0;
    uint8_t *cx0_blob = build_banding_complexity_blob(4, 4, 255, 1.0f, 0.0f, &cx0_len);
    mu_assert("cx0 blob alloc", cx0_blob != NULL);
    double mean_cx0 = pool_with_blob(cx0_blob, cx0_len);
    free(cx0_blob);
    mu_assert("complexity==0 is identical to banding-only", bit_exact(mean_cx0, mean_band_only));

    /* Same banding map + complexity 1.0: factor (1 - 0.5*1) = 0.5 -> salience
     * 0.5 -> weight 1.5. Closed-form: (60 + 80 + 1.5*100) / (1 + 1 + 1.5). */
    size_t cx1_len = 0;
    uint8_t *cx1_blob = build_banding_complexity_blob(4, 4, 255, 1.0f, 1.0f, &cx1_len);
    mu_assert("cx1 blob alloc", cx1_blob != NULL);
    double mean_cx1 = pool_with_blob(cx1_blob, cx1_len);
    free(cx1_blob);
    double expect_w15 = (1.0 * 60.0 + 1.0 * 80.0 + 1.5 * 100.0) / (1.0 + 1.0 + 1.5);
    mu_assert("complexity==1 attenuates to weight 1.5", bit_exact(mean_cx1, expect_w15));

    /* Load-bearing: high complexity STRICTLY lowers the pooled mean vs no/zero
     * complexity (the up-weighted high-scoring frame is pulled back). */
    mu_assert("high complexity lowers pooled mean", mean_cx1 < mean_band_only);

    /* Monotone in complexity: a mid value (0.5 -> factor 0.75 -> weight 1.75)
     * lands strictly between the two extremes. */
    size_t cxm_len = 0;
    uint8_t *cxm_blob = build_banding_complexity_blob(4, 4, 255, 1.0f, 0.5f, &cxm_len);
    mu_assert("cxm blob alloc", cxm_blob != NULL);
    double mean_cxm = pool_with_blob(cxm_blob, cxm_len);
    free(cxm_blob);
    mu_assert("mid complexity between extremes", mean_cxm < mean_band_only && mean_cxm > mean_cx1);

    return NULL;
}

/* The complexity section is grid-independent: it modulates even the grid==0
 * frame-level-scalar path (today's deband placeholder). */
static char *test_complexity_modulates_grid_zero(void)
{
    /* grid 0x0, banding scalar salience = 1.0; no complexity -> weight 2.0. */
    size_t b_len = 0;
    uint8_t *b_blob = build_banding_blob(0, 0, 0, 1.0f, &b_len);
    mu_assert("grid0 banding blob alloc", b_blob != NULL);
    double mean_no_cx = pool_with_blob(b_blob, b_len);
    free(b_blob);

    /* grid 0x0 + complexity 1.0 -> factor 0.5 -> weight 1.5; strictly lower. */
    size_t c_len = 0;
    uint8_t *c_blob = build_banding_complexity_blob(0, 0, 0, 1.0f, 1.0f, &c_len);
    mu_assert("grid0 complexity blob alloc", c_blob != NULL);
    double mean_cx = pool_with_blob(c_blob, c_len);
    free(c_blob);

    mu_assert("grid0 high complexity lowers pooled mean", mean_cx < mean_no_cx);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_pooling_unweighted_without_sidedata);
    mu_run_test(test_enabled_but_no_sidedata_is_inert);
    mu_run_test(test_sidedata_but_disabled_is_inert);
    mu_run_test(test_weighting_applies_with_sidedata);
    mu_run_test(test_grid_zero_degrades_to_scalar);
    mu_run_test(test_robustness_foreign_and_bad_abi);
    mu_run_test(test_argument_guards);
    mu_run_test(test_complexity_modulates_weight);
    mu_run_test(test_complexity_modulates_grid_zero);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
