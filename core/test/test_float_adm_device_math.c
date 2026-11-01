/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1420 — device-free replay of the float_adm CUDA and HIP kernels against
 * the CPU extractor's own routines.
 *
 * feature/float_adm_gpu_common.h, included here through the CUDA twin's
 * cuda/float_adm/float_adm_device.h, holds the arithmetic every thread of the
 * decouple, term and row-sum kernels of float_adm_cuda and float_adm_hip
 * (ADR-1458) runs. This test compiles the same header
 * for the host and checks it against adm_tools.c, bit for bit:
 *
 *   - the reference's decouple against the IEEE quotient, on inputs where a
 *     reciprocal refined from the processor's RCPSS estimate gives another
 *     float (ADR-1442: the reference divides, on every host);
 *   - fadm_decouple_csf() against adm_decouple_s() followed by adm_csf_s(),
 *     sample by sample, over bands that reach every branch of the decouple
 *     (aligned and opposed vectors, enhancement above and below the gain
 *     limit, zeros, values of either sign) and several gain limits;
 *   - the three reductions of one scale, composed in the kernels' order
 *     (decouple, terms, row sums, fold, pool), against adm_csf_den_scale_s()
 *     and the two adm_cm_s() calls of compute_adm(), on bands whose reduced
 *     region does and does not reach the band's edges;
 *   - adm_pool_bands_s() against the cube-root form the reference spelled out
 *     before it had one pooling routine.
 *
 * A reciprocal in place of the quotient, an fp32 gain or fp32 1/30 and 1/15
 * constants, a threshold summed in another order, cos^2 * (|o|^2 * |t|^2) and
 * a sum per tile each fail at least one of these. A host replay cannot see the device's scheduling, its
 * compiler or its DWT; test_cuda_float_adm_parity covers those on an NVIDIA
 * device.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/adm_float_reference.h"
#include "feature/adm_options.h"
#include "feature/adm_tools.h"
#include "feature/cuda/float_adm/float_adm_device.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define MAX_W 136
#define MAX_H 80
#define BAND_FLOATS ((size_t)MAX_W * MAX_H)
#define DECOUPLE_TRIALS 64

/* Deterministic generator: the same inputs on every host. */
static uint32_t rng_state = 0x1420adu;

static uint32_t rng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* Uniform in [-1, 1). */
static float rng_unit(void)
{
    return (float)((double)(rng_next() >> 8) / 8388608.0 - 1.0);
}

