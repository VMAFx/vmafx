/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  T-UPSTREAM-930 regression — integer-ADM `angle_flag` predicate parity
 *  (ADR-1194, docs/state.md).
 *
 *  The fork used to carry four different evaluations of one 1-degree angle
 *  test:
 *
 *    CPU / AVX2 / AVX-512 : narrow the int64 operands to float, compare in
 *                           double                     <- golden-frozen form
 *    CUDA / HIP scale 0   : compare the *exact* int64 products in double
 *    SYCL                 : the whole comparison in float
 *    Metal scale 0        : exact int64 products narrowed to float
 *
 *  `angle_flag` selects the enhancement-gain-limited branch of decouple(), so
 *  a flipped flag moves the adm scores. The four forms disagree on any
 *  near-parallel band quadruple whose operands run past the 24-bit binary32
 *  significand — roughly 3e-5 of scale-0 pixels.
 *
 *  This test pins the single shared predicate in
 *  core/src/feature/adm_angle_flag.h:
 *
 *    1. the fp64 helper still spells the golden expression, on the specific
 *       quadruples where the legacy GPU forms diverged;
 *    2. the fp64-free int64 helper (used by SYCL, mirrored in MSL for Metal,
 *       neither of which can execute a binary64 instruction) returns exactly
 *       the same answer, over a directed corpus plus a randomised sweep that
 *       walks the cos(1deg)^2 boundary one integer at a time;
 *    3. the legacy forms really are the divergent ones, so the corpus keeps
 *       its teeth if someone reintroduces them.
 *
 *  Host-only: no GPU, no device runtime.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "adm_angle_flag.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but the Windows
 * MSVC legs compile the test tree with cl.exe, whose documented /std:clatest
 * C23 feature set does not include `nullptr`. Same carve-out and reasoning as
 * core/test/test_compat_clz.c. ADR-1138. */

/* ------------------------------------------------------------------ */
/*  The three legacy GPU spellings, kept verbatim as the anti-oracle.  */
/* ------------------------------------------------------------------ */

/* CUDA / HIP decouple_angle_flag_s0 before ADR-1194. */
static int legacy_gpu_exact_f64(int64_t ot_dp, int64_t o_mag_sq, int64_t t_mag_sq, float c)
{
    return (ot_dp >= 0) && (double)(ot_dp * ot_dp) >= (double)(o_mag_sq * t_mag_sq) * (double)c;
}

/* SYCL integer_adm_sycl.cpp before ADR-1194. */
static int legacy_gpu_all_f32(int64_t ot_dp, int64_t o_mag_sq, int64_t t_mag_sq, float c)
{
    const float ot_f = (float)ot_dp / 4096.0f;
    const float om_f = (float)o_mag_sq / 4096.0f;
    const float tm_f = (float)t_mag_sq / 4096.0f;
    return (ot_f >= 0.0f) && ((ot_f * ot_f) >= ((c * om_f) * tm_f));
}

/* Metal iadm_angle_flag_s0 before ADR-1194. */
static int legacy_gpu_exact_f32(int64_t ot_dp, int64_t o_mag_sq, int64_t t_mag_sq, float c)
{
    return (ot_dp >= 0) && ((float)(ot_dp * ot_dp) >= (float)(o_mag_sq * t_mag_sq) * c);
}

/* ------------------------------------------------------------------ */
/*  Directed corpus: scale-0 (int16) band quadruples on which at least  */
/*  one legacy GPU form disagreed with the golden CPU expression.       */
/* ------------------------------------------------------------------ */

struct band_quad {
    int32_t oh, ov, th, tv;
    int expect; /* golden angle_flag */
};

static const struct band_quad divergent_quads[] = {
    /* From the T-UPSTREAM-930 sweep quoted in docs/state.md. */
    {-11037, -15188, -12400, -16452, 1}, {-14118, -1840, -12911, -1454, 1},
    {-14532, -7262, -15386, -7356, 1},   {1599, -17748, 1285, -17718, 1},
    {19858, 32399, 19305, 32767, 1},     {13202, -691, 13542, -472, 0},
    {-20373, 12815, -20407, 13339, 0},   {-13681, 17685, -13424, 17993, 0},
    {19732, 14734, 19451, 15060, 1},     {31578, 10682, 32013, 10210, 1},
    {-8718, -32429, -8126, -32477, 0},   {5336, 7969, 5658, 8139, 0},
    {30548, 15302, 31016, 14865, 1},
};

#define N_QUADS ((int)(sizeof(divergent_quads) / sizeof(divergent_quads[0])))

static void quad_operands(const struct band_quad *q, int64_t *ot, int64_t *om, int64_t *tm)
{
    *ot = (int64_t)q->oh * q->th + (int64_t)q->ov * q->tv;
    *om = (int64_t)q->oh * q->oh + (int64_t)q->ov * q->ov;
    *tm = (int64_t)q->th * q->th + (int64_t)q->tv * q->tv;
}

