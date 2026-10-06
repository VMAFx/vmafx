/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * T-METAL-PSNR-HVS-PER-BLOCK-SUM-FP32-MASK-2026-10-05 (ADR-1397, ADR-1401,
 * ADR-1498): psnr_hvs_metal's arithmetic, compiled on the host, against the
 * CPU psnr_hvs extractor.
 *
 * feature/metal/metal_psnr_hvs_math.h is the kernel's arithmetic, valid as
 * Metal Shading Language and as C. This test composes it as
 * integer_psnr_hvs.metal does (the variance ratios from the raw samples,
 * od_bin_fdct8x8() as eight column transforms into a scratch block and eight
 * more back, the masking energies, the block's threshold, the 64 stored
 * terms), forms the masking table as integer_psnr_hvs_metal.mm does
 * (vmaf_psnr_hvs_mask_value()), and adds the terms as its collect() does
 * (vmaf_psnr_hvs_plane_score(), vmaf_psnr_hvs_combined_score(),
 * vmaf_psnr_hvs_score_db()). Every output is compared with the CPU
 * extractor's through libvmaf, bit for bit, on the fixtures of
 * test_metal_integer_psnr_hvs_parity (psnr_hvs_twin_parity.h: 8 to 12 bits,
 * 4:0:0 / 4:2:0 / 4:2:2 / 4:4:4, enable_chroma=false, 3840x2160).
 *
 * The same composition with the two constructs the twin had before is
 * refused: the masking table as an fp32 product squared, and each block's
 * terms added into a float partial that the host adds up. Each, alone or
 * both, changes some outputs: the M4 Pro report of issue #2118 measured
 * both together on the device.
 *
 * What this cannot show: that the header compiles as MSL and returns these
 * bits on an Apple GPU (test_metal_integer_psnr_hvs_parity on the tester's
 * device).
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "psnr_hvs_twin_parity.h"

#include "feature/metal/metal_psnr_hvs_math.h"
#include "feature/psnr_hvs_score.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

enum {
    BLOCK = 8,
    STEP = 7,
    TERMS = (int)VMAF_MTL_HVS_TERMS,
    /* The twin as ported, then the planted defects: fp32 masking table,
     * per-block float partials, and both (the twin before the port). */
    VARIANTS = 4,
};

typedef struct HvsVariant {
    const char *what;
    int fp32_mask;
    int block_partials;
} HvsVariant;

static const HvsVariant VARIANT[VARIANTS] = {
    {"ported twin", 0, 0},
    {"fp32 masking table", 1, 0},
    {"per-block partials", 0, 1},
    {"fp32 masking table and per-block partials (the twin before the port)", 1, 1},
};

/* The cases of test_metal_integer_psnr_hvs_parity (psnr_hvs_twin_parity.h). */
static const HvsFixture FIXTURES[] = {
    {VMAF_PIX_FMT_YUV420P, 8u, FIXTURE_W, FIXTURE_H, HVS_PATTERN_RAMP, 0},
    {VMAF_PIX_FMT_YUV420P, 8u, FIXTURE_W, FIXTURE_H, HVS_PATTERN_NOISE, 0},
    {VMAF_PIX_FMT_YUV420P, 9u, 64u, 48u, HVS_PATTERN_MIXED, 0},
    {VMAF_PIX_FMT_YUV420P, 10u, 64u, 48u, HVS_PATTERN_MIXED, 0},
    {VMAF_PIX_FMT_YUV420P, 11u, 64u, 48u, HVS_PATTERN_MIXED, 0},
    {VMAF_PIX_FMT_YUV420P, 12u, 64u, 48u, HVS_PATTERN_MIXED, 0},
    {VMAF_PIX_FMT_YUV400P, 8u, 64u, 48u, HVS_PATTERN_MIXED, 0},
    {VMAF_PIX_FMT_YUV422P, 8u, 64u, 48u, HVS_PATTERN_MIXED, 0},
    {VMAF_PIX_FMT_YUV444P, 8u, 64u, 48u, HVS_PATTERN_MIXED, 0},
    {VMAF_PIX_FMT_YUV420P, 8u, FIXTURE_W, FIXTURE_H, HVS_PATTERN_NOISE, 1},
    {VMAF_PIX_FMT_YUV422P, 10u, 64u, 48u, HVS_PATTERN_MIXED, 1},
    {VMAF_PIX_FMT_YUV420P, 8u, 3840u, 2160u, HVS_PATTERN_NOISE, 0},
    {VMAF_PIX_FMT_YUV420P, 10u, 3840u, 2160u, HVS_PATTERN_NOISE, 0},
};

#define N_FIXTURES (sizeof(FIXTURES) / sizeof(FIXTURES[0]))