static uint32_t bits_of(float x)
{
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

static float float_of(uint32_t bits)
{
    float x;
    memcpy(&x, &bits, sizeof(x));
    return x;
}

/* One scale's buffers in the kernels' layout, and the reference's views of
 * the same memory. */
typedef struct Scale {
    float *ref;    /* 4 sub-bands: a, h, v, d */
    float *dis;    /* 4 sub-bands */
    float *csf_a;  /* 3 sub-bands: h, v, d */
    float *csf_fa; /* 3 sub-bands */
    float *csf_r;  /* 3 sub-bands */
    float *csf_fr; /* 3 sub-bands */
    float *cpu[6]; /* decouple_r, decouple_a, csf_a, csf_fa, csf_r, csf_fr of the reference */
    float *terms;
    float *rows;
} Scale;

static int scale_alloc(Scale *s)
{
    memset(s, 0, sizeof(*s));
    s->ref = calloc(4u * BAND_FLOATS, sizeof(float));
    s->dis = calloc(4u * BAND_FLOATS, sizeof(float));
    s->csf_a = calloc(3u * BAND_FLOATS, sizeof(float));
    s->csf_fa = calloc(3u * BAND_FLOATS, sizeof(float));
    s->csf_r = calloc(3u * BAND_FLOATS, sizeof(float));
    s->csf_fr = calloc(3u * BAND_FLOATS, sizeof(float));
    s->terms = calloc((size_t)FADM_TERM_SLOTS * BAND_FLOATS, sizeof(float));
    s->rows = calloc((size_t)FADM_TERM_SLOTS * MAX_H, sizeof(float));
    bool ok =
        s->ref && s->dis && s->csf_a && s->csf_fa && s->csf_r && s->csf_fr && s->terms && s->rows;
    for (int i = 0; i < 6; i++) {
        s->cpu[i] = calloc(3u * BAND_FLOATS, sizeof(float));
        ok = ok && s->cpu[i];
    }
    return ok ? 0 : -1;
}

static void scale_free(Scale *s)
{
    free(s->ref);
    free(s->dis);
    free(s->csf_a);
    free(s->csf_fa);
    free(s->csf_r);
    free(s->csf_fr);
    free(s->terms);
    free(s->rows);
    for (int i = 0; i < 6; i++)
        free(s->cpu[i]);
}

/* The reference's (h, v, d) view of a three-sub-band buffer. */
static adm_dwt_band_t_s view3(float *buf, int w, int h)
{
    const size_t plane = (size_t)w * (size_t)h;
    adm_dwt_band_t_s b = {
        .band_a = NULL, .band_v = buf + plane, .band_h = buf, .band_d = buf + 2u * plane};
    return b;
}

/* The reference's view of the (h, v, d) sub-bands of a DWT band buffer. */
static adm_dwt_band_t_s view4(float *buf, int w, int h)
{
    return view3(buf + (size_t)w * (size_t)h, w, h);
}

/* Band content that reaches every branch of the decouple: mostly a distorted
 * band correlated with the reference, with runs of aligned, enhanced, opposed
 * and zero samples. */
static void fill_bands(Scale *s, int w, int h, float amplitude)
{
    const size_t plane = (size_t)w * (size_t)h;
    for (size_t i = 0; i < 4u * plane; i++) {
        const float o = rng_unit() * amplitude;
        float t = o * (0.5f + 0.75f * (rng_unit() + 1.0f)) + rng_unit() * amplitude * 0.05f;
        const uint32_t kind = rng_next() % 16u;
        if (kind == 0u) {
            t = o;
        } else if (kind == 1u) {
            t = o * 1.5f;
        } else if (kind == 2u) {
            t = o * 1.1f;
        } else if (kind == 3u) {
            t = -o;
        } else if (kind == 4u) {
            t = 0.0f;
        }
        s->ref[i] = (kind == 5u) ? 0.0f : o;
        s->dis[i] = t;
    }
    /* Whole samples aligned across h and v, so the angle test passes. */
    for (size_t i = 0; i < plane; i += 3u) {
        const float gain = 1.0f + 0.4f * (rng_unit() + 1.0f);
        s->dis[plane + i] = s->ref[plane + i] * gain;
        s->dis[2u * plane + i] = s->ref[2u * plane + i] * gain;
        s->dis[3u * plane + i] = s->ref[3u * plane + i] * gain;
    }
    /* Whole samples one degree apart: the two sides of the angle test then
     * agree to within their rounding, so the test's outcome depends on the
     * association of cos^2 * |o|^2 * |t|^2. */
    const double cos_1deg = 0.99984769515639127;
    const double sin_1deg = 0.017452406437283512;
    for (size_t i = 1; i < plane; i += 3u) {
        const double oh = s->ref[plane + i];
        const double ov = s->ref[2u * plane + i];
        const double gain = 1.0 + 0.4 * ((double)rng_unit() + 1.0);
        s->dis[plane + i] = (float)(gain * (cos_1deg * oh - sin_1deg * ov));
        s->dis[2u * plane + i] = (float)(gain * (sin_1deg * oh + cos_1deg * ov));
    }
}

typedef struct Options {
    double gain_limit;
    double noise_weight;
    double p_norm;
    int bypass_cm;
    int scale;
} Options;

static FloatAdmCudaDecoupleArgs decouple_args(int w, int h, const Options *o)
{
    FloatAdmCudaDecoupleArgs a;
    memset(&a, 0, sizeof(a));
    a.bands.half_w = w;
    a.bands.half_h = h;
    a.bands.buf_stride = w;
    adm_csf_rfactor_s(o->scale, DEFAULT_ADM_NORM_VIEW_DIST, DEFAULT_ADM_REF_DISPLAY_HEIGHT,
                      DEFAULT_ADM_CSF_MODE, DEFAULT_ADM_CSF_LUMINANCE_LEVEL, DEFAULT_ADM_CSF_SCALE,
                      DEFAULT_ADM_CSF_DIAG_SCALE, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
                      a.bands.rfactor);
    a.adm_enhn_gain_limit = o->gain_limit;
    a.cos_1deg_sq = adm_decouple_cos_1deg_sq_s();
    return a;
}

/* The decouple kernel over the whole band. */
static void replay_decouple(Scale *s, const FloatAdmCudaDecoupleArgs *a)
{
    const int w = a->bands.half_w;
    const int h = a->bands.half_h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float o[FADM_BANDS];
            float t[FADM_BANDS];
            for (int b = 0; b < FADM_BANDS; b++) {
                const size_t idx = fadm_band_index(b + 1, y, x, w, h);
                o[b] = s->ref[idx];
                t[b] = s->dis[idx];
            }
            const int flag = fadm_angle_flag(o[0], o[1], t[0], t[1], a->cos_1deg_sq);
            for (int b = 0; b < FADM_BANDS; b++) {
                const FloatAdmCsfSample c = fadm_decouple_csf(a, b, o[b], t[b], flag);
                const size_t idx = fadm_band_index(b, y, x, w, h);
                s->csf_a[idx] = c.csf_a;
                s->csf_fa[idx] = c.csf_fa;
                s->csf_r[idx] = c.csf_r;
                s->csf_fr[idx] = c.csf_fr;
            }
        }
    }
}

