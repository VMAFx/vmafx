/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 */

/*
 * float_motion_hip adds its SAD in the CPU's order (ADR-1409), checked
 * without an AMD device.
 *
 * The kernels of float_motion_score.hip compute every absolute difference
 * and every row sum through feature/hip/float_motion/float_motion_rows.h,
 * and float_motion_hip.c turns the row sums into a plane score through the
 * host helper of the same header.
 * This test compiles those lines for the host and holds the plane score
 * against motion.c::compute_motion(), the function the CPU extractor calls,
 * with and without `motion_add_scale1`, bit for bit.
 *
 * Before, the twin summed each 16x16 block on the device and the blocks in
 * double on the host: closer to the exact sum, and 3e-6 to 1.4e-4 from the
 * CPU on a gfx1036. The last test shows that this fixture tells the two
 * reductions apart.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/hip/float_motion/float_motion_rows.h"
#include "feature/motion.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FM_BLOCK 16u

/* Two packed float planes, as the blur kernel leaves them. */
typedef struct FmPair {
    unsigned w;
    unsigned h;
    float *cur;
    float *prev;
} FmPair;

static uint32_t fm_lcg(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state >> 8;
}

static void fm_pair_free(FmPair *p)
{
    free(p->cur);
    free(p->prev);
    p->cur = NULL;
    p->prev = NULL;
}

/* Blurred-picture values: a ramp in [-128, 127] with a fractional part, and a
 * second frame that differs by up to a few units, so the absolute differences
 * have full fp32 mantissas and the running sums round at every step. */
static int fm_pair_fill(FmPair *p, unsigned w, unsigned h, uint32_t seed)
{
    p->w = w;
    p->h = h;
    p->cur = malloc((size_t)w * h * sizeof(float));
    p->prev = malloc((size_t)w * h * sizeof(float));
    if (!p->cur || !p->prev) {
        fm_pair_free(p);
        return -1;
    }
    uint32_t state = seed;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            const size_t i = ((size_t)y * w) + x;
            const float base = (float)(((x * 3u) + (y * 5u)) & 255u) - 128.0f;
            const float fine = (float)(fm_lcg(&state) & 0xFFFFu) * (1.0f / 65536.0f);
            const float move = ((float)(fm_lcg(&state) & 0xFFFu) * (1.0f / 512.0f)) - 4.0f;
            p->cur[i] = base + fine;
            p->prev[i] = base + fine + move;
        }
    }
    return 0;
}

/* The row sums of a `w` x `h` plane as the kernels form them: every sample's
 * absolute difference into the transposed plane (the blur kernel when
 * `half` is NULL, the scale-1 kernel otherwise), then the row kernel. */
static float *fm_twin_rows(const FmPair *p, const VmafHipFloatMotionScale1 *half, unsigned w,
                           unsigned h)
{
    float *diff = malloc(vmaf_hip_float_motion_diff_count(w, h) * sizeof(float));
    float *rows = malloc((size_t)h * sizeof(float));
    if (!diff || !rows) {
        free(diff);
        free(rows);
        return NULL;
    }
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            const size_t i = ((size_t)y * w) + x;
            diff[vmaf_hip_float_motion_diff_index(x, y, w)] =
                half ? vmaf_hip_float_motion_scale1_abs_diff(half, x, y) :
                       vmaf_hip_float_motion_abs_diff(p->cur[i], p->prev[i]);
        }
    }
    for (unsigned y = 0; y < h; y++) {
        rows[y] = vmaf_hip_float_motion_row_sum(diff, w, y);
    }
    free(diff);
    return rows;
}

/* The plane score as the twin forms it: the kernels' row sums, then the host
 * helper. */
static int fm_twin_score(const FmPair *p, bool scale1, double *score)
{
    /* motion.c::vmaf_image_sad_c(): (int)(width * 0.5 + 0.5). */
    const unsigned sw = (unsigned)(((double)p->w * 0.5) + 0.5);
    const unsigned sh = (unsigned)(((double)p->h * 0.5) + 0.5);
    const VmafHipFloatMotionScale1 half = {p->cur, p->prev, p->w, p->h, sw, sh};
    float *rows = fm_twin_rows(p, NULL, p->w, p->h);
    float *rows1 = scale1 ? fm_twin_rows(p, &half, sw, sh) : NULL;
    const bool ok = rows != NULL && (rows1 != NULL || !scale1);
    if (ok) {
        *score = vmaf_hip_float_motion_plane_score(rows, p->w, p->h, rows1, sw, sh);
    }
    free(rows);
    free(rows1);
    return ok ? 0 : -1;
}

