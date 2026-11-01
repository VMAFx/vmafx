/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * feature/float_psnr_rows.h against the CPU float_psnr extractor
 * (ADR-1499). No device is needed.
 *
 * float_psnr.c adds each row's squared differences into a double, exact in
 * any order, and the rows into one double, which rounds past 2^53 units of
 * 1 / scaler^2. The GPU twins add each segment of a row as an integer and
 * the host forms each row's exact sum and adds the rows in order with
 * vmaf_float_psnr_row_noise(). This test forms the twins' segment sums on the
 * host (segments of 256 pixels of one row, as the kernels lay them out), runs
 * the helper and the hosts' score formula, and compares the score with the
 * CPU extractor's bit for bit on the cases of float_psnr_twin_parity.h past
 * 2^53 and on a 7680x4320 frame. It also checks that the frame total rounded
 * once, which the twins returned before, is another score wherever the case
 * says the CPU rounds.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "test.h"
#include "float_bits.h"

#include "feature/float_psnr_rows.h"
#include "float_psnr_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define SEGMENT 256u
#define PEAK_16BIT 255.99609375
#define PSNR_MAX_16BIT 108.0

/* The twins' segment sums of frame 0 of the case, row-major, `per_row` per
 * row. NULL when a picture cannot be made. */
static uint64_t *segment_sums(const FloatPsnrTwinCase *c, unsigned *per_row)
{
    VmafPicture ref;
    VmafPicture dis;
    const bool target = c->target != 0u;
    if (float_psnr_twin_fill(&ref, c, c->ref_lo, c->ref_hi, 1u)) {
        return NULL;
    }
    if (float_psnr_twin_fill(&dis, c, target ? 0u : c->dis_lo, target ? 0u : c->dis_hi, 8u)) {
        (void)vmaf_picture_unref(&ref);
        return NULL;
    }
    *per_row = (c->w + SEGMENT - 1u) / SEGMENT;
    uint64_t *seg = calloc((size_t)c->h * *per_row, sizeof(uint64_t));
    for (unsigned row = 0; row < c->h && seg; row++) {
        const uint16_t *r =
            (const uint16_t *)((const uint8_t *)ref.data[0] + ((size_t)row * ref.stride[0]));
        const uint16_t *d =
            (const uint16_t *)((const uint8_t *)dis.data[0] + ((size_t)row * dis.stride[0]));
        for (unsigned col = 0; col < c->w; col++) {
            seg[((size_t)row * *per_row) + (col / SEGMENT)] +=
                float_psnr_twin_float_square((int)r[col] - (int)d[col]);
        }
    }
    (void)vmaf_picture_unref(&ref);
    (void)vmaf_picture_unref(&dis);
    return seg;
}

/* The hosts' formula from a sum in units of 1 / 256^2 (16 bits). */
static double score_of(double units, const FloatPsnrTwinCase *c)
{
    const double noise = (units / (256.0 * 256.0)) / ((double)c->w * (double)c->h);
    const double eps = 1e-10;
    const double score = 10.0 * log10(PEAK_16BIT * PEAK_16BIT / (noise > eps ? noise : eps));
    return score < PSNR_MAX_16BIT ? score : PSNR_MAX_16BIT;
}

/* The CPU extractor's score of frame 0 of the case. */
static int cpu_score(const FloatPsnrTwinCase *c, double *score)
{
    double scores[FLOAT_PSNR_TWIN_FRAMES] = {0.0};
    const int err = float_psnr_twin_scores(NULL, NULL, c, scores);
    *score = scores[0];
    return err;
}

/* Mismatches of the helper against the CPU on one case, and of the old
 * frame total against the case's expectation. */
static unsigned case_mismatches(const FloatPsnrTwinCase *c)
{
    unsigned per_row = 0u;
    uint64_t *seg = segment_sums(c, &per_row);
    double cpu = 0.0;
    if (!seg || cpu_score(c, &cpu)) {
        free(seg);
        return 1u;
    }
    uint64_t total = 0u;
    for (size_t i = 0; i < (size_t)c->h * per_row; i++) {
        total += seg[i];
    }
    const double rows = score_of(vmaf_float_psnr_row_noise(seg, c->h, per_row), c);
    const double once = score_of((double)total, c);
    free(seg);
    unsigned bad = 0u;
    if (!vmaf_test_identical_f64(rows, cpu)) {
        bad++;
        (void)fprintf(stderr, "\n%s: rows %.17g, CPU %.17g", c->name, rows, cpu);
    }
    const bool once_differs = !vmaf_test_identical_f64(once, cpu);
    if (once_differs != c->rounds) {
        bad++;
        (void)fprintf(stderr, "\n%s: the frame total rounded once %s the CPU's score", c->name,
                      c->rounds ? "equals" : "differs from");
    }
    return bad;
}

static char *test_rows_equal_cpu_past_2_53(void)
{
    unsigned bad = 0u;
    for (size_t k = 0; k < FLOAT_PSNR_TWIN_PAST_CASE_COUNT; k++) {
        bad += case_mismatches(&FLOAT_PSNR_TWIN_PAST_CASES[k]);
    }
    mu_assert("the rows' sum is not the CPU's on the parity cases", bad == 0u);
    return NULL;
}

static char *test_rows_equal_cpu_8k(void)
{
    static const FloatPsnrTwinCase c = {"16-bit 7680x4320 noise",
                                        7680u,
                                        4320u,
                                        16u,
                                        0u,
                                        65535u,
                                        0u,
                                        65535u,
                                        false,
                                        NULL,
                                        0u,
                                        0u,
                                        true};
    mu_assert("the rows' sum is not the CPU's at 7680x4320", case_mismatches(&c) == 0u);
    return NULL;
}

/* A row's partial sums stay exact: segments that add to more than 2^53 in
 * one row are not a float_psnr frame, and the helper still adds rows in
 * order. */
static char *test_row_order(void)
{
    const uint64_t seg[4] = {(uint64_t)1 << 53, 1u, 1u, 1u};
    mu_assert("rows are added in order",
              vmaf_test_identical_f64(vmaf_float_psnr_row_noise(seg, 4u, 1u),
                                      (double)((uint64_t)1 << 53)));
    mu_assert("segments of a row are added exactly",
              vmaf_test_identical_f64(vmaf_float_psnr_row_noise(seg, 1u, 4u),
                                      (double)(((uint64_t)1 << 53) + 3u)));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_row_order);
    mu_run_test(test_rows_equal_cpu_past_2_53);
    mu_run_test(test_rows_equal_cpu_8k);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
