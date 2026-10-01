/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1412 — device-free replay of the float_vif CUDA kernels against the CPU
 * extractor's own routines.
 *
 * cuda/float_vif/float_vif_device.h holds the arithmetic every
 * float_vif_score.cu thread runs. This test compiles the same header for the
 * host and checks it against vif_tools.c and vif.c, bit for bit:
 *
 *   - fvif_pixel_statistic() against vif_statistic_s() on a 1x1 plane, which
 *     returns exactly one pixel's numerator and denominator term, over inputs
 *     that reach every branch of vif_pixel_statistic_s() and several values
 *     of vif_sigma_nsq and vif_enhn_gain_limit;
 *   - fvif_row_sum() + fvif_sum_rows() against vif_statistic_s() on planes
 *     whose width and height are not multiples of any block size, which pins
 *     the order of the two fp32 accumulations;
 *   - the whole per-frame pipeline, composed in the kernels' order (decimate,
 *     vertical pass, horizontal pass, statistic, row sums) from the header's
 *     helpers and vif_get_filter()'s taps, against compute_vif(), for 8- and
 *     10-bit frames, default and non-default options.
 *
 * A fixed table of taps, a libm log2f(), an fp32 vif_sigma_nsq or a per-block
 * reduction each fail at least one of these, which is how the twin differed
 * from the CPU before ADR-1412. A host replay cannot see the device's
 * scheduling or its compiler; test_cuda_float_vif_parity covers that on an
 * NVIDIA device.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/cuda/cuda_tile_index.h"
#include "feature/cuda/float_vif/float_vif_device.h"
#include "feature/vif.h"
#include "feature/vif_tools.h"
#include "mem.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define STAT_SAMPLES 200000u
#define MAX_W 96
#define MAX_H 64

/* Deterministic generator: the same inputs on every host. */
static uint32_t rng_state = 0x2545f491u;

static uint32_t rng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* Uniform in [0, 1). */
static float rng_unit(void)
{
    return (float)(rng_next() >> 8) * (1.0f / 16777216.0f);
}

static int same_bits(float a, float b)
{
    return (a != a && b != b) || fvif_float_bits(a) == fvif_float_bits(b);
}

typedef struct Moments {
    float mu1;
    float mu2;
    float xx;
    float yy;
    float xy;
} Moments;

/* One pixel's five moments. `kind` selects the variance regime so that every
 * branch of vif_pixel_statistic_s() is reached: flat reference, flat
 * distortion, variances below and above sigma_nsq, negative covariance and
 * moments that make a variance negative before the clamp. */
static Moments random_moments(unsigned kind)
{
    static const float scale[] = {0.0f, 1.0e-11f, 1.0e-3f, 1.5f, 40.0f, 3000.0f, 16000.0f};
    const unsigned levels = (unsigned)(sizeof(scale) / sizeof(scale[0]));
    const float s1 = scale[kind % levels] * rng_unit();
    const float s2 = scale[(kind / levels) % levels] * rng_unit();
    const float rho = 2.0f * rng_unit() - 1.0f;
    Moments m;
    m.mu1 = 255.0f * rng_unit() - 128.0f;
    m.mu2 = 255.0f * rng_unit() - 128.0f;
    m.xx = s1 + m.mu1 * m.mu1;
    m.yy = s2 + m.mu2 * m.mu2;
    m.xy = rho * sqrtf(s1 * s2) + m.mu1 * m.mu2;
    if (kind % 11u == 0u)
        m.xx = m.mu1 * m.mu1 - 1.0e-3f * rng_unit();
    return m;
}

static const double SIGMA_NSQ[] = {2.0, 1.5, 0.3, 5.0};
static const double GAIN_LIMIT[] = {100.0, 1.0, 1.7};

static float sigma_max_inv(double sigma_nsq)
{
    return (float)(powf((float)sigma_nsq, 2.0f) / (255.0 * 255.0));
}