/* adm_csf_s() with the options the twin passes. */
static void reference_csf(const adm_dwt_band_t_s *src, const adm_dwt_band_t_s *dst,
                          const adm_dwt_band_t_s *flt, int w, int h, const Options *o)
{
    const int stride = w * (int)sizeof(float);
    adm_csf_s(src, dst, flt, h, o->scale, w, h, stride, stride, ADM_BORDER_FACTOR,
              DEFAULT_ADM_NORM_VIEW_DIST, DEFAULT_ADM_REF_DISPLAY_HEIGHT, DEFAULT_ADM_CSF_MODE,
              DEFAULT_ADM_CSF_LUMINANCE_LEVEL, DEFAULT_ADM_CSF_SCALE, DEFAULT_ADM_CSF_DIAG_SCALE,
              -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0);
}

/* adm_cm_s() with the options the twin passes. */
static float reference_cm(const adm_dwt_band_t_s *src, const adm_dwt_band_t_s *csf_f,
                          const adm_dwt_band_t_s *csf_a, int w, int h, const Options *o,
                          double noise_weight)
{
    const int stride = w * (int)sizeof(float);
    return adm_cm_s(src, csf_f, csf_a, w, h, stride, stride, stride, ADM_BORDER_FACTOR, o->scale,
                    DEFAULT_ADM_NORM_VIEW_DIST, DEFAULT_ADM_REF_DISPLAY_HEIGHT,
                    DEFAULT_ADM_CSF_MODE, DEFAULT_ADM_CSF_LUMINANCE_LEVEL, DEFAULT_ADM_CSF_SCALE,
                    DEFAULT_ADM_CSF_DIAG_SCALE, noise_weight, o->bypass_cm, o->p_norm, -1.0, -1.0,
                    -1.0, -1.0, -1.0, -1.0, -1.0, -1.0);
}

/* compute_adm()'s scale body past the DWT: den_scale, num_scale and
 * aim_num_scale in out[0..2]. Leaves decouple and CSF results in s->cpu[]. */
static void reference_scale(Scale *s, int w, int h, const Options *o, float out[3])
{
    const int stride = w * (int)sizeof(float);
    const adm_dwt_band_t_s ref = view4(s->ref, w, h);
    const adm_dwt_band_t_s dis = view4(s->dis, w, h);
    const adm_dwt_band_t_s r = view3(s->cpu[0], w, h);
    const adm_dwt_band_t_s a = view3(s->cpu[1], w, h);
    const adm_dwt_band_t_s csf_a = view3(s->cpu[2], w, h);
    const adm_dwt_band_t_s csf_fa = view3(s->cpu[3], w, h);
    const adm_dwt_band_t_s csf_r = view3(s->cpu[4], w, h);
    const adm_dwt_band_t_s csf_fr = view3(s->cpu[5], w, h);

    adm_decouple_s(&ref, &dis, &r, &a, w, h, stride, stride, stride, stride, ADM_BORDER_FACTOR,
                   o->gain_limit);
    out[0] = adm_csf_den_scale_s(&ref, h, o->scale, w, h, stride, ADM_BORDER_FACTOR,
                                 DEFAULT_ADM_NORM_VIEW_DIST, DEFAULT_ADM_REF_DISPLAY_HEIGHT,
                                 DEFAULT_ADM_CSF_MODE, DEFAULT_ADM_CSF_LUMINANCE_LEVEL,
                                 DEFAULT_ADM_CSF_SCALE, DEFAULT_ADM_CSF_DIAG_SCALE, o->noise_weight,
                                 o->p_norm, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0);
    reference_csf(&a, &csf_a, &csf_fa, w, h, o);
    out[1] = reference_cm(&r, &csf_fa, &csf_a, w, h, o, o->noise_weight);
    reference_csf(&r, &csf_r, &csf_fr, w, h, o);
    out[2] = reference_cm(&a, &csf_fr, &csf_r, w, h, o, 0.0);
}

