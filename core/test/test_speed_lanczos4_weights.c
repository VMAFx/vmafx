/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The lanczos4 prescale weight table the device-resident SpEED twins read
 *  (T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30).
 *
 *  vif_scale_frame_s() evaluates each lanczos4 weight in fp64 with sin() and
 *  rounds it once. A device that evaluated the weights itself in fp32 was a
 *  few ulp off on some of them, and SpEED turned that into a score difference
 *  above the ADR-0214 tolerance on smooth content. The weights depend only on
 *  the output column and row, so the host now evaluates them once per run
 *  with the scaler's own routine, vif_scale_lanczos4_axis_weights(), and the
 *  scale kernel reads the table that speed_internal_gpu_lanczos_weights()
 *  lays out. No device is needed to check that:
 *
 *    positive  a plane resampled with the table, in the scale kernel's
 *              operation order (speed_score.cu scale_lanczos()), equals
 *              vif_scale_frame_s() bit for bit at 0.5x, 2x and two ratios
 *              that are not a power of two;
 *    boundary  an unscaled axis (every fraction is 0, so the centre tap is
 *              exactly 1), a single output sample, and an empty axis, which
 *              must not write;
 *    negative  the table helpers refuse a NULL argument, a size that is not
 *              the table's, and a pipeline that does not resample with
 *              lanczos4 (its table is empty).
 *
 *  The device run is test_cuda_speed_lanczos4_parity.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/speed_gpu_common.h"
#include "feature/speed_internal.h"
#include "feature/vif_tools.h"

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: retain NULL for Windows C
 * support and upstream-compatible C test conventions. */

#define LZ_TAPS ((int)VIF_LANCZOS4_TAPS)
#define LZ_RADIUS ((LZ_TAPS - 1) / 2)
#define LZ_GUARD (-7.0f) /* never a lanczos4 weight: every weight is in (-1, 1] */

typedef struct LzCase {
    int src_w;
    int src_h;
    int dst_w;
    int dst_h;
} LzCase;

/* A smooth ramp with a little deterministic texture, the content class the
 * drift showed on, in picture_copy()'s value range. */
static void lz_fill(float *plane, int w, int h)
{
    uint32_t state = 0x9e3779b9u;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            state = state * 1664525u + 1013904223u;
            const float ramp = 90.0f * (float)x / (float)w + 60.0f * (float)y / (float)h;
            const float texture = (float)(state >> 29u); /* 0..7 */
            plane[(ptrdiff_t)y * w + x] = ramp + texture - 128.0f;
        }
    }
}

/* mirror(), vif_tools.c. */
static int lz_mirror(int i, int last)
{
    const float v = (float)i;
    const float right = (float)last;
    if (v < 0.0f)
        return (int)-v;
    if (v > right)
        return (int)(2.0f * right - v);
    return i;
}

/* One output sample as the scale kernel computes it: the nine column taps
 * `wx` and nine row taps `wy` come from the table, everything else is the
 * kernel's fp32 arithmetic in the reference's order. */
static float lz_sample(const float *src, const LzCase *c, const float *wx, const float *wy, int x0,
                       int y0)
{
    float value = 0.0f;
    float weight_sum = 0.0f;
    for (int iy = -LZ_RADIUS; iy <= LZ_RADIUS; iy++) {
        for (int ix = -LZ_RADIUS; ix <= LZ_RADIUS; ix++) {
            const float weight = wx[ix + LZ_RADIUS] * wy[iy + LZ_RADIUS];
            weight_sum += weight;
            const int xi = lz_mirror(x0 + ix, c->src_w - 1);
            const int yi = lz_mirror(y0 + iy, c->src_h - 1);
            value += src[(ptrdiff_t)yi * c->src_w + xi] * weight;
        }
    }
    return value / weight_sum;
}

