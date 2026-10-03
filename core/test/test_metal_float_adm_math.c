/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1498 -- what float_adm_metal's kernels run per work-item
 * (feature/metal/metal_float_adm_math.h, the Metal spelling of the SYCL
 * twin's sycl_float_adm_math.h, ADR-1434) against the CPU extractor's own
 * routines, bit for bit. The header is compiled here as C, on the host, so
 * the test needs no Apple device; it is the same text float_adm.metal
 * includes.
 *
 *   - The three expressions adm_tools.c evaluates in fp64 (the 1/30 product
 *     of adm_csf_s(), the centre tap of adm_cm_thresh3x3_s() and the
 *     enhancement-gain clamp of the decouple) against the same C expressions
 *     compiled here, over samples of every magnitude, subnormals and the
 *     neighbourhood of the clamp included. The device has no fp64 type: it
 *     evaluates them as exact fp32 pairs and replays the fp64 operations in
 *     integers where a pair does not decide the rounding. Both ways are
 *     checked, the replay also on its own.
 *   - One scale past the DWT, composed as the three kernels compose it
 *     (decouple, terms, row sums, with the kernels' own buffer indexing;
 *     then the host's fold and pooling), against adm_decouple_s(),
 *     adm_csf_s(), adm_csf_den_scale_s() and adm_cm_s(), on bands whose
 *     reduced region does and does not reach the band's edges.
 *
 * A quotient formed with a reciprocal, fp32 gain, fp32 1/30 and 1/15
 * constants, a threshold summed in another order, cos^2 * (|o|^2 * |t|^2) and
 * a sum per block each fail at least one of these.
 *
 * What this cannot show: that the same source compiles as Metal Shading
 * Language (the macOS job compiles it) and that the device's `/` and fma are
 * the correctly rounded ones (test_metal_float_adm_parity on a device).
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
#include "feature/metal/metal_float_adm_math.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define MAX_W 136
#define MAX_H 80
#define BAND_FLOATS ((size_t)MAX_W * MAX_H)
#define TERM_SLOTS 9u
#define SPOT_SAMPLES 1000000u
#define REPLAY_SAMPLES 150000u
#define DECOUPLE_TRIALS 64

/* adm_tools.c's constants: double literals, so not the fp32 values of those
 * names. test_metal_float_adm_exact_contract.py holds them to the source. */
#define REFERENCE_ONE_BY_30 0.0333333351
#define REFERENCE_ONE_BY_15 0.0666666701

/* Deterministic generator: the same inputs on every host. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
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

static bool same_float(float a, float b)
{
    return bits_of(a) == bits_of(b) || (isnan(a) && isnan(b));
}

/* Uniform in [-1, 1). */
static float rng_unit(void)
{
    return (float)((double)(rng_next() >> 40) / 8388608.0 - 1.0);
}

/* A non-negative float of assorted magnitude. */
static float pick_magnitude(void)
{
    switch (rng_next() % 6u) {
    case 0u: /* any finite value */
        return float_of((uint32_t)(rng_next() & 0x7f7fffffu));
    case 1u: /* band magnitudes */
        return (float)(rng_next() % 100000u) * (1.0f / 64.0f);
    case 2u: /* subnormal */
        return float_of((uint32_t)(rng_next() & 0x007fffffu));
    case 3u: /* mid range */
        return (float)(rng_next() >> 40) * 0x1p-12f;
    case 4u: /* next to one */
        return float_of(0x3f800000u + (uint32_t)(rng_next() % 64u));
    default: /* small or zero */
        return (rng_next() & 7u) ? (float)(rng_next() >> 44) * 0x1p-30f : 0.0f;
    }
}

/* adm_csf_s(): `flt = FLOAT_ONE_BY_30 * fabsf(dst)`. */
static float reference_flt(float csf)
{
    return (float)(REFERENCE_ONE_BY_30 * fabsf(csf));
}

/* adm_cm_thresh3x3_s(): `sum += FLOAT_ONE_BY_15 * fabsf(centre)`. */
static float reference_centre(float sum, float centre)
{
    float s = sum;
    s += REFERENCE_ONE_BY_15 * fabsf(centre);
    return s;
}

/* adm_decouple_band_s()'s clamp, with its MIN / MAX spelled out. */
static float reference_gain(float rst, float t, double limit)
{
    if (rst > 0.0f) {
        const double gained = rst * limit;
        rst = (float)(gained < t ? gained : t);
    }
    if (rst < 0.0f) {
        const double gained = rst * limit;
        rst = (float)(gained > t ? gained : t);
    }
    return rst;
}

/* Inputs of the fp64 expressions. */
typedef struct Spots {
    float *a;
    float *sum;
    float *rst;
    float *t;
    size_t n;
} Spots;

static int spots_alloc(Spots *s, size_t n)
{
    memset(s, 0, sizeof(*s));
    s->n = n;
    s->a = calloc(n, sizeof(float));
    s->sum = calloc(n, sizeof(float));
    s->rst = calloc(n, sizeof(float));
    s->t = calloc(n, sizeof(float));
    return (s->a && s->sum && s->rst && s->t) ? 0 : -1;
}

static void spots_free(Spots *s)
{
    free(s->a);
    free(s->sum);
    free(s->rst);
    free(s->t);
}

/* Samples of every magnitude and sign; a quarter of the clamp's inputs sit at
 * the gained product or one fp32 step either side of it. */
static void spots_fill(Spots *s, double limit)
{
    for (size_t i = 0; i < s->n; i++) {
        const float sign = (rng_next() & 1u) ? -1.0f : 1.0f;
        s->a[i] = sign * pick_magnitude();
        s->sum[i] = pick_magnitude();
        float rst = pick_magnitude();
        float t = pick_magnitude();
        const float at_limit = (float)((double)rst * limit);
        const unsigned mode = (unsigned)(rng_next() % 8u);
        if (mode == 0u) {
            t = at_limit;
        } else if (mode == 1u) {
            t = nextafterf(at_limit, INFINITY);
        } else if (mode == 2u) {
            t = nextafterf(at_limit, 0.0f);
        } else if (mode == 3u) {
            t = -t;
        }
        if (rng_next() & 1u) {
            rst = -rst;
            t = -t;
        }
        s->rst[i] = (rst == 0.0f) ? 1.0f : rst;
        s->t[i] = t;
    }
}

/* One sample's three expressions through the header, as the kernels select
 * (`replay_only` 0) or with the pair's decision replaced by the replay. */
static void twin_spot(const Spots *s, size_t i, const VmafMtlFadmGainLimit *limit, bool replay_only,
                      float out[3])
{
    const float a = fabsf(s->a[i]);
    const VmafMtlFadmConst one_by_30 = vmaf_mtl_fadm_one_by_30();
    const VmafMtlFadmConst one_by_15 = vmaf_mtl_fadm_one_by_15();
    const bool plain = a == 0.0f || !isfinite(a);
    if (replay_only && !plain) {
        out[0] = vmaf_mtl_fadm_times_constant_replayed(a, one_by_30);
    } else {
        out[0] = vmaf_mtl_fadm_csf_flt(s->a[i]);
    }
    if (replay_only && !plain && s->sum[i] != 0.0f && isfinite(s->sum[i])) {
        out[1] = vmaf_mtl_fadm_add_scaled_replayed(s->sum[i], a, one_by_15);
    } else {
        out[1] = vmaf_mtl_fadm_add_scaled(s->sum[i], a, one_by_15);
    }
    out[2] = vmaf_mtl_fadm_gain_limited(s->rst[i], s->t[i], *limit);
}

/* Every output of the header against the reference expressions. Returns the
 * number of samples whose 1/30 product the pair does not decide through
 * `undecided`. */
static char *spots_check(const Spots *s, double limit, bool replay_only, size_t *undecided)
{
    const VmafMtlFadmGainLimit gain = vmaf_mtl_fadm_make_gain_limit(limit);
    size_t wrong = 0u;
    size_t count = 0u;
    for (size_t i = 0; i < s->n; i++) {
        const float a = s->a[i];
        float got[3];
        twin_spot(s, i, &gain, replay_only, got);
        const float want[3] = {reference_flt(a), reference_centre(s->sum[i], a),
                               reference_gain(s->rst[i], s->t[i], limit)};
        const float mag = fabsf(a);
        if (mag > 0.0f && isfinite(mag) &&
            vmaf_mtl_fadm_undecided(vmaf_mtl_fadm_scaled_pair(mag, vmaf_mtl_fadm_one_by_30()))) {
            count++;
        }
        for (int k = 0; k < 3; k++) {
            if (same_float(want[k], got[k]))
                continue;
            if (wrong++ < 4u) {
                (void)fprintf(stderr,
                              "\nexpression %d sample %zu (a=%a sum=%a rst=%a t=%a limit=%g): "
                              "reference %a, twin %a\n",
                              k, i, (double)a, (double)s->sum[i], (double)s->rst[i],
                              (double)s->t[i], limit, (double)want[k], (double)got[k]);
            }
        }
    }
    *undecided = count;
    mu_assert("an fp64 expression of the reference is not reproduced", wrong == 0u);
    return NULL;
}

/* 100 and 1 are fp32 values, where the clamp's fp64 product is exact; the
 * others are not, and reach the replay. */
static const double GAIN_LIMITS[5] = {100.0, 1.0, 1.2, 37.3, 99.999};

static bool near_exact(double value, double hi, double lo, double bound)
{
    return fabs((hi + lo) - value) <= bound;
}

static char *test_constants_are_the_reference_literals(void)
{
    const VmafMtlFadmConst c30 = vmaf_mtl_fadm_one_by_30();
    const VmafMtlFadmConst c15 = vmaf_mtl_fadm_one_by_15();
    mu_assert("one_by_30 must be the double literal FLOAT_ONE_BY_30",
              ldexp((double)c30.mant, c30.exp) == REFERENCE_ONE_BY_30);
    mu_assert("one_by_15 must be the double literal FLOAT_ONE_BY_15",
              ldexp((double)c15.mant, c15.exp) == REFERENCE_ONE_BY_15);
    mu_assert("one_by_30's pair must be the literal to 2^-48",
              near_exact(REFERENCE_ONE_BY_30, c30.hi, c30.lo, REFERENCE_ONE_BY_30 * 0x1p-48));
    mu_assert("one_by_15's pair must be the literal to 2^-48",
              near_exact(REFERENCE_ONE_BY_15, c15.hi, c15.lo, REFERENCE_ONE_BY_15 * 0x1p-48));
    mu_assert("one_by_30's significand must be normalised",
              c30.mant >= (VMAF_MTL_U64(1) << 52) && c30.mant < (VMAF_MTL_U64(1) << 53));
    mu_assert("one_by_15's significand must be normalised",
              c15.mant >= (VMAF_MTL_U64(1) << 52) && c15.mant < (VMAF_MTL_U64(1) << 53));
    mu_assert("the decouple epsilon must be (float)1e-30", VMAF_MTL_FADM_EPS == (float)1e-30);
    mu_assert("the pair floor must be 2^-100", vmaf_mtl_fadm_fast_low() == 0x1p-100f);
    return NULL;
}

static char *check_spots(Spots *s, double limit, bool replay_only)
{
    size_t undecided = 0u;
    char *msg = spots_check(s, limit, replay_only, &undecided);
    if (msg)
        return msg;
    /* The pair decides most samples and not all: both ways are exercised. */
    mu_assert("no sample reached the replay", undecided > 0u);
    mu_assert("no sample was decided by the pair", undecided < s->n);
    return NULL;
}

static char *test_fp64_expressions(void)
{
    Spots s;
    mu_assert("allocation failed", spots_alloc(&s, SPOT_SAMPLES) == 0);
    char *msg = NULL;
    for (int l = 0; l < 5 && !msg; l++) {
        spots_fill(&s, GAIN_LIMITS[l]);
        msg = check_spots(&s, GAIN_LIMITS[l], false);
    }
    spots_free(&s);
    return msg;
}

static char *test_fp64_expressions_replay(void)
{
    Spots s;
    mu_assert("allocation failed", spots_alloc(&s, REPLAY_SAMPLES) == 0);
    char *msg = NULL;
    for (int l = 0; l < 5 && !msg; l++) {
        spots_fill(&s, GAIN_LIMITS[l]);
        msg = check_spots(&s, GAIN_LIMITS[l], true);
    }
    spots_free(&s);
    return msg;
}

/* The gain limit the host hands the kernels, in its two forms. */
static char *test_gain_limit_forms(void)
{
    const VmafMtlFadmGainLimit hundred = vmaf_mtl_fadm_make_gain_limit(100.0);
    const VmafMtlFadmGainLimit odd = vmaf_mtl_fadm_make_gain_limit(1.2);
    mu_assert("100 is an fp32 value", hundred.is_float == 1 && hundred.value == 100.0f);
    mu_assert("1.2 is not an fp32 value", odd.is_float == 0);
    mu_assert("the limit's significand must be normalised",
              odd.mant >= (VMAF_MTL_U64(1) << 52) && odd.mant < (VMAF_MTL_U64(1) << 53));
    mu_assert("the limit must be mant * 2^exp", ldexp((double)odd.mant, odd.exp) == 1.2);
    return NULL;
}

/* One scale's buffers in the kernels' layout, and the reference's results. */
typedef struct Scale {
    float *ref;    /* 4 sub-bands: a, h, v, d */
    float *dis;    /* 4 sub-bands */
    float *csf[4]; /* the twin's csf_a, csf_fa, csf_r, csf_fr */
    float *cpu[6]; /* the reference's decouple_r, decouple_a, csf_a, csf_fa, csf_r, csf_fr */
    float *terms;
    float *rows;
} Scale;

static int scale_alloc(Scale *s)
{
    memset(s, 0, sizeof(*s));
    s->ref = calloc(4u * BAND_FLOATS, sizeof(float));
    s->dis = calloc(4u * BAND_FLOATS, sizeof(float));
    s->terms = calloc((size_t)TERM_SLOTS * BAND_FLOATS, sizeof(float));
    s->rows = calloc((size_t)TERM_SLOTS * MAX_H, sizeof(float));
    bool ok = s->ref && s->dis && s->terms && s->rows;
    for (int i = 0; i < 4; i++) {
        s->csf[i] = calloc(3u * BAND_FLOATS, sizeof(float));
        ok = ok && s->csf[i];
    }
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
    free(s->terms);
    free(s->rows);
    for (int i = 0; i < 4; i++)
        free(s->csf[i]);
    for (int i = 0; i < 6; i++)
        free(s->cpu[i]);
}

/* The reference's (h, v, d) view of a three-sub-band buffer. */
static adm_dwt_band_t_s view3(float *buf, int w, int h)
{
    const size_t plane = (size_t)w * (size_t)h;
    const adm_dwt_band_t_s b = {
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
 * and zero samples, and whole samples whose (h, v) vectors are one degree
 * apart, where the outcome of the angle test depends on the association of
 * cos^2 * |o|^2 * |t|^2. */
static void fill_bands(Scale *s, int w, int h, float amplitude)
{
    const size_t plane = (size_t)w * (size_t)h;
    for (size_t i = 0; i < 4u * plane; i++) {
        const float o = rng_unit() * amplitude;
        float t = o * (0.5f + 0.75f * (rng_unit() + 1.0f)) + rng_unit() * amplitude * 0.05f;
        const unsigned kind = (unsigned)(rng_next() % 16u);
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
    for (size_t i = 0; i < plane; i += 3u) {
        const float gain = 1.0f + 0.4f * (rng_unit() + 1.0f);
        s->dis[plane + i] = s->ref[plane + i] * gain;
        s->dis[2u * plane + i] = s->ref[2u * plane + i] * gain;
        s->dis[3u * plane + i] = s->ref[3u * plane + i] * gain;
    }
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

/* One scale's inputs as the kernels' argument structs hold them. */
typedef struct ScaleInput {
    int w;
    int h;
    AdmBorderS region;
    VmafMtlFadmDecoupleArgs decouple;
    VmafMtlFadmTermArgs terms;
} ScaleInput;

static ScaleInput scale_input(int w, int h, const Options *o)
{
    ScaleInput in;
    memset(&in, 0, sizeof(in));
    in.w = w;
    in.h = h;
    in.region = adm_border_s(w, h, ADM_BORDER_FACTOR);
    float rfactor[3];
    adm_csf_rfactor_s(o->scale, DEFAULT_ADM_NORM_VIEW_DIST, DEFAULT_ADM_REF_DISPLAY_HEIGHT,
                      DEFAULT_ADM_CSF_MODE, DEFAULT_ADM_CSF_LUMINANCE_LEVEL, DEFAULT_ADM_CSF_SCALE,
                      DEFAULT_ADM_CSF_DIAG_SCALE, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
                      rfactor);
    in.decouple.limit = vmaf_mtl_fadm_make_gain_limit(o->gain_limit);
    in.decouple.half_w = w;
    in.decouple.half_h = h;
    in.decouple.buf_stride = w;
    in.decouple.cos_1deg_sq = adm_decouple_cos_1deg_sq_s();
    in.decouple.rfactor_h = rfactor[0];
    in.decouple.rfactor_v = rfactor[1];
    in.decouple.rfactor_d = rfactor[2];
    in.terms.half_w = w;
    in.terms.half_h = h;
    in.terms.buf_stride = w;
    in.terms.left = in.region.left;
    in.terms.top = in.region.top;
    in.terms.region_w = (vmaf_mtl_u32)(in.region.right - in.region.left);
    in.terms.region_h = (vmaf_mtl_u32)(in.region.bottom - in.region.top);
    in.terms.p_norm = (float)o->p_norm;
    in.terms.is_cube = (o->p_norm == 3.0) ? 1u : 0u;
    in.terms.bypass_cm = (o->bypass_cm != 0) ? 1u : 0u;
    in.terms.rfactor_h = rfactor[0];
    in.terms.rfactor_v = rfactor[1];
    in.terms.rfactor_d = rfactor[2];
    return in;
}

/* float_adm_decouple, for every sample: float_adm.metal's body, with the same
 * indexing. */
static void host_decouple(Scale *s, const ScaleInput *in)
{
    const VmafMtlFadmDecoupleArgs *a = &in->decouple;
    const int slice = a->buf_stride * a->half_h;
    for (int gy = 0; gy < a->half_h; gy++) {
        for (int gx = 0; gx < a->half_w; gx++) {
            const int at = gy * a->buf_stride + gx;
            const VmafMtlFadmDecouple c = vmaf_mtl_fadm_decouple_sample(
                a->limit, a->cos_1deg_sq, a->rfactor_h, a->rfactor_v, a->rfactor_d,
                s->ref[slice + at], s->ref[2 * slice + at], s->ref[3 * slice + at],
                s->dis[slice + at], s->dis[2 * slice + at], s->dis[3 * slice + at]);
            const VmafMtlFadmCsfSample bands[3] = {c.h, c.v, c.d};
            for (int b = 0; b < 3; b++) {
                s->csf[0][b * slice + at] = bands[b].csf_a;
                s->csf[1][b * slice + at] = bands[b].csf_fa;
                s->csf[2][b * slice + at] = bands[b].csf_r;
                s->csf[3][b * slice + at] = bands[b].csf_fr;
            }
        }
    }
}

/* fadm_taps() of float_adm.metal. */
static VmafMtlFadmBandTaps host_taps(const float *flt, const float *csf, int plane, int stride,
                                     VmafMtlFadmWindow w)
{
    const int above = plane + w.ym * stride;
    const int here = plane + w.y * stride;
    const int below = plane + w.yp * stride;
    const VmafMtlFadmNeighbours n = {flt[above + w.xm], flt[above + w.x], flt[above + w.xp],
                                     flt[here + w.xm],  flt[here + w.xp], flt[below + w.xm],
                                     flt[below + w.x],  flt[below + w.xp]};
    const VmafMtlFadmBandTaps taps = {n, csf[here + w.x]};
    return taps;
}

static float host_threshold(const float *flt, const float *csf, int slice, int stride,
                            VmafMtlFadmWindow w)
{
    return vmaf_mtl_fadm_threshold(host_taps(flt, csf, 0, stride, w),
                                   host_taps(flt, csf, slice, stride, w),
                                   host_taps(flt, csf, 2 * slice, stride, w));
}

/* float_adm_terms for one sample of the reduced region. */
static void host_terms_sample(Scale *s, const VmafMtlFadmTermArgs *a, unsigned rx, unsigned ry)
{
    const int x = a->left + (int)rx;
    const int y = a->top + (int)ry;
    const int slice = a->buf_stride * a->half_h;
    const VmafMtlFadmWindow w = vmaf_mtl_fadm_window(x, y, a->half_w, a->half_h);
    float thr_additive = 0.0f;
    float thr_restored = 0.0f;
    if (a->bypass_cm == 0u) {
        thr_additive = host_threshold(s->csf[1], s->csf[0], slice, a->buf_stride, w);
        thr_restored = host_threshold(s->csf[3], s->csf[2], slice, a->buf_stride, w);
    }
    const bool is_cube = a->is_cube != 0u;
    const int at = y * a->buf_stride + x;
    const float rfactor[3] = {a->rfactor_h, a->rfactor_v, a->rfactor_d};
    for (int b = 0; b < 3; b++) {
        const VmafMtlFadmBandTerms t = vmaf_mtl_fadm_band_terms(
            rfactor[b], s->ref[(b + 1) * slice + at], s->csf[2][b * slice + at],
            s->csf[0][b * slice + at], thr_additive, thr_restored, is_cube, a->p_norm);
        const vmaf_mtl_u32 base = (vmaf_mtl_u32)b;
        s->terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_DEN + base, rx, ry, a->region_w,
                                          a->region_h)] = t.den;
        s->terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_CM + base, rx, ry, a->region_w,
                                          a->region_h)] = t.cm;
        s->terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_AIM + base, rx, ry, a->region_w,
                                          a->region_h)] = t.aim;
    }
}