/* The term and row-sum kernels, then the host's fold and pooling. */
static void replay_scale(Scale *s, const FloatAdmCudaDecoupleArgs *a, const Options *o,
                         float out[3])
{
    const int w = a->bands.half_w;
    const int h = a->bands.half_h;
    const AdmBorderS r = adm_border_s(w, h, ADM_BORDER_FACTOR);
    const uint32_t region_w = (uint32_t)(r.right - r.left);
    const uint32_t region_h = (uint32_t)(r.bottom - r.top);
    const uint32_t is_cube = (o->p_norm == 3.0) ? 1u : 0u;
    const float p_norm = (float)o->p_norm;

    replay_decouple(s, a);
    for (uint32_t ry = 0u; ry < region_h; ry++) {
        for (uint32_t rx = 0u; rx < region_w; rx++) {
            const int x = r.left + (int)rx;
            const int y = r.top + (int)ry;
            float thr_a = 0.0f;
            float thr_r = 0.0f;
            if (!o->bypass_cm) {
                thr_a = fadm_threshold(s->csf_a, s->csf_fa, &a->bands, y, x);
                thr_r = fadm_threshold(s->csf_r, s->csf_fr, &a->bands, y, x);
            }
            for (uint32_t b = 0u; b < FADM_BANDS; b++) {
                const float src = s->ref[fadm_band_index((int)b + 1, y, x, w, h)];
                const size_t idx = fadm_band_index((int)b, y, x, w, h);
                s->terms[fadm_term_index(FADM_SLOT_DEN + b, rx, ry, region_w, region_h)] =
                    fadm_den_term(a->bands.rfactor[b], src, is_cube, p_norm);
                s->terms[fadm_term_index(FADM_SLOT_CM + b, rx, ry, region_w, region_h)] =
                    fadm_cm_term(s->csf_r[idx], thr_a, is_cube, p_norm);
                s->terms[fadm_term_index(FADM_SLOT_AIM + b, rx, ry, region_w, region_h)] =
                    fadm_cm_term(s->csf_a[idx], thr_r, is_cube, p_norm);
            }
        }
    }

    float accum[FADM_TERM_SLOTS];
    for (uint32_t slot = 0u; slot < FADM_TERM_SLOTS; slot++) {
        for (uint32_t ry = 0u; ry < region_h; ry++) {
            s->rows[fadm_row_index(slot, ry, region_h)] = fadm_row_sum(
                s->terms + fadm_term_index(slot, 0u, ry, region_w, region_h), region_h, region_w);
        }
        accum[slot] = fadm_fold_rows(s->rows + fadm_row_index(slot, 0u, region_h), region_h);
    }
    out[0] = adm_pool_bands_s(accum + FADM_SLOT_DEN, (int)region_w, (int)region_h, o->noise_weight,
                              o->p_norm);
    out[1] = adm_pool_bands_s(accum + FADM_SLOT_CM, (int)region_w, (int)region_h, o->noise_weight,
                              o->p_norm);
    out[2] = adm_pool_bands_s(accum + FADM_SLOT_AIM, (int)region_w, (int)region_h, 0.0, o->p_norm);
}

/* ------------------------------------------------------------------ */

/* Decouple inputs (diagonal band) whose restored value depends on how t / o is
 * formed. `quotient` is k * o with k the IEEE quotient t / (o + eps). On a
 * Ryzen 9 9950X3D the reciprocal refined from RCPSS, which the reference used
 * before ADR-1442, gives the neighbouring float named in each comment. */
typedef struct DivideCase {
    uint32_t o;
    uint32_t t;
    uint32_t quotient;
} DivideCase;

#define DIVIDE_CASES 4

static const DivideCase divide_cases[DIVIDE_CASES] = {
    {0xc1c597beu, 0xc19e4395u, 0xc19e4395u}, /* -24.6990929, -19.782999; estimate 0xc19e4396 */
    {0x4327548au, 0x42a9643cu, 0x42a9643cu}, /* 167.330231, 84.6957703; estimate 0x42a9643b */
    {0x42fda1e5u, 0x422582a9u, 0x422582a9u}, /* 126.8162, 41.3775978; estimate 0x422582aa */
    {0x41effe8du, 0x417e215bu, 0x417e215bu}, /* 29.9992924, 15.8831434; estimate 0x417e215d */
};