static uint32_t lz_bits(float v)
{
    uint32_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* floor() of the source position of output sample `i`, the expression of
 * vif_scale_frame_lanczos4_s(): fp64, rounded once to fp32. */
static int lz_base(int i, int src_len, int dst_len)
{
    const float ratio = (float)src_len / (float)dst_len;
    const float position = (float)(((double)i + 0.5) * (double)ratio - 0.5);
    return (int)floorf(position);
}

/* Resample with the table and count the samples that differ from `want`. */
static size_t lz_mismatches(const float *src, const float *want, const float *table,
                            const LzCase *c)
{
    const float *rows = table + (ptrdiff_t)LZ_TAPS * c->dst_w;
    size_t differing = 0u;
    for (int y = 0; y < c->dst_h; y++) {
        const int y0 = lz_base(y, c->src_h, c->dst_h);
        for (int x = 0; x < c->dst_w; x++) {
            const int x0 = lz_base(x, c->src_w, c->dst_w);
            const float got = lz_sample(src, c, table + (ptrdiff_t)LZ_TAPS * x,
                                        rows + (ptrdiff_t)LZ_TAPS * y, x0, y0);
            const float ref = want[(ptrdiff_t)y * c->dst_w + x];
            differing += lz_bits(got) != lz_bits(ref) ? 1u : 0u;
        }
    }
    return differing;
}

static SpeedGpuGeometry lz_geometry(const LzCase *c)
{
    SpeedGpuGeometry g;
    memset(&g, 0, sizeof(g));
    g.src_w = (uint32_t)c->src_w;
    g.src_h = (uint32_t)c->src_h;
    g.scaled_w = (uint32_t)c->dst_w;
    g.scaled_h = (uint32_t)c->dst_h;
    g.prescale = 1;
    g.scale_method = (int32_t)vif_scale_lanczos4;
    return g;
}

/* The scaled plane of one case through vif_scale_frame_s() and through the
 * device table; NULL when both agree on every sample. */
static char *lz_compare(const LzCase *c, const SpeedGpuGeometry *g, float *src, float *want,
                        float *table, size_t count)
{
    lz_fill(src, c->src_w, c->src_h);
    vif_scale_frame_s(vif_scale_lanczos4, src, want, c->src_w, c->src_h, c->src_w, c->dst_w,
                      c->dst_h, c->dst_w);
    mu_assert("speed_internal_gpu_lanczos_weights failed",
              speed_internal_gpu_lanczos_weights(g, table, count) == 0);
    mu_assert("the table does not reproduce vif_scale_frame_s() bit for bit",
              lz_mismatches(src, want, table, c) == 0u);
    return NULL;
}

static char *lz_check_case(const LzCase *c)
{
    const SpeedGpuGeometry g = lz_geometry(c);
    const size_t count = speed_internal_gpu_lanczos_count(&g);
    mu_assert("table size is 9 per scaled column plus 9 per scaled row",
              count == (size_t)LZ_TAPS * ((size_t)c->dst_w + (size_t)c->dst_h));
    float *src = malloc(sizeof(*src) * (size_t)c->src_w * (size_t)c->src_h);
    float *want = malloc(sizeof(*want) * (size_t)c->dst_w * (size_t)c->dst_h);
    float *table = malloc(sizeof(*table) * count);
    char *msg = "out of memory";
    if (src && want && table)
        msg = lz_compare(c, &g, src, want, table, count);
    free(table);
    free(want);
    free(src);
    return msg;
}

static char *test_table_reproduces_cpu_scaler(void)
{
    static const LzCase cases[] = {
        {192, 108, 96, 54},  /* prescale 0.5 */
        {96, 54, 192, 108},  /* prescale 2.0 */
        {161, 91, 121, 68},  /* 0.75, odd sizes */
        {120, 68, 181, 101}, /* 1.5, odd sizes */
        {64, 48, 64, 31},    /* one axis unscaled */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        mu_assert_msg(lz_check_case(&cases[i]));
    return NULL;
}

static char *test_unscaled_axis_has_unit_centre_tap(void)
{
    enum { LEN = 17 };
    float weights[LZ_TAPS * LEN];
    vif_scale_lanczos4_axis_weights(LEN, LEN, weights);
    for (int i = 0; i < LEN; i++) {
        const float *w = weights + (ptrdiff_t)LZ_TAPS * i;
        mu_assert("fraction 0: the centre tap is exactly 1", w[LZ_RADIUS] == 1.0f);
        for (int k = 0; k < LZ_TAPS; k++) {
            mu_assert("fraction 0: every other tap is a rounding residue of sin(k pi)",
                      k == LZ_RADIUS || fabsf(w[k]) < 1e-15f);
        }
    }
    return NULL;
}

static char *test_single_and_empty_axis(void)
{
    float weights[LZ_TAPS + 1];
    for (int k = 0; k <= LZ_TAPS; k++)
        weights[k] = LZ_GUARD;
    vif_scale_lanczos4_axis_weights(8, 0, weights);
    mu_assert("an empty axis writes nothing", weights[0] == LZ_GUARD);
    vif_scale_lanczos4_axis_weights(8, 1, weights);
    float sum = 0.0f;
    for (int k = 0; k < LZ_TAPS; k++) {
        mu_assert("a single output sample gets nine finite weights",
                  isfinite(weights[k]) && weights[k] != LZ_GUARD);
        sum += weights[k];
    }
    mu_assert("one sample writes nine weights and no more", weights[LZ_TAPS] == LZ_GUARD);
    mu_assert("the nine taps of a sample sum to about 1", fabsf(sum - 1.0f) < 0.05f);
    return NULL;
}

static char *test_table_helpers_reject_bad_arguments(void)
{
    const LzCase c = {64, 48, 32, 24};
    const SpeedGpuGeometry g = lz_geometry(&c);
    float table[LZ_TAPS * (32 + 24)];
    const size_t count = sizeof(table) / sizeof(table[0]);
    mu_assert("the sized table is accepted",
              speed_internal_gpu_lanczos_weights(&g, table, count) == 0);
    mu_assert("NULL geometry has no table", speed_internal_gpu_lanczos_count(NULL) == 0u);
    mu_assert("NULL geometry is refused",
              speed_internal_gpu_lanczos_weights(NULL, table, count) == -EINVAL);
    mu_assert("NULL table is refused",
              speed_internal_gpu_lanczos_weights(&g, NULL, count) == -EINVAL);
    mu_assert("a short table is refused",
              speed_internal_gpu_lanczos_weights(&g, table, count - 1u) == -EINVAL);
    mu_assert("a long table is refused",
              speed_internal_gpu_lanczos_weights(&g, table, count + 1u) == -EINVAL);
    return NULL;
}

static char *test_no_table_without_lanczos4_resample(void)
{
    const LzCase c = {64, 48, 32, 24};
    SpeedGpuGeometry g = lz_geometry(&c);
    float table[LZ_TAPS * (32 + 24)];
    const size_t count = sizeof(table) / sizeof(table[0]);
    g.scale_method = (int32_t)vif_scale_bicubic;
    mu_assert("bicubic prescale has no table", speed_internal_gpu_lanczos_count(&g) == 0u);
    mu_assert("bicubic prescale is refused",
              speed_internal_gpu_lanczos_weights(&g, table, count) == -EINVAL);
    g.scale_method = (int32_t)vif_scale_lanczos4;
    g.prescale = 0;
    mu_assert("no prescale has no table", speed_internal_gpu_lanczos_count(&g) == 0u);
    mu_assert("an empty table is refused",
              speed_internal_gpu_lanczos_weights(&g, table, 0u) == -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_table_reproduces_cpu_scaler);
    mu_run_test(test_unscaled_axis_has_unit_centre_tap);
    mu_run_test(test_single_and_empty_axis);
    mu_run_test(test_table_helpers_reject_bad_arguments);
    mu_run_test(test_no_table_without_lanczos4_resample);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