/* float_adm_rows for every (slot, row): one fp32 accumulator, left to right. */
static void host_rows(Scale *s, const VmafMtlFadmTermArgs *a)
{
    for (vmaf_mtl_u32 id = 0u; id < VMAF_MTL_FADM_TERM_SLOTS * a->region_h; id++) {
        const vmaf_mtl_u32 slot = id / a->region_h;
        const vmaf_mtl_u32 y = id - slot * a->region_h;
        float inner = 0.0f;
        for (vmaf_mtl_u32 x = 0u; x < a->region_w; x++) {
            inner += s->terms[vmaf_mtl_fadm_term_index(slot, x, y, a->region_w, a->region_h)];
        }
        s->rows[id] = inner;
    }
}

/* The three kernels over the scale, then the host's fold and pooling:
 * den_scale, num_scale and aim_num_scale in out[0..2]. */
static void twin_scale(Scale *s, int w, int h, const Options *o, float out[3])
{
    const ScaleInput in = scale_input(w, h, o);
    host_decouple(s, &in);
    for (vmaf_mtl_u32 ry = 0u; ry < in.terms.region_h; ry++) {
        for (vmaf_mtl_u32 rx = 0u; rx < in.terms.region_w; rx++) {
            host_terms_sample(s, &in.terms, rx, ry);
        }
    }
    host_rows(s, &in.terms);
    const int region_w = in.region.right - in.region.left;
    const int region_h = in.region.bottom - in.region.top;
    float accum[TERM_SLOTS];
    for (unsigned slot = 0u; slot < TERM_SLOTS; slot++) {
        accum[slot] = vmaf_mtl_fadm_fold_rows(s->rows + (size_t)slot * (size_t)region_h,
                                              (vmaf_mtl_u32)region_h);
    }
    out[0] = adm_pool_bands_s(accum + VMAF_MTL_FADM_SLOT_DEN, region_w, region_h, o->noise_weight,
                              o->p_norm);
    out[1] = adm_pool_bands_s(accum + VMAF_MTL_FADM_SLOT_CM, region_w, region_h, o->noise_weight,
                              o->p_norm);
    out[2] = adm_pool_bands_s(accum + VMAF_MTL_FADM_SLOT_AIM, region_w, region_h, 0.0, o->p_norm);
}