/* A 2x2 band: h and v are perpendicular between reference and distorted, so
 * the angle test fails and the gain limit stays out; d carries the cases. */
static void fill_divide_cases(Scale *s)
{
    const size_t plane = (size_t)DIVIDE_CASES;
    for (size_t i = 0; i < plane; i++) {
        s->ref[plane + i] = 1.0f;      /* h */
        s->ref[2u * plane + i] = 0.0f; /* v */
        s->dis[plane + i] = 0.0f;
        s->dis[2u * plane + i] = 1.0f;
        s->ref[3u * plane + i] = float_of(divide_cases[i].o); /* d */
        s->dis[3u * plane + i] = float_of(divide_cases[i].t);
    }
}

/* The reference's decouple and the shared header both restore with the IEEE
 * quotient. A reciprocal estimate in either fails here on a host whose
 * estimate differs from the quotient, as this one's does on every case. */
static char *test_decouple_divides(void)
{
    Scale s;
    mu_assert("allocation failed", scale_alloc(&s) == 0);
    fill_divide_cases(&s);
    const Options o = {
        .gain_limit = 100.0, .noise_weight = DEFAULT_ADM_NOISE_WEIGHT, .p_norm = 3.0};
    float unused[3];
    reference_scale(&s, 2, 2, &o, unused);
    const FloatAdmCudaDecoupleArgs a = decouple_args(2, 2, &o);

    char *msg = NULL;
    for (size_t i = 0; i < (size_t)DIVIDE_CASES && !msg; i++) {
        const float od = float_of(divide_cases[i].o);
        const float td = float_of(divide_cases[i].t);
        const volatile float k = td / (od + FADM_EPS);
        const volatile float ieee_quotient = k * od;
        const uint32_t cpu = bits_of(s.cpu[0][2u * (size_t)DIVIDE_CASES + i]);
        const uint32_t dev = bits_of(fadm_decouple_band(&a, od, td, 0));
        if (cpu != divide_cases[i].quotient || dev != divide_cases[i].quotient) {
            (void)fprintf(stderr, "\ndecouple case %zu: quotient=%08x cpu=%08x header=%08x\n", i,
                          divide_cases[i].quotient, cpu, dev);
        }
        if (bits_of(ieee_quotient) != divide_cases[i].quotient) {
            msg = "the IEEE quotient of a decouple case is not the recorded one";
        } else if (cpu != divide_cases[i].quotient) {
            msg = "adm_decouple_s() does not restore with the IEEE quotient";
        } else if (dev != divide_cases[i].quotient) {
            msg = "fadm_decouple_band() does not restore with the IEEE quotient";
        }
    }
    scale_free(&s);
    return msg;
}

static char *check_decouple(Scale *s, int w, int h, const Options *o)
{
    float unused[3];
    const FloatAdmCudaDecoupleArgs a = decouple_args(w, h, o);
    reference_scale(s, w, h, o, unused);
    replay_decouple(s, &a);

    const float *const dev[4] = {s->csf_a, s->csf_fa, s->csf_r, s->csf_fr};
    const size_t count = 3u * (size_t)w * (size_t)h;
    for (int k = 0; k < 4; k++) {
        for (size_t i = 0; i < count; i++) {
            const uint32_t want = bits_of(s->cpu[2 + k][i]);
            const uint32_t got = bits_of(dev[k][i]);
            if (want != got) {
                (void)fprintf(stderr, "\ndecouple+csf buffer %d sample %zu: cpu=%08x dev=%08x\n", k,
                              i, want, got);
            }
            mu_assert("decouple + CSF must equal adm_decouple_s() + adm_csf_s()", want == got);
        }
    }
    return NULL;
}

/* Bands below 25 samples a side: the reference's decouple and CSF then cover
 * the whole band, so every sample can be compared. */
static char *test_decouple_csf_matches_reference(void)
{
    Scale s;
    mu_assert("allocation failed", scale_alloc(&s) == 0);
    static const double gains[4] = {100.0, 1.2, 1.0, 3.7};
    static const float amplitudes[4] = {1.0f, 37.5f, 4000.0f, 1e-3f};
    char *msg = NULL;
    for (int trial = 0; trial < DECOUPLE_TRIALS && !msg; trial++) {
        const int w = 2 + (int)(rng_next() % 23u);
        const int h = 2 + (int)(rng_next() % 23u);
        const Options o = {.gain_limit = gains[trial % 4],
                           .noise_weight = DEFAULT_ADM_NOISE_WEIGHT,
                           .p_norm = 3.0,
                           .bypass_cm = 0,
                           .scale = trial % FADM_SCALES};
        fill_bands(&s, w, h, amplitudes[(trial / 4) % 4]);
        msg = check_decouple(&s, w, h, &o);
    }
    scale_free(&s);
    return msg;
}