/* cos(1deg)^2 exactly as integer_adm.c computes it at run time. */
static float golden_cos_1deg_sq(void)
{
    return (float)(cos(1.0 * M_PI / 180.0) * cos(1.0 * M_PI / 180.0));
}

/* ------------------------------------------------------------------ */

static char *test_cos_constant_is_one_float_everywhere(void)
{
    const float from_libm = golden_cos_1deg_sq();

    mu_assert("cos(1deg)^2 literal must equal the run-time value the CPU path uses",
              from_libm == ADM_ANGLE_FLAG_COS_1DEG_SQ);
    /* The int64 helper hard-codes the significand of that float; if the
     * constant ever moves, MC/D must move with it. */
    mu_assert("ADM_ANGLE_FLAG_MC must be the exact significand of the constant",
              (double)ADM_ANGLE_FLAG_COS_1DEG_SQ * 16777216.0 == (double)ADM_ANGLE_FLAG_MC);
    mu_assert("ADM_ANGLE_FLAG_D must be 2^24 - MC",
              ADM_ANGLE_FLAG_D == UINT64_C(16777216) - ADM_ANGLE_FLAG_MC);
    return NULL;
}

static char *test_golden_expression_on_divergent_quads(void)
{
    const float c = golden_cos_1deg_sq();

    for (int i = 0; i < N_QUADS; i++) {
        int64_t ot = 0;
        int64_t om = 0;
        int64_t tm = 0;
        quad_operands(&divergent_quads[i], &ot, &om, &tm);
        mu_assert("adm_angle_flag_fp64 must reproduce the golden angle_flag",
                  adm_angle_flag_fp64(ot, om, tm, c) == divergent_quads[i].expect);
    }
    return NULL;
}

/* The corpus is only a regression corpus if the legacy forms fail on it. */
static char *test_legacy_gpu_forms_are_the_divergent_ones(void)
{
    const float c = golden_cos_1deg_sq();
    int exact_f64_diffs = 0;
    int all_f32_diffs = 0;
    int exact_f32_diffs = 0;

    for (int i = 0; i < N_QUADS; i++) {
        int64_t ot = 0;
        int64_t om = 0;
        int64_t tm = 0;
        quad_operands(&divergent_quads[i], &ot, &om, &tm);
        const int golden = divergent_quads[i].expect;
        exact_f64_diffs += (legacy_gpu_exact_f64(ot, om, tm, c) != golden);
        all_f32_diffs += (legacy_gpu_all_f32(ot, om, tm, c) != golden);
        exact_f32_diffs += (legacy_gpu_exact_f32(ot, om, tm, c) != golden);
    }

    mu_assert("legacy CUDA/HIP scale-0 form must disagree with the golden one here",
              exact_f64_diffs > 0);
    mu_assert("legacy SYCL all-float form must disagree with the golden one here",
              all_f32_diffs > 0);
    mu_assert("legacy Metal exact-product-to-float form must disagree here", exact_f32_diffs > 0);
    return NULL;
}

/* This is the assertion that goes red if the shipped SYCL / Metal predicate
 * stops matching the golden CPU one. */
static char *test_i64_helper_matches_golden_on_divergent_quads(void)
{
    const float c = golden_cos_1deg_sq();

    for (int i = 0; i < N_QUADS; i++) {
        int64_t ot = 0;
        int64_t om = 0;
        int64_t tm = 0;
        quad_operands(&divergent_quads[i], &ot, &om, &tm);
        mu_assert("adm_angle_flag_i64 must match the golden angle_flag",
                  adm_angle_flag_i64(ot, om, tm) == divergent_quads[i].expect);
        mu_assert("adm_angle_flag_i64 must match adm_angle_flag_fp64",
                  adm_angle_flag_i64(ot, om, tm) == adm_angle_flag_fp64(ot, om, tm, c));
    }
    return NULL;
}

/* Operands that sit on a rounding boundary of the int64 -> float narrowing, on
 * a binade edge of the integer reformulation, or outside the domain the caller
 * can actually produce. */
static const int64_t edge_operands[] = {
    INT64_MIN,
    -((int64_t)1 << 40),
    -5,
    -1,
    0,
    1,
    2,
    3,
    ((int64_t)1 << 23) - 1,
    (int64_t)1 << 23,
    ((int64_t)1 << 24) - 1,
    (int64_t)1 << 24,
    ((int64_t)1 << 24) + 1,
    ((int64_t)1 << 24) + 3,
    ((int64_t)1 << 31) - 1,
    (int64_t)1 << 31,
    (int64_t)1 << 52,
    ((int64_t)1 << 53) - 1,
    (int64_t)1 << 62,
    INT64_MAX,
};