/* The CPU extractor's score of the same two planes. */
static int fm_cpu_score(const FmPair *p, bool scale1, double *score)
{
    const int stride = (int)((size_t)p->w * sizeof(float));
    return compute_motion(p->cur, p->prev, (int)p->w, (int)p->h, stride, stride, score,
                          scale1 ? 1 : 0);
}

static uint64_t fm_bits(double v)
{
    uint64_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* 0 when the twin's score of a `w` x `h` pair is the CPU's bit for bit, 1 when
 * it differs (reported), -1 when a run failed. */
static int fm_compare(unsigned w, unsigned h, bool scale1, uint32_t seed)
{
    FmPair p = {0};
    double cpu = 0.0;
    double twin = 0.0;
    int err = fm_pair_fill(&p, w, h, seed);
    if (!err) {
        err = fm_cpu_score(&p, scale1, &cpu);
    }
    if (!err) {
        err = fm_twin_score(&p, scale1, &twin);
    }
    fm_pair_free(&p);
    if (err || !(cpu > 0.0)) {
        return -1;
    }
    if (fm_bits(cpu) == fm_bits(twin)) {
        return 0;
    }
    (void)fprintf(stderr, "\n%ux%u scale1=%d: cpu=%.17g twin=%.17g", w, h, (int)scale1, cpu, twin);
    return 1;
}

static char *test_row_sums_match_cpu(void)
{
    mu_assert("640x360: the row sums are not compute_motion()'s score",
              fm_compare(640u, 360u, false, 1u) == 0);
    mu_assert("1920x1080: the row sums are not compute_motion()'s score",
              fm_compare(1920u, 1080u, false, 2u) == 0);
    mu_assert("333x127 (odd): the row sums are not compute_motion()'s score",
              fm_compare(333u, 127u, false, 3u) == 0);
    mu_assert("3x3 (the blur's minimum): the row sums are not compute_motion()'s score",
              fm_compare(3u, 3u, false, 4u) == 0);
    return NULL;
}

/* motion_add_scale1: the half-size bilinear planes, their row sums, and the
 * fp32 sum of the two means. Odd sizes make the scaler mirror at the right
 * and bottom edge. */
static char *test_scale1_row_sums_match_cpu(void)
{
    mu_assert("640x360 scale1: not compute_motion()'s score",
              fm_compare(640u, 360u, true, 5u) == 0);
    mu_assert("1920x1080 scale1: not compute_motion()'s score",
              fm_compare(1920u, 1080u, true, 6u) == 0);
    mu_assert("333x127 scale1 (odd): not compute_motion()'s score",
              fm_compare(333u, 127u, true, 7u) == 0);
    mu_assert("3x3 scale1: not compute_motion()'s score", fm_compare(3u, 3u, true, 8u) == 0);
    return NULL;
}

/* The fp32 sum of one 16x16 block's absolute differences, rows in order. */
static float fm_block_sum(const FmPair *p, unsigned bx, unsigned by)
{
    float block = 0.0f;
    for (unsigned y = by; y < by + FM_BLOCK && y < p->h; y++) {
        for (unsigned x = bx; x < bx + FM_BLOCK && x < p->w; x++) {
            const size_t i = ((size_t)y * p->w) + x;
            block += vmaf_hip_float_motion_abs_diff(p->cur[i], p->prev[i]);
        }
    }
    return block;
}

/* The reduction the twin used before ADR-1409: one fp32 sum per 16x16 block,
 * the blocks added in double. */
static double fm_block_score(const FmPair *p)
{
    double total = 0.0;
    for (unsigned by = 0; by < p->h; by += FM_BLOCK) {
        for (unsigned bx = 0; bx < p->w; bx += FM_BLOCK) {
            total += (double)fm_block_sum(p, bx, by);
        }
    }
    return total / ((double)p->w * (double)p->h);
}

/* The comparison has teeth: on this fixture the per-block reduction is not
 * the CPU's score. */
static char *test_block_reduction_is_detected(void)
{
    FmPair p = {0};
    double cpu = 0.0;
    mu_assert("fixture allocation failed", fm_pair_fill(&p, 1920u, 1080u, 2u) == 0);
    const int err = fm_cpu_score(&p, false, &cpu);
    const double blocks = fm_block_score(&p);
    fm_pair_free(&p);
    mu_assert("compute_motion() failed", err == 0);
    mu_assert("a per-block sum gives the CPU's score: the fixture cannot tell the orders apart",
              blocks != cpu);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_row_sums_match_cpu);
    mu_run_test(test_scale1_row_sums_match_cpu);
    mu_run_test(test_block_reduction_is_detected);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
