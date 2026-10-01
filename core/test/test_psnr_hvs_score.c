/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Host tail of calc_psnrhvs() for the GPU twins (ADR-1397):
 * feature/psnr_hvs_score.c must add the terms one by one into a single float,
 * across block boundaries, exactly as the CPU extractor does. Device-free.
 *
 * The fixtures put a term of 2^24 next to terms of 1. A float at 2^24 has a
 * spacing of 2, so `2^24 + 1` rounds back to 2^24: a running float sum that
 * meets the large term first absorbs every later 1, while a sum that is
 * widened, reordered or taken per block does not. Each expected value below
 * therefore holds for the CPU's order and type only.
 */

#include <limits.h>
#include <math.h>
#include <stddef.h>

#include "test.h"

#include "feature/psnr_hvs_score.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define TERMS VMAF_PSNR_HVS_TERMS_PER_BLOCK
#define TWO_BLOCKS ((size_t)2 * TERMS)
#define BIG 16777216.0f /* 2^24 */
#define PEAK_SQ_8 65025.0f
#define PEAK_SQ_10 1046529.0f
#define PEAK_SQ_12 16769025.0f

static void fill(float *terms, size_t count, float value)
{
    for (size_t i = 0; i < count; i++)
        terms[i] = value;
}

/* What calc_psnrhvs() returns for a running sum `sum` over `n_terms` terms. */
static double plane_score_of(float sum, size_t n_terms, float peak_sq)
{
    float ret = sum;
    ret /= (float)n_terms;
    ret /= peak_sq;
    return (double)ret;
}

static char *test_sum_is_one_running_float(void)
{
    float terms[TERMS];
    fill(terms, TERMS, 1.0f);
    terms[0] = BIG;
    /* 2^24, then 63 additions of 1 that each round back to 2^24. */
    const double score = vmaf_psnr_hvs_plane_score(terms, 1u, 8u);
    mu_assert("a running float sum absorbs every 1 after 2^24",
              score == plane_score_of(BIG, TERMS, PEAK_SQ_8));
    mu_assert("a double accumulator would keep the 63 ones",
              score != plane_score_of(BIG + 63.0f, TERMS, PEAK_SQ_8));
    return NULL;
}

static char *test_sum_follows_term_order(void)
{
    float terms[TERMS];
    fill(terms, TERMS, 1.0f);
    terms[TERMS - 1u] = BIG;
    /* 63 ones add exactly; 63 + 2^24 lies midway between two floats and
     * rounds to the even one, 2^24 + 64. */
    const double score = vmaf_psnr_hvs_plane_score(terms, 1u, 8u);
    mu_assert("the same terms in the opposite order give a different sum",
              score == plane_score_of(BIG + 64.0f, TERMS, PEAK_SQ_8));
    return NULL;
}

static char *test_sum_runs_across_blocks(void)
{
    float terms[TWO_BLOCKS];
    fill(terms, TWO_BLOCKS, 0.0f);
    terms[0] = BIG;
    fill(terms + TERMS, TERMS, 1.0f);
    /* Summing the second block on its own first gives 64, and 2^24 + 64 is
     * exact; the CPU's single running sum stays at 2^24. */
    const double score = vmaf_psnr_hvs_plane_score(terms, 2u, 8u);
    mu_assert("the running sum carries across block boundaries",
              score == plane_score_of(BIG, TWO_BLOCKS, PEAK_SQ_8));
    mu_assert("per-block subtotals would give 2^24 + 64",
              score != plane_score_of(BIG + 64.0f, TWO_BLOCKS, PEAK_SQ_8));
    return NULL;
}

static char *test_normalisation_per_depth(void)
{
    float terms[TERMS];
    fill(terms, TERMS, 0.0f);
    terms[0] = 64.0f * PEAK_SQ_8;
    mu_assert("8-bit: divide by 64 terms and 255^2",
              vmaf_psnr_hvs_plane_score(terms, 1u, 8u) == 1.0);
    terms[0] = 64.0f * PEAK_SQ_10;
    mu_assert("10-bit: divide by 1023^2", vmaf_psnr_hvs_plane_score(terms, 1u, 10u) == 1.0);
    terms[0] = PEAK_SQ_12;
    mu_assert("12-bit: divide by 4095^2",
              vmaf_psnr_hvs_plane_score(terms, 1u, 12u) == plane_score_of(1.0f, TERMS, 1.0f));
    mu_assert("1-bit input is the lower bound of the depth range",
              vmaf_psnr_hvs_plane_score(terms, 1u, 1u) == plane_score_of(PEAK_SQ_12, TERMS, 1.0f));
    return NULL;
}

static char *test_plane_score_rejects_bad_input(void)
{
    float terms[TERMS];
    fill(terms, TERMS, 1.0f);
    mu_assert("NULL terms", isnan(vmaf_psnr_hvs_plane_score(NULL, 1u, 8u)));
    mu_assert("no block", isnan(vmaf_psnr_hvs_plane_score(terms, 0u, 8u)));
    mu_assert("0-bit input", isnan(vmaf_psnr_hvs_plane_score(terms, 1u, 0u)));
    mu_assert("13-bit input", isnan(vmaf_psnr_hvs_plane_score(terms, 1u, 13u)));
    /* The CPU counts terms in an int; a count beyond it is refused before any
     * term is read. */
    const size_t too_many = (size_t)INT_MAX / TERMS + 1u;
    mu_assert("term count beyond int", isnan(vmaf_psnr_hvs_plane_score(terms, too_many, 8u)));
    return NULL;
}

static char *test_combined_score(void)
{
    const double planes[3] = {0.25, 0.5, 0.125};
    mu_assert("one plane: luma alone", vmaf_psnr_hvs_combined_score(planes, 1u) == 0.25);
    mu_assert("three planes: 0.8 Y + 0.1 (Cb + Cr)",
              vmaf_psnr_hvs_combined_score(planes, 3u) == 0.25 * .8 + .1 * (0.5 + 0.125));
    mu_assert("two planes are not a psnr_hvs layout",
              isnan(vmaf_psnr_hvs_combined_score(planes, 2u)));
    mu_assert("zero planes", isnan(vmaf_psnr_hvs_combined_score(planes, 0u)));
    mu_assert("NULL scores", isnan(vmaf_psnr_hvs_combined_score(NULL, 3u)));
    return NULL;
}

static char *test_score_db(void)
{
    mu_assert("a score of 1 is 0 dB", vmaf_psnr_hvs_score_db(1.0) == 0.0);
    mu_assert("a score of 0.1 is 10 dB", vmaf_psnr_hvs_score_db(0.1) == 10.0);
    const double identical = vmaf_psnr_hvs_score_db(0.0);
    mu_assert("identical planes score +inf, as on the CPU", isinf(identical) && identical > 0.0);
    mu_assert("NaN stays NaN", isnan(vmaf_psnr_hvs_score_db((double)NAN)));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_sum_is_one_running_float);
    mu_run_test(test_sum_follows_term_order);
    mu_run_test(test_sum_runs_across_blocks);
    mu_run_test(test_normalisation_per_depth);
    mu_run_test(test_plane_score_rejects_bad_input);
    mu_run_test(test_combined_score);
    mu_run_test(test_score_db);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