static char *check_scale(Scale *s, int w, int h, const Options *o)
{
    float want[3];
    float got[3];
    const FloatAdmCudaDecoupleArgs a = decouple_args(w, h, o);
    reference_scale(s, w, h, o, want);
    replay_scale(s, &a, o, got);
    static const char *const names[3] = {"den_scale", "num_scale", "aim_num_scale"};
    for (int k = 0; k < 3; k++) {
        if (bits_of(want[k]) != bits_of(got[k])) {
            (void)fprintf(stderr, "\n%s %dx%d p=%g bypass=%d: cpu=%.9g dev=%.9g\n", names[k], w, h,
                          o->p_norm, o->bypass_cm, (double)want[k], (double)got[k]);
        }
        mu_assert("a scale's reductions must equal the reference's",
                  bits_of(want[k]) == bits_of(got[k]));
    }
    return NULL;
}

static char *test_scale_reductions_match_reference(void)
{
    Scale s;
    mu_assert("allocation failed", scale_alloc(&s) == 0);
    /* Bands of 14 samples or fewer keep their edges in the reduced region,
     * so the mirrored and clamped threshold taps are part of the sums. */
    static const int dims[8][2] = {{2, 2},   {5, 9},   {9, 5},    {14, 14},
                                   {15, 31}, {67, 43}, {136, 80}, {81, 21}};
    static const Options variants[5] = {
        {.gain_limit = 100.0, .noise_weight = DEFAULT_ADM_NOISE_WEIGHT, .p_norm = 3.0},
        {.gain_limit = 1.2, .noise_weight = DEFAULT_ADM_NOISE_WEIGHT, .p_norm = 3.0, .scale = 1},
        {.gain_limit = 100.0, .noise_weight = 0.0, .p_norm = 3.0, .scale = 2},
        {.gain_limit = 100.0, .noise_weight = 0.5, .p_norm = 3.0, .bypass_cm = 1, .scale = 3},
        /* Both sides call the host's powf() here; the device's differs. */
        {.gain_limit = 100.0, .noise_weight = DEFAULT_ADM_NOISE_WEIGHT, .p_norm = 2.0},
    };
    char *msg = NULL;
    for (int d = 0; d < 8 && !msg; d++) {
        for (int v = 0; v < 5 && !msg; v++) {
            fill_bands(&s, dims[d][0], dims[d][1], (v == 1) ? 250.0f : 12.0f);
            msg = check_scale(&s, dims[d][0], dims[d][1], &variants[v]);
        }
    }
    scale_free(&s);
    return msg;
}

/* The pooling routine replaced four spelled-out tails of adm_tools.c. At the
 * default exponent those wrote `powf(accum, 1.0f / 3.0f)`; the routine rounds
 * 1 / adm_p_norm to float, which has to be the same float. */
static char *test_pool_bands_is_the_reference_tail(void)
{
    for (int i = 0; i < 4096; i++) {
        const float accum[3] = {fabsf(rng_unit()) * 1e6f, fabsf(rng_unit()) * 30.0f,
                                (i % 7 == 0) ? 0.0f : fabsf(rng_unit())};
        const int rw = 1 + (int)(rng_next() % 1536u);
        const int rh = 1 + (int)(rng_next() % 864u);
        const double weight = (i % 3 == 0) ? 0.0 : DEFAULT_ADM_NOISE_WEIGHT;
        const float noise = powf((float)(rw * rh * weight), 1.0f / 3.0f);
        const float h = powf(accum[0], 1.0f / 3.0f) + noise;
        const float v = powf(accum[1], 1.0f / 3.0f) + noise;
        const float d = powf(accum[2], 1.0f / 3.0f) + noise;
        const float want = h + v + d;
        const float got = adm_pool_bands_s(accum, rw, rh, weight, 3.0);
        mu_assert("adm_pool_bands_s() at p = 3 must be the cube-root tail",
                  bits_of(want) == bits_of(got));
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_decouple_divides);
    mu_run_test(test_decouple_csf_matches_reference);
    mu_run_test(test_scale_reductions_match_reference);
    mu_run_test(test_pool_bands_is_the_reference_tail);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