static char *test_pixel_statistic_is_vif_pixel_statistic_s(void)
{
    for (unsigned i = 0u; i < STAT_SAMPLES; i++) {
        const Moments m = random_moments(i);
        const double nsq = SIGMA_NSQ[i % 4u];
        const double egl = GAIN_LIMIT[(i / 4u) % 3u];
        float cpu_num = 0.0f;
        float cpu_den = 0.0f;
        vif_statistic_s(&m.mu1, &m.mu2, &m.xx, &m.yy, &m.xy, &cpu_num, &cpu_den, 1, 1,
                        (int)sizeof(float), (int)sizeof(float), (int)sizeof(float),
                        (int)sizeof(float), (int)sizeof(float), egl, nsq);
        float num = 0.0f;
        float den = 0.0f;
        fvif_pixel_statistic(m.mu1, m.mu2, m.xx, m.yy, m.xy, sigma_max_inv(nsq), (float)egl, nsq,
                             &num, &den);
        if (!same_bits(num, cpu_num) || !same_bits(den, cpu_den)) {
            (void)fprintf(stderr,
                          "\nsample %u (nsq=%g egl=%g): num %.9g vs cpu %.9g, den %.9g vs cpu "
                          "%.9g\n",
                          i, nsq, egl, (double)num, (double)cpu_num, (double)den, (double)cpu_den);
        }
        mu_assert("fvif_pixel_statistic() numerator differs from vif_pixel_statistic_s()",
                  same_bits(num, cpu_num));
        mu_assert("fvif_pixel_statistic() denominator differs from vif_pixel_statistic_s()",
                  same_bits(den, cpu_den));
    }
    return NULL;
}

/* fvif_log2() at the edges log2f_approx() special-cases, through the statistic:
 * a zero denominator argument cannot be reached there, so call it directly
 * for the values the reference names. */
static char *test_log2_special_values(void)
{
    mu_assert("fvif_log2(1) must be exactly 0", fvif_log2(1.0f) == 0.0f);
    mu_assert("fvif_log2(2) must be exactly 1", fvif_log2(2.0f) == 1.0f);
    mu_assert("fvif_log2(0) must be -inf", fvif_log2(0.0f) == -INFINITY);
    const float negative = fvif_log2(-1.0f);
    mu_assert("fvif_log2(negative) must be NaN", negative != negative);
    return NULL;
}

typedef struct Plane {
    float *data;
    int w;
    int h;
    int stride; /* bytes */
} Plane;

static int plane_alloc(Plane *p, int w, int h)
{
    p->w = w;
    p->h = h;
    p->stride = (int)ALIGN_CEIL((size_t)w * sizeof(float));
    p->data = aligned_malloc((size_t)p->stride * (size_t)h, MAX_ALIGN);
    if (!p->data)
        return -1;
    memset(p->data, 0, (size_t)p->stride * (size_t)h);
    return 0;
}

static float *plane_at(const Plane *p, int x, int y)
{
    return p->data + (size_t)y * ((size_t)p->stride / sizeof(float)) + (size_t)x;
}

/* The device's reduction of one scale: the terms column by column, one row sum
 * per thread, the rows on the host. */
static void device_reduce(const Plane mom[5], float smi, float egl, double nsq, float *terms,
                          float *rows, double *num, double *den)
{
    const uint32_t w = (uint32_t)mom[0].w;
    const uint32_t h = (uint32_t)mom[0].h;
    for (uint32_t y = 0u; y < h; y++) {
        for (uint32_t x = 0u; x < w; x++) {
            const size_t at = fvif_term_index(x, y, h);
            fvif_pixel_statistic(
                *plane_at(&mom[0], (int)x, (int)y), *plane_at(&mom[1], (int)x, (int)y),
                *plane_at(&mom[2], (int)x, (int)y), *plane_at(&mom[3], (int)x, (int)y),
                *plane_at(&mom[4], (int)x, (int)y), smi, egl, nsq, &terms[at], &terms[at + 1u]);
        }
    }
    for (uint32_t y = 0u; y < h; y++) {
        fvif_row_sum(terms, w, h, y, &rows[(size_t)y * FVIF_TERM_FLOATS],
                     &rows[(size_t)y * FVIF_TERM_FLOATS + 1u]);
    }
    fvif_sum_rows(rows, h, num, den);
}

static char *check_reduction(int w, int h, double nsq, double egl)
{
    Plane mom[5] = {{0}};
    for (int i = 0; i < 5; i++)
        mu_assert("plane allocation failed", !plane_alloc(&mom[i], w, h));
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const Moments m = random_moments((unsigned)(y * w + x));
            *plane_at(&mom[0], x, y) = m.mu1;
            *plane_at(&mom[1], x, y) = m.mu2;
            *plane_at(&mom[2], x, y) = m.xx;
            *plane_at(&mom[3], x, y) = m.yy;
            *plane_at(&mom[4], x, y) = m.xy;
        }
    }
    float cpu_num = 0.0f;
    float cpu_den = 0.0f;
    vif_statistic_s(mom[0].data, mom[1].data, mom[2].data, mom[3].data, mom[4].data, &cpu_num,
                    &cpu_den, w, h, mom[0].stride, mom[1].stride, mom[2].stride, mom[3].stride,
                    mom[4].stride, egl, nsq);

    static float terms[MAX_W * MAX_H * FVIF_TERM_FLOATS];
    static float rows[MAX_H * FVIF_TERM_FLOATS];
    double num = 0.0;
    double den = 0.0;
    device_reduce(mom, sigma_max_inv(nsq), (float)egl, nsq, terms, rows, &num, &den);
    for (int i = 0; i < 5; i++)
        aligned_free(mom[i].data);
    mu_assert("row-ordered numerator sum differs from vif_statistic_s()", num == (double)cpu_num);
    mu_assert("row-ordered denominator sum differs from vif_statistic_s()", den == (double)cpu_den);
    return NULL;
}