/* Every sample of the twin's four CSF buffers against the reference's. */
static char *compare_csf(const Scale *s, int w, int h)
{
    const size_t count = 3u * (size_t)w * (size_t)h;
    for (int k = 0; k < 4; k++) {
        for (size_t i = 0; i < count; i++) {
            const uint32_t want = bits_of(s->cpu[2 + k][i]);
            const uint32_t got = bits_of(s->csf[k][i]);
            if (want != got) {
                (void)fprintf(stderr, "\ndecouple+csf buffer %d sample %zu: cpu=%08x twin=%08x\n",
                              k, i, want, got);
            }
            mu_assert("decouple + CSF must equal adm_decouple_s() + adm_csf_s()", want == got);
        }
    }
    return NULL;
}

/* Bands below 25 samples a side: the reference's decouple and CSF then cover
 * the whole band, so every sample can be compared. */
static char *check_decouple(Scale *s)
{
    static const double gains[4] = {100.0, 1.2, 1.0, 3.7};
    static const float amplitudes[4] = {1.0f, 37.5f, 4000.0f, 1e-3f};
    for (int trial = 0; trial < DECOUPLE_TRIALS; trial++) {
        const int w = 2 + (int)(rng_next() % 23u);
        const int h = 2 + (int)(rng_next() % 23u);
        const Options o = {.gain_limit = gains[trial % 4],
                           .noise_weight = DEFAULT_ADM_NOISE_WEIGHT,
                           .p_norm = 3.0,
                           .bypass_cm = 0,
                           .scale = trial % 4};
        float want[3];
        float got[3];
        fill_bands(s, w, h, amplitudes[(trial / 4) % 4]);
        reference_scale(s, w, h, &o, want);
        twin_scale(s, w, h, &o, got);
        mu_assert_msg(compare_csf(s, w, h));
    }
    return NULL;
}