/* ------------------------------------------------------------------ */
/* The masking table                                                    */
/* ------------------------------------------------------------------ */

/* calc_psnrhvs()'s statement, verbatim but for the names: a float CSF value
 * times a double constant, squared in double, stored as float. */
static float cpu_mask_entry(float csf)
{
    float mask;
    mask = (csf * 0.3885746225901003) * (csf * 0.3885746225901003);
    return mask;
}

/* The shipped kernel's table before the port: an fp32 product, squared. */
static float fp32_mask_entry(float csf)
{
    const float m = csf * 0.3885746225901003f;
    return m * m;
}

static void mask_table(unsigned plane, int fp32, float mask[TERMS])
{
    for (int k = 0; k < TERMS; k++) {
        const float csf = vmaf_mtl_hvs_csf[plane][k];
        mask[k] = fp32 ? fp32_mask_entry(csf) : vmaf_psnr_hvs_mask_value(csf);
    }
}

static char *test_mask_table_is_the_cpus(void)
{
    unsigned wrong = 0u;
    unsigned fp32_differs = 0u;
    for (unsigned plane = 0u; plane < VMAF_MTL_HVS_PLANES; plane++) {
        for (int k = 0; k < TERMS; k++) {
            const float csf = vmaf_mtl_hvs_csf[plane][k];
            wrong += vmaf_psnr_hvs_mask_value(csf) != cpu_mask_entry(csf) ? 1u : 0u;
            fp32_differs += fp32_mask_entry(csf) != cpu_mask_entry(csf) ? 1u : 0u;
        }
    }
    (void)fprintf(stderr, "\n  fp32 masking table: %u of 192 entries differ from the CPU's\n",
                  fp32_differs);
    mu_assert("vmaf_psnr_hvs_mask_value() is not calc_psnrhvs()'s masking entry", wrong == 0u);
    mu_assert("the fp32 masking table no longer differs: the planted case is void",
              fp32_differs > 0u);
    mu_assert("a CSF value of 0 masks nothing", vmaf_psnr_hvs_mask_value(0.0f) == 0.0f);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The kernel, composed on the host                                     */
/* ------------------------------------------------------------------ */

static int sample_at(const VmafPicture *pic, unsigned plane, unsigned row, unsigned col)
{
    const uint8_t *line = (const uint8_t *)pic->data[plane] + (size_t)row * pic->stride[plane];
    return pic->bpc > 8u ? (int)((const uint16_t *)line)[col] : (int)line[col];
}

/* hvs_fdct8x8() of integer_psnr_hvs.metal: lane `lane` transforms column
 * `lane` of `blk` into row `lane` of `z`, then column `lane` of `z` into row
 * `lane` of `blk`. */
static void block_dct(int blk[TERMS])
{
    int z[TERMS];
    for (int lane = 0; lane < BLOCK; lane++) {
        VmafMtlHvsLine column;
        for (int r = 0; r < BLOCK; r++)
            column.v[r] = blk[(r * BLOCK) + lane];
        const VmafMtlHvsLine out = vmaf_mtl_hvs_fdct8(column);
        for (int r = 0; r < BLOCK; r++)
            z[(lane * BLOCK) + r] = out.v[r];
    }
    for (int lane = 0; lane < BLOCK; lane++) {
        VmafMtlHvsLine column;
        for (int r = 0; r < BLOCK; r++)
            column.v[r] = z[(r * BLOCK) + lane];
        const VmafMtlHvsLine out = vmaf_mtl_hvs_fdct8(column);
        for (int r = 0; r < BLOCK; r++)
            blk[(lane * BLOCK) + r] = out.v[r];
    }
}

/* hvs_variance_ratio() of integer_psnr_hvs.metal. */
static float block_ratio(const int blk[TERMS])
{
    VmafMtlHvsMoments sums = vmaf_mtl_hvs_moments_zero();
    for (int i = 0; i < BLOCK; i++) {
        for (int j = 0; j < BLOCK; j++)
            sums = vmaf_mtl_hvs_mean_add(sums, i, j, blk[(i * BLOCK) + j]);
    }
    const VmafMtlHvsMoments means = vmaf_mtl_hvs_means(sums);
    VmafMtlHvsMoments variances = vmaf_mtl_hvs_moments_zero();
    for (int i = 0; i < BLOCK; i++) {
        for (int j = 0; j < BLOCK; j++)
            variances = vmaf_mtl_hvs_variance_add(variances, means, i, j, blk[(i * BLOCK) + j]);
    }
    return vmaf_mtl_hvs_variance_ratio(variances);
}

/* hvs_mask_energy() of integer_psnr_hvs.metal: DC skipped. */
static float block_energy(const int coef[TERMS], const float mask[TERMS])
{
    float energy = 0.f;
    for (int k = 1; k < TERMS; k++)
        energy = vmaf_mtl_hvs_energy_add(energy, coef[k], mask[k]);
    return energy;
}

typedef struct HvsPlane {
    const VmafPicture *ref;
    const VmafPicture *dis;
    unsigned plane;
    unsigned blocks_x;
    unsigned blocks_y;
    const float *mask;
} HvsPlane;

/* psnr_hvs_block_terms() of integer_psnr_hvs.metal for the block at
 * (bx, by): its 64 terms, row-major. */
static void block_terms(const HvsPlane *pl, unsigned bx, unsigned by, float terms[TERMS])
{
    int s[TERMS];
    int d[TERMS];
    for (int k = 0; k < TERMS; k++) {
        const unsigned row = (by * STEP) + ((unsigned)k / BLOCK);
        const unsigned col = (bx * STEP) + ((unsigned)k % BLOCK);
        s[k] = sample_at(pl->ref, pl->plane, row, col);
        d[k] = sample_at(pl->dis, pl->plane, row, col);
    }
    const float ratio_s = block_ratio(s);
    const float ratio_d = block_ratio(d);
    block_dct(s);
    block_dct(d);
    const float threshold =
        vmaf_mtl_hvs_block_threshold(vmaf_mtl_hvs_threshold(block_energy(s, pl->mask), ratio_s),
                                     vmaf_mtl_hvs_threshold(block_energy(d, pl->mask), ratio_d));
    for (int k = 0; k < TERMS; k++) {
        terms[k] = vmaf_mtl_hvs_term(s[k], d[k], vmaf_mtl_hvs_csf[pl->plane][k], pl->mask[k],
                                     threshold, (vmaf_mtl_u32)k);
    }
}

/* The twin's host before the port: each block's terms added into a float
 * partial, the partials added into a float, divided as calc_psnrhvs()
 * divides. */
static double block_partials_score(const float *terms, size_t n_blocks, unsigned bpc)
{
    float total = 0.0f;
    for (size_t b = 0; b < n_blocks; b++) {
        float part = 0.0f;
        for (int k = 0; k < TERMS; k++)
            part += terms[(b * (size_t)TERMS) + (size_t)k];
        total += part;
    }
    const int samplemax = (1 << bpc) - 1;
    total /= (float)(n_blocks * (size_t)TERMS);
    total /= (float)(samplemax * samplemax);
    return (double)total;
}

/* One plane's 64 terms per block under one masking table, or NULL when the
 * buffer cannot be allocated. */
static float *plane_terms(const VmafPicture *ref, const VmafPicture *dis, unsigned plane,
                          int fp32_mask, size_t *n_blocks)
{
    float mask[TERMS];
    mask_table(plane, fp32_mask, mask);
    const HvsPlane pl = {ref,
                         dis,
                         plane,
                         ((ref->w[plane] - BLOCK) / STEP) + 1u,
                         ((ref->h[plane] - BLOCK) / STEP) + 1u,
                         mask};
    *n_blocks = (size_t)pl.blocks_x * pl.blocks_y;
    float *terms = malloc(*n_blocks * (size_t)TERMS * sizeof(float));
    if (!terms)
        return NULL;
    for (unsigned by = 0u; by < pl.blocks_y; by++) {
        for (unsigned bx = 0u; bx < pl.blocks_x; bx++)
            block_terms(&pl, bx, by, terms + (((size_t)by * pl.blocks_x) + bx) * (size_t)TERMS);
    }
    return terms;
}

/* The four outputs of the twin for one fixture under one masking table, once
 * per summation (index = HvsVariant.block_partials), laid out as
 * hvs_read_scores() lays out the CPU's: the chroma slots stay 0 for luma-only
 * input. The two summations read the same terms, so each table's terms are
 * formed once; a plane whose buffer cannot be allocated scores NaN. */
static void twin_scores(const VmafPicture *ref, const VmafPicture *dis, const HvsFixture *fx,
                        int fp32_mask, double scores[2][HVS_FEATURES])
{
    const unsigned n_planes = (fx->fmt == VMAF_PIX_FMT_YUV400P || fx->luma_only) ? 1u : 3u;
    double planes[2][3] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
    for (unsigned p = 0u; p < n_planes; p++) {
        size_t n_blocks = 0u;
        float *terms = plane_terms(ref, dis, p, fp32_mask, &n_blocks);
        planes[0][p] = terms ? vmaf_psnr_hvs_plane_score(terms, n_blocks, ref->bpc) : (double)NAN;
        planes[1][p] = terms ? block_partials_score(terms, n_blocks, ref->bpc) : (double)NAN;
        free(terms);
    }
    for (unsigned sum = 0u; sum < 2u; sum++) {
        for (unsigned p = 0u; p < 3u; p++)
            scores[sum][p] = p < n_planes ? vmaf_psnr_hvs_score_db(planes[sum][p]) : 0.0;
        scores[sum][3] =
            vmaf_psnr_hvs_score_db(vmaf_psnr_hvs_combined_score(planes[sum], n_planes));
    }
}

/* ------------------------------------------------------------------ */
/* The comparison                                                       */
/* ------------------------------------------------------------------ */

/* Outputs that differ from the CPU's, per fixture and variant. */
static unsigned differing[N_FIXTURES][VARIANTS];
static int compared;

static unsigned count_differing(const double cpu[HVS_FEATURES], const double twin[HVS_FEATURES])
{
    unsigned n = 0u;
    for (unsigned i = 0u; i < HVS_FEATURES; i++)
        n += hvs_score_bits(cpu[i]) != hvs_score_bits(twin[i]) ? 1u : 0u;
    return n;
}

static void report_fixture(const HvsFixture *fx, const double cpu[HVS_FEATURES],
                           const double twin[HVS_FEATURES])
{
    for (unsigned i = 0u; i < HVS_FEATURES; i++) {
        if (hvs_score_bits(cpu[i]) != hvs_score_bits(twin[i])) {
            (void)fprintf(stderr, "\n%ux%u %u-bit %s: cpu=%.17g twin=%.17g", fx->w, fx->h, fx->bpc,
                          hvs_features[i], cpu[i], twin[i]);
        }
    }
}

/* The CPU extractor and every variant of the twin on one fixture. */
static mu_message_t compare_fixture(size_t f)
{
    static const HvsTwin cpu_only = {"psnr_hvs", "host", NULL, NULL, NULL};
    const HvsFixture *fx = &FIXTURES[f];
    double cpu[HVS_FEATURES] = {0.0, 0.0, 0.0, 0.0};
    int unbuilt = 0;
    mu_assert_msg(hvs_score(&cpu_only, NULL, fx, cpu, &unbuilt));
    VmafPicture ref;
    VmafPicture dis;
    mu_assert("fill reference failed", !hvs_fill_pic(&ref, fx, 0u));
    if (hvs_fill_pic(&dis, fx, 1u)) {
        mu_assert("vmaf_picture_unref failed", vmaf_picture_unref(&ref) == 0);
        return "fill distorted failed";
    }
    double twin[2][2][HVS_FEATURES];
    for (int fp32_mask = 0; fp32_mask < 2; fp32_mask++)
        twin_scores(&ref, &dis, fx, fp32_mask, twin[fp32_mask]);
    for (unsigned v = 0u; v < VARIANTS; v++) {
        const double *out = twin[VARIANT[v].fp32_mask][VARIANT[v].block_partials];
        differing[f][v] = count_differing(cpu, out);
        if (v == 0u)
            report_fixture(fx, cpu, out);
    }
    const int unref_ref = vmaf_picture_unref(&ref);
    const int unref_dis = vmaf_picture_unref(&dis);
    mu_assert("vmaf_picture_unref failed", unref_ref == 0 && unref_dis == 0);
    return NULL;
}

static mu_message_t compare_all(void)
{
    if (compared)
        return NULL;
    for (size_t f = 0; f < N_FIXTURES; f++)
        mu_assert_msg(compare_fixture(f));
    compared = 1;
    return NULL;
}

static char *test_twin_is_the_cpu_extractor(void)
{
    mu_assert_msg(compare_all());
    unsigned wrong = 0u;
    for (size_t f = 0; f < N_FIXTURES; f++)
        wrong += differing[f][0];
    if (wrong != 0u)
        (void)fprintf(stderr, "\n  ported twin: %u outputs differ from the CPU's\n", wrong);
    mu_assert("the twin's psnr_hvs on the host differs from the CPU extractor", wrong == 0u);
    return NULL;
}

static char *test_planted_defects_are_refused(void)
{
    mu_assert_msg(compare_all());
    for (unsigned v = 1u; v < VARIANTS; v++) {
        unsigned changed = 0u;
        unsigned fixtures = 0u;
        for (size_t f = 0; f < N_FIXTURES; f++) {
            changed += differing[f][v];
            fixtures += differing[f][v] != 0u ? 1u : 0u;
        }
        (void)fprintf(stderr, "\n  %s: %u outputs on %u of %u fixtures differ", VARIANT[v].what,
                      changed, fixtures, (unsigned)N_FIXTURES);
        mu_assert("a planted defect leaves every output equal to the CPU's", changed != 0u);
    }
    (void)fprintf(stderr, "\n");
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_mask_table_is_the_cpus);
    mu_run_test(test_twin_is_the_cpu_extractor);
    mu_run_test(test_planted_defects_are_refused);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