static char *test_row_sums_are_vif_statistic_s(void)
{
    mu_assert_msg(check_reduction(MAX_W, MAX_H, 2.0, 100.0));
    mu_assert_msg(check_reduction(67, 41, 2.0, 100.0));
    mu_assert_msg(check_reduction(33, 17, 1.5, 1.0));
    mu_assert_msg(check_reduction(1, 1, 2.0, 100.0));
    return NULL;
}

/* A sample of a synthetic frame after picture_copy() with offset -128. */
static float frame_sample(int x, int y, unsigned bpc, int distorted)
{
    const unsigned peak = (1u << bpc) - 1u;
    unsigned v = ((unsigned)(x * 7 + y * 13) ^ (unsigned)((x * y) >> 2)) & peak;
    if (distorted)
        v = (v + ((unsigned)(x * 3 + y * 5) % 23u) * (peak / 255u)) & peak;
    if (bpc <= 8u)
        return FVIF_FSUB((float)v, 128.0f);
    return FVIF_FSUB(FVIF_FDIV((float)v, (float)(1u << (bpc - 8u))), 128.0f);
}

/* float_vif_compute's two filter passes for one moment: `mode` 0 filters a,
 * 1 filters a * a, 2 filters a * b. */
static float device_moment(const FloatVifCudaTaps *taps, const Plane *a, const Plane *b, int mode,
                           int x, int y)
{
    const int hfw = taps->width / 2;
    float acc = 0.0f;
    for (int kj = 0; kj < taps->width; kj++) {
        const int px = vmaf_cuda_reflect_101(x - hfw + kj, a->w);
        float v = 0.0f;
        for (int ki = 0; ki < taps->width; ki++) {
            const int py = vmaf_cuda_reflect_101(y - hfw + ki, a->h);
            const float s = *plane_at(a, px, py);
            float sample = s;
            if (mode == 1) {
                sample = FVIF_FMUL(s, s);
            } else if (mode == 2) {
                sample = FVIF_FMUL(s, *plane_at(b, px, py));
            }
            v = fvif_tap(v, taps->coeff[ki], sample);
        }
        acc = fvif_tap(acc, taps->coeff[kj], v);
    }
    return acc;
}

static int scale_taps(FloatVifCudaTaps *taps, int scale)
{
    float filter[128] = {0};
    const int width = vif_get_filter_size(scale, 1.0f);
    if (width < 1 || width > FVIF_MAX_FW)
        return -1;
    vif_get_filter(filter, scale, 1.0f);
    memset(taps, 0, sizeof(*taps));
    memcpy(taps->coeff, filter, (size_t)width * sizeof(filter[0]));
    taps->width = width;
    return 0;
}

/* float_vif_decimate: `taps` over `in`, sampled at even positions. */
static int device_decimate(const FloatVifCudaTaps *taps, const Plane *in, Plane *out)
{
    if (plane_alloc(out, in->w / 2, in->h / 2))
        return -1;
    for (int y = 0; y < out->h; y++) {
        for (int x = 0; x < out->w; x++)
            *plane_at(out, x, y) = device_moment(taps, in, in, 0, 2 * x, 2 * y);
    }
    return 0;
}

/* One scale of the device pipeline on the host. */
static int device_scale(const FloatVifCudaTaps *taps, const Plane *ref, const Plane *dis,
                        double nsq, double egl, double *num, double *den)
{
    static float terms[MAX_W * MAX_H * FVIF_TERM_FLOATS];
    static float rows[MAX_H * FVIF_TERM_FLOATS];
    Plane mom[5] = {{0}};
    for (int i = 0; i < 5; i++) {
        if (plane_alloc(&mom[i], ref->w, ref->h))
            return -1;
    }
    for (int y = 0; y < ref->h; y++) {
        for (int x = 0; x < ref->w; x++) {
            *plane_at(&mom[0], x, y) = device_moment(taps, ref, ref, 0, x, y);
            *plane_at(&mom[1], x, y) = device_moment(taps, dis, dis, 0, x, y);
            *plane_at(&mom[2], x, y) = device_moment(taps, ref, ref, 1, x, y);
            *plane_at(&mom[3], x, y) = device_moment(taps, dis, dis, 1, x, y);
            *plane_at(&mom[4], x, y) = device_moment(taps, ref, dis, 2, x, y);
        }
    }
    device_reduce(mom, sigma_max_inv(nsq), (float)egl, nsq, terms, rows, num, den);
    for (int i = 0; i < 5; i++)
        aligned_free(mom[i].data);
    return 0;
}