/* One band size and option set: the three pooled values of the twin against
 * the reference's. */
static char *check_reduction_case(Scale *s, int w, int h, const Options *o)
{
    static const char *const names[3] = {"den_scale", "num_scale", "aim_num_scale"};
    float want[3];
    float got[3];
    reference_scale(s, w, h, o, want);
    twin_scale(s, w, h, o, got);
    for (int k = 0; k < 3; k++) {
        if (bits_of(want[k]) != bits_of(got[k])) {
            (void)fprintf(stderr, "\n%s %dx%d p=%g bypass=%d: cpu=%.9g twin=%.9g\n", names[k], w, h,
                          o->p_norm, o->bypass_cm, (double)want[k], (double)got[k]);
        }
        mu_assert("a scale's reductions must equal the reference's",
                  bits_of(want[k]) == bits_of(got[k]));
    }
    return NULL;
}

static char *check_reductions(Scale *s)
{
    /* Bands of 14 samples or fewer keep their edges in the reduced region,
     * so the mirrored and clamped threshold taps are part of the sums. */
    static const int dims[8][2] = {{2, 2},   {5, 9},   {9, 5},    {14, 14},
                                   {15, 31}, {67, 43}, {136, 80}, {81, 21}};
    static const Options variants[4] = {
        {.gain_limit = 100.0, .noise_weight = DEFAULT_ADM_NOISE_WEIGHT, .p_norm = 3.0},
        {.gain_limit = 1.2, .noise_weight = DEFAULT_ADM_NOISE_WEIGHT, .p_norm = 3.0, .scale = 1},
        {.gain_limit = 100.0, .noise_weight = 0.0, .p_norm = 3.0, .scale = 2},
        {.gain_limit = 100.0, .noise_weight = 0.5, .p_norm = 3.0, .bypass_cm = 1, .scale = 3},
    };
    for (int d = 0; d < 8; d++) {
        for (int v = 0; v < 4; v++) {
            fill_bands(s, dims[d][0], dims[d][1], (v == 1) ? 250.0f : 12.0f);
            mu_assert_msg(check_reduction_case(s, dims[d][0], dims[d][1], &variants[v]));
        }
    }
    return NULL;
}

static char *with_scale(char *(*check)(Scale *))
{
    Scale s;
    mu_assert("allocation failed", scale_alloc(&s) == 0);
    char *msg = check(&s);
    scale_free(&s);
    return msg;
}

static char *test_decouple_csf(void)
{
    return with_scale(check_decouple);
}

static char *test_scale_reductions(void)
{
    return with_scale(check_reductions);
}

char *run_tests(void)
{
    mu_run_test(test_constants_are_the_reference_literals);
    mu_run_test(test_gain_limit_forms);
    mu_run_test(test_fp64_expressions);
    mu_run_test(test_fp64_expressions_replay);
    mu_run_test(test_decouple_csf);
    mu_run_test(test_scale_reductions);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