#define N_EDGES ((int)(sizeof(edge_operands) / sizeof(edge_operands[0])))

static char *test_i64_helper_handles_degenerate_operands(void)
{
    const float c = golden_cos_1deg_sq();

    for (int i = 0; i < N_EDGES * N_EDGES * N_EDGES; i++) {
        const int64_t ot = edge_operands[i / (N_EDGES * N_EDGES)];
        const int64_t om = edge_operands[(i / N_EDGES) % N_EDGES];
        const int64_t tm = edge_operands[i % N_EDGES];

        mu_assert("adm_angle_flag_i64 must match adm_angle_flag_fp64 on edge operands",
                  adm_angle_flag_i64(ot, om, tm) == adm_angle_flag_fp64(ot, om, tm, c));
    }
    return NULL;
}

/* xoroshiro128+ — deterministic, no libc rand() (banned, principles.md
 * rule 30) and identical on every platform so a failure reproduces. */
static uint64_t rng_state[2] = {UINT64_C(0x9E3779B97F4A7C15), UINT64_C(0xBF58476D1CE4E5B9)};

static uint64_t rng_next(void)
{
    const uint64_t s0 = rng_state[0];
    uint64_t s1 = rng_state[1];
    const uint64_t result = s0 + s1;

    s1 ^= s0;
    rng_state[0] = ((s0 << 55) | (s0 >> 9)) ^ s1 ^ (s1 << 14);
    rng_state[1] = (s1 << 36) | (s1 >> 28);
    return result;
}

/* Scale-0 sweep: int16 bands, distorted vector jittered off the reference so
 * a large share of samples lands within a degree of it. */
static char *test_i64_helper_matches_golden_on_scale0_sweep(void)
{
    const float c = golden_cos_1deg_sq();

    for (int i = 0; i < 400000; i++) {
        const int64_t oh = (int64_t)(rng_next() % 65536u) - 32768;
        const int64_t ov = (int64_t)(rng_next() % 65536u) - 32768;
        int64_t th = oh + ((int64_t)(rng_next() % 1201u) - 600);
        int64_t tv = ov + ((int64_t)(rng_next() % 1201u) - 600);

        th = th < -32768 ? -32768 : (th > 32767 ? 32767 : th);
        tv = tv < -32768 ? -32768 : (tv > 32767 ? 32767 : tv);

        const int64_t ot = oh * th + ov * tv;
        const int64_t om = oh * oh + ov * ov;
        const int64_t tm = th * th + tv * tv;

        mu_assert("adm_angle_flag_i64 must match adm_angle_flag_fp64 on scale-0 bands",
                  adm_angle_flag_i64(ot, om, tm) == adm_angle_flag_fp64(ot, om, tm, c));
    }
    return NULL;
}

/* Boundary walk: pick magnitudes across the whole int64 range (scales 1-3 use
 * int32 bands, so o_mag_sq / t_mag_sq reach far past 2^31), then step the dot
 * product one integer at a time through the value that satisfies the
 * comparison with equality. Every rounding decision in the helper is
 * exercised there. */
static char *test_i64_helper_matches_golden_on_boundary_walk(void)
{
    const float c = golden_cos_1deg_sq();

    for (int i = 0; i < 40000; i++) {
        const int wo = 1 + (int)(rng_next() % 62u);
        const int wt = 1 + (int)(rng_next() % 62u);
        int64_t om = (int64_t)(rng_next() & ((UINT64_C(1) << wo) - 1));
        int64_t tm = (int64_t)(rng_next() & ((UINT64_C(1) << wt) - 1));

        if (om <= 0) {
            om = 1;
        }
        if (tm <= 0) {
            tm = 1;
        }

        const double boundary = sqrt((double)c * (double)om * (double)tm);
        if (boundary > 9.0e18) {
            continue;
        }

        for (int d = -6; d <= 6; d++) {
            const int64_t ot = (int64_t)boundary + d;
            if (ot < 0) {
                continue;
            }
            mu_assert("adm_angle_flag_i64 must match adm_angle_flag_fp64 at the 1-degree boundary",
                      adm_angle_flag_i64(ot, om, tm) == adm_angle_flag_fp64(ot, om, tm, c));
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cos_constant_is_one_float_everywhere);
    mu_run_test(test_golden_expression_on_divergent_quads);
    mu_run_test(test_legacy_gpu_forms_are_the_divergent_ones);
    mu_run_test(test_i64_helper_matches_golden_on_divergent_quads);
    mu_run_test(test_i64_helper_handles_degenerate_operands);
    mu_run_test(test_i64_helper_matches_golden_on_scale0_sweep);
    mu_run_test(test_i64_helper_matches_golden_on_boundary_walk);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