/* The synthetic frame pair, as picture_copy() would hand it to compute_vif(). */
static int make_frames(Plane *ref, Plane *dis, int w, int h, unsigned bpc)
{
    if (plane_alloc(ref, w, h) || plane_alloc(dis, w, h))
        return -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            *plane_at(ref, x, y) = frame_sample(x, y, bpc, 0);
            *plane_at(dis, x, y) = frame_sample(x, y, bpc, 1);
        }
    }
    return 0;
}

/* Replace the pair by the next scale's: `taps` over it, every second sample. */
static int next_scale(const FloatVifCudaTaps *taps, Plane *ref, Plane *dis)
{
    Plane ref_next = {0};
    Plane dis_next = {0};
    if (device_decimate(taps, ref, &ref_next) || device_decimate(taps, dis, &dis_next))
        return -1;
    aligned_free(ref->data);
    aligned_free(dis->data);
    *ref = ref_next;
    *dis = dis_next;
    return 0;
}

/* One scale of the device pipeline against compute_vif()'s sums for it. */
static char *check_scale(const FloatVifCudaTaps *taps, const Plane *ref, const Plane *dis,
                         double nsq, double egl, const double cpu[2])
{
    double num = 0.0;
    double den = 0.0;
    mu_assert("scale allocation failed", !device_scale(taps, ref, dis, nsq, egl, &num, &den));
    if (num != cpu[0] || den != cpu[1]) {
        (void)fprintf(stderr, "\n%dx%d: num %.17g vs cpu %.17g, den %.17g vs cpu %.17g\n", ref->w,
                      ref->h, num, cpu[0], den, cpu[1]);
    }
    mu_assert("device pipeline numerator differs from compute_vif()", num == cpu[0]);
    mu_assert("device pipeline denominator differs from compute_vif()", den == cpu[1]);
    return NULL;
}

static char *check_pipeline(int w, int h, unsigned bpc, double nsq, double egl)
{
    Plane ref = {0};
    Plane dis = {0};
    mu_assert("frame allocation failed", !make_frames(&ref, &dis, w, h, bpc));
    double score = 0.0;
    double score_num = 0.0;
    double score_den = 0.0;
    double cpu[2 * FVIF_SCALES] = {0};
    mu_assert("compute_vif() failed",
              !compute_vif(ref.data, dis.data, w, h, ref.stride, dis.stride, &score, &score_num,
                           &score_den, cpu, egl, 1.0, 0, nsq, NULL, NULL));

    for (size_t scale = 0u; scale < FVIF_SCALES; scale++) {
        FloatVifCudaTaps taps;
        mu_assert("vif_get_filter() width out of the kernels' range",
                  !scale_taps(&taps, (int)scale));
        if (scale > 0u) {
            mu_assert("decimate allocation failed", !next_scale(&taps, &ref, &dis));
        }
        mu_assert_msg(check_scale(&taps, &ref, &dis, nsq, egl, &cpu[2u * scale]));
    }
    aligned_free(ref.data);
    aligned_free(dis.data);
    return NULL;
}

static char *test_pipeline_is_compute_vif(void)
{
    mu_assert_msg(check_pipeline(MAX_W, MAX_H, 8u, 2.0, 100.0));
    mu_assert_msg(check_pipeline(MAX_W, MAX_H, 10u, 2.0, 100.0));
    /* Odd sizes: 50x38 halves to 25x19, 12x9 and 6x4, so every scale has
     * columns the AVX2 convolution leaves to its scalar edge path. */
    mu_assert_msg(check_pipeline(50, 38, 8u, 2.0, 100.0));
    mu_assert_msg(check_pipeline(50, 38, 12u, 1.5, 1.0));
    /* The smallest frame the extractors accept. */
    mu_assert_msg(check_pipeline(16, 16, 8u, 2.0, 100.0));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_pixel_statistic_is_vif_pixel_statistic_s);
    mu_run_test(test_log2_special_values);
    mu_run_test(test_row_sums_are_vif_statistic_s);
    mu_run_test(test_pipeline_is_compute_vif);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
