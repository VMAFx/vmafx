/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The gain terms integer_vif_metal's kernels compute, against the CPU's fp64
 * expressions, without a device (T-METAL-INTEGER-VIF-FP32-GAIN-2026-10-03,
 * ADR-1498; the SYCL twin's test_sycl_integer_vif_math.c is the model).
 *
 * integer_vif.c::vif_accumulate_pixel() (and the same lines in
 * x86/vif_avx2.c and x86/vif_avx512.c) forms a pixel's gain in fp64 and
 * truncates two results to integers before the log2 table. reference_terms()
 * below holds those lines verbatim (test_metal_integer_vif_gain_contract.py
 * pins that integer_vif.c still has them). The Metal kernels compile
 * feature/metal/metal_integer_vif_gain.h, the code this test runs on the
 * host. The cases:
 *
 *   - the replay alone on every sample (it must be the reference on all of
 *     them, since it is what an undecided sample gets);
 *   - the integer evaluation alone on every sample it claims to decide;
 *   - the selected value, which is what a kernel returns;
 *   - over random variances and over the cases that sit on a boundary:
 *     identical planes, quotients that are integers or just beside one, a
 *     gain at the limit, powers of two; and over variances computed from
 *     random pixel windows through the CPU's filter taps and rounding at 8,
 *     10 and 12 bits (scale 0) and 16-bit data (scales 1 to 3);
 *   - for the gain limits 100 (the default), 1, 37 and 2.5, 1.2, 12.75
 *     (integer and not);
 *   - the VmafMtlGainLimit layout the host binds and the kernels read.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "test.h"

#include "feature/integer_vif.h"
#include "feature/metal/metal_integer_vif_gain.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

enum {
    RANDOM_SAMPLES = 300000,
    WINDOW_SAMPLES = 25000,
    WINDOW_MODES = 4,
    SAMPLES = RANDOM_SAMPLES + WINDOW_SAMPLES * WINDOW_MODES,
    LIMIT_COUNT = 6,
    PATH_SELECTED = 0,
    PATH_INTEGER = 1,
    PATH_REPLAY = 2,
    MAX_TAPS = 17,
};

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static const double GAIN_LIMIT[LIMIT_COUNT] = {100.0, 1.0, 1.2, 37.0, 2.5, 12.75};

static uint32_t sigmas[SAMPLES * 3];
static uint32_t ref_sv[SAMPLES];
static int64_t ref_gg[SAMPLES];
static uint32_t got_sv[SAMPLES];
static int64_t got_gg[SAMPLES];
static uint32_t fast_sv[SAMPLES];
static int64_t fast_gg[SAMPLES];

/* integer_vif.c::vif_accumulate_pixel(), the lines between the two integer
 * accumulations, verbatim. */
static void reference_terms(int32_t sigma1_sq, int32_t sigma2_sq, int32_t sigma12,
                            double vif_enhn_gain_limit, uint32_t *sv_out, int64_t *gg_out)
{
    const double eps = 65536 * 1.0e-10;
    double g = sigma12 / (sigma1_sq + eps); // this epsilon can go away
    int32_t sv_sq = sigma2_sq - g * sigma12;

    sv_sq = (uint32_t)(MAX(sv_sq, 0));

    g = MIN(g, vif_enhn_gain_limit);

    *sv_out = (uint32_t)sv_sq;
    *gg_out = (int64_t)((g * g * sigma1_sq));
}

/* Deterministic generator: the same inputs on every host. */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static uint32_t clamp_variance(uint64_t value)
{
    if (value == 0u)
        return 1u;
    return value >= 0x80000000ULL ? 0x7fffffffu : (uint32_t)value;
}

/* A value in [1, 2^31) of a random magnitude; `near_power` puts it beside a
 * power of two. */
static uint32_t random_variance(int near_power)
{
    const unsigned bits = 1u + (unsigned)(rng_next() % 31u);
    uint64_t value = rng_next() & ((1ULL << bits) - 1u);
    if (near_power)
        value = (1ULL << (bits - 1u)) + (rng_next() % 3u) - 1u;
    return clamp_variance(value);
}

/* One sample; `kind` cycles through the boundary cases. */
static void fill_sample(unsigned kind, double limit, uint32_t *s)
{
    uint32_t s1 = random_variance((int)(rng_next() & 1u));
    if (s1 < 131072u)
        s1 += 131072u;
    uint32_t s2 = random_variance((int)(rng_next() & 1u));
    uint32_t s12 = random_variance((int)(rng_next() & 1u));
    switch (kind % 8u) {
    case 0: /* identical planes */
        s12 = s1;
        s2 = s1;
        break;
    case 1: /* sigma2_sq - g * sigma12 beside 0, 1 or 2 */
        s12 = clamp_variance(((uint64_t)s1 * (rng_next() % 1000u)) / 1000u + 1u);
        s2 = clamp_variance(((uint64_t)s12 * s12) / s1 + (rng_next() % 3u));
        break;
    case 2: /* sigma12^2 / sigma1_sq an integer or beside one */
        s12 = clamp_variance(((uint64_t)s1 * ((rng_next() % 4096u) + 1u)) >> 12);
        break;
    case 3: /* the gain at the limit */
        s12 = clamp_variance((uint64_t)llround(limit * (double)s1) + (rng_next() % 3u) - 1u);
        break;
    case 4: { /* sigma12^2 / sigma1_sq exactly an integer: sigma1_sq = a^2, sigma12 = a * b */
        const uint64_t a = 363u + rng_next() % 46000u;
        const uint64_t b = 1u + rng_next() % (a < 40000u ? a : 40000u);
        s1 = clamp_variance(a * a);
        s12 = clamp_variance(a * b);
        s2 = clamp_variance(b * b + 1u + rng_next() % 3u);
        break;
    }
    default: /* random */
        break;
    }
    s[0] = s1;
    s[1] = s2;
    s[2] = s12;
}

/* ------------------------------------------------------------------ */
/* Variances as the CPU's filter passes form them                       */
/* ------------------------------------------------------------------ */

typedef struct WindowMode {
    int scale;        /* 0, or 1 for the 16-bit data of scales 1 to 3 */
    unsigned bpc;     /* bits of the scale-0 samples */
    unsigned max_val; /* largest sample */
} WindowMode;

static const WindowMode WINDOW_MODE[WINDOW_MODES] = {
    {0, 8u, 255u},
    {0, 10u, 1023u},
    {0, 12u, 4095u},
    {1, 16u, 65535u},
};

/* integer_vif.c::vif_shift_for_scale()'s constants for the vertical pass:
 * the shift of the mean, and of the squared moments. */
static void vertical_shifts(const WindowMode *mode, unsigned *shift_mu, unsigned *shift_sq)
{
    *shift_mu = mode->scale == 0 ? mode->bpc : 16u;
    *shift_sq = mode->scale == 0 ? (mode->bpc - 8u) * 2u : 16u;
    if (mode->scale == 0 && mode->bpc == 8u)
        *shift_mu = 8u;
}

/* A sample of the window: a smooth base and a random texture, the distorted
 * plane a scaled and noised copy of the reference. */
static void random_window(const WindowMode *mode, uint32_t ref[MAX_TAPS][MAX_TAPS],
                          uint32_t dis[MAX_TAPS][MAX_TAPS])
{
    const uint64_t top = mode->max_val;
    const uint64_t base = rng_next() % (top + 1u);
    const uint64_t amp = 1u + rng_next() % (top / 2u + 1u);
    const uint64_t gain = rng_next() % 160u; /* 0 .. 1.59 in percent */
    const uint64_t noise = rng_next() % (top / 8u + 1u);
    for (int i = 0; i < MAX_TAPS; i++) {
        for (int j = 0; j < MAX_TAPS; j++) {
            const uint64_t r = MIN(top, base + rng_next() % amp);
            const uint64_t d = MIN(top, (r * gain) / 100u + rng_next() % (noise + 1u));
            ref[i][j] = (uint32_t)r;
            dis[i][j] = (uint32_t)d;
        }
    }
}

typedef struct Moments {
    uint32_t mu1, mu2, xx, yy, xy;
} Moments;

/* integer_vif.c's vertical pass (vif_vertical_line_8 / _16) of column j and
 * its horizontal pass (vif_horizontal_pixel) at the window's centre. */
static Moments window_moments(const WindowMode *mode, uint32_t ref[MAX_TAPS][MAX_TAPS],
                              uint32_t dis[MAX_TAPS][MAX_TAPS])
{
    const uint16_t *f = vif_filter1d_table[mode->scale];
    const int taps = vif_filter1d_width[mode->scale];
    const int off = (MAX_TAPS - taps) / 2;
    unsigned shift_mu = 0u, shift_sq = 0u;
    vertical_shifts(mode, &shift_mu, &shift_sq);
    const uint64_t add_mu = UINT64_C(1) << (shift_mu - 1u);
    const uint64_t add_sq = shift_sq == 0u ? 0u : UINT64_C(1) << (shift_sq - 1u);
    uint64_t acc_mu1 = 0, acc_mu2 = 0, acc_ref = 0, acc_dis = 0, acc_xy = 0;
    for (int j = 0; j < taps; j++) {
        uint64_t m1 = 0, m2 = 0, rr = 0, dd = 0, rd = 0;
        for (int i = 0; i < taps; i++) {
            const uint64_t r = ref[off + i][off + j], d = dis[off + i][off + j];
            m1 += f[i] * r;
            m2 += f[i] * d;
            rr += f[i] * r * r;
            dd += f[i] * d * d;
            rd += f[i] * r * d;
        }
        acc_mu1 += (uint64_t)f[j] * (uint32_t)((m1 + add_mu) >> shift_mu);
        acc_mu2 += (uint64_t)f[j] * (uint32_t)((m2 + add_mu) >> shift_mu);
        acc_ref += f[j] * ((rr + add_sq) >> shift_sq);
        acc_dis += f[j] * ((dd + add_sq) >> shift_sq);
        acc_xy += f[j] * ((rd + add_sq) >> shift_sq);
    }
    const Moments m = {(uint32_t)acc_mu1, (uint32_t)acc_mu2, (uint32_t)((acc_ref + 32768) >> 16),
                       (uint32_t)((acc_dis + 32768) >> 16), (uint32_t)((acc_xy + 32768) >> 16)};
    return m;
}

/* integer_vif.c::vif_accumulate_pixel()'s first lines. Returns 1 when the
 * pixel takes the log branch's gain (sigma1_sq >= 2 * 65536, sigma12 > 0,
 * sigma2_sq > 0). */
static int sigmas_of(Moments m, uint32_t *s)
{
    const uint32_t mu1_sq = (uint32_t)((((uint64_t)m.mu1 * m.mu1) + 2147483648) >> 32);
    const uint32_t mu2_sq = (uint32_t)((((uint64_t)m.mu2 * m.mu2) + 2147483648) >> 32);
    const uint32_t mu1_mu2 = (uint32_t)((((uint64_t)m.mu1 * m.mu2) + 2147483648) >> 32);
    const int32_t sigma1_sq = (int32_t)(m.xx - mu1_sq);
    int32_t sigma2_sq = (int32_t)(m.yy - mu2_sq);
    const int32_t sigma12 = (int32_t)(m.xy - mu1_mu2);
    sigma2_sq = MAX(sigma2_sq, 0);
    s[0] = (uint32_t)sigma1_sq;
    s[1] = (uint32_t)sigma2_sq;
    s[2] = (uint32_t)sigma12;
    return sigma1_sq >= (65536 << 1) && sigma12 > 0 && sigma2_sq > 0;
}

static unsigned fill_window_samples(unsigned mode_index, unsigned first)
{
    const WindowMode *mode = &WINDOW_MODE[mode_index];
    uint32_t ref[MAX_TAPS][MAX_TAPS], dis[MAX_TAPS][MAX_TAPS];
    unsigned taken = 0u;
    for (unsigned tries = 0u; taken < WINDOW_SAMPLES && tries < 40u * WINDOW_SAMPLES; tries++) {
        random_window(mode, ref, dis);
        if (sigmas_of(window_moments(mode, ref, dis), &sigmas[(size_t)(first + taken) * 3u]))
            taken++;
    }
    return taken;
}

static unsigned fill_samples(double limit)
{
    unsigned filled = 0u;
    rng_state = 0x9e3779b97f4a7c15ULL;
    for (unsigned i = 0u; i < RANDOM_SAMPLES; i++)
        fill_sample(i, limit, &sigmas[(size_t)i * 3u]);
    for (unsigned m = 0u; m < WINDOW_MODES; m++) {
        const unsigned first = RANDOM_SAMPLES + m * WINDOW_SAMPLES;
        const unsigned taken = fill_window_samples(m, first);
        filled += taken;
        /* A window mode that yields too few log-branch pixels pads with
         * boundary samples, so the arrays stay full. */
        for (unsigned i = taken; i < WINDOW_SAMPLES; i++)
            fill_sample(i, limit, &sigmas[(size_t)(first + i) * 3u]);
    }
    for (unsigned i = 0u; i < SAMPLES; i++) {
        const uint32_t *s = &sigmas[(size_t)i * 3u];
        reference_terms((int32_t)s[0], (int32_t)s[1], (int32_t)s[2], limit, &ref_sv[i], &ref_gg[i]);
    }
    return filled;
}

/* ------------------------------------------------------------------ */
/* The paths under test                                                 */
/* ------------------------------------------------------------------ */

static size_t run_path(double limit, int path, uint32_t *sv, int64_t *gg)
{
    const VmafMtlGainLimit gain_limit = vmaf_mtl_ivif_make_gain_limit(limit);
    size_t replays = 0u;
    for (size_t i = 0u; i < SAMPLES; i++) {
        const uint32_t *s = &sigmas[i * 3u];
        VmafMtlGainTerms terms;
        if (path == PATH_REPLAY) {
            terms = vmaf_mtl_ivif_gain_terms_replayed(s[0], s[1], s[2], gain_limit);
        } else if (path == PATH_INTEGER) {
            const VmafMtlIvifGainResult fast =
                vmaf_mtl_ivif_gain_terms_integer(s[0], s[1], s[2], gain_limit);
            terms = fast.terms;
            replays += fast.replay ? 1u : 0u;
        } else {
            terms = vmaf_mtl_ivif_gain_terms(s[0], s[1], s[2], gain_limit);
        }
        sv[i] = terms.sv_sq;
        gg[i] = terms.gg_sigma;
    }
    return replays;
}

static unsigned count_differing(const char *where, double limit, const uint32_t *sv,
                                const int64_t *gg)
{
    unsigned differing = 0u;
    for (unsigned i = 0u; i < SAMPLES; i++) {
        if (sv[i] == ref_sv[i] && gg[i] == ref_gg[i])
            continue;
        if (differing < 5u) {
            const uint32_t *s = &sigmas[(size_t)i * 3u];
            (void)fprintf(stderr,
                          "\n%s sample %u (limit %g) sigma1_sq=%u sigma2_sq=%u sigma12=%u: sv_sq "
                          "%u vs %u, gg %lld vs %lld",
                          where, i, limit, s[0], s[1], s[2], sv[i], ref_sv[i], (long long)gg[i],
                          (long long)ref_gg[i]);
        }
        differing++;
    }
    if (differing != 0u)
        (void)fprintf(stderr, "\n%s: %u of %d samples differ\n", where, differing, SAMPLES);
    return differing;
}

static char *test_window_samples_are_log_branch_pixels(void)
{
    const unsigned filled = fill_samples(GAIN_LIMIT[0]);
    mu_assert("the pixel windows yield too few log-branch pixels",
              filled >= (unsigned)(WINDOW_SAMPLES * WINDOW_MODES * 3u / 4u));
    return NULL;
}

static char *test_replay_is_the_fp64_arithmetic(void)
{
    for (unsigned l = 0u; l < LIMIT_COUNT; l++) {
        (void)fill_samples(GAIN_LIMIT[l]);
        (void)run_path(GAIN_LIMIT[l], PATH_REPLAY, got_sv, got_gg);
        mu_assert("the integer replay is not the reference's fp64 arithmetic",
                  count_differing("replay", GAIN_LIMIT[l], got_sv, got_gg) == 0u);
    }
    return NULL;
}

/* The integer evaluation is right wherever it claims to decide, and it hands
 * the boundary cases over: both halves of the selection are exercised. */
static char *test_integer_evaluation_decides_or_hands_over(void)
{
    for (unsigned l = 0u; l < LIMIT_COUNT; l++) {
        (void)fill_samples(GAIN_LIMIT[l]);
        const size_t replays = run_path(GAIN_LIMIT[l], PATH_INTEGER, fast_sv, fast_gg);
        (void)run_path(GAIN_LIMIT[l], PATH_SELECTED, got_sv, got_gg);
        unsigned decided_wrong = 0u;
        unsigned handed_over_and_wrong = 0u;
        for (unsigned i = 0u; i < SAMPLES; i++) {
            const int fast_right = fast_sv[i] == ref_sv[i] && fast_gg[i] == ref_gg[i];
            const int selected_is_fast = got_sv[i] == fast_sv[i] && got_gg[i] == fast_gg[i];
            decided_wrong += (!fast_right && selected_is_fast) ? 1u : 0u;
            handed_over_and_wrong += (!fast_right && !selected_is_fast) ? 1u : 0u;
        }
        mu_assert("the selected value is not the reference's fp64 arithmetic",
                  count_differing("selected", GAIN_LIMIT[l], got_sv, got_gg) == 0u);
        mu_assert("the integer evaluation decided a sample wrongly", decided_wrong == 0u);
        mu_assert("no sample takes the replay: the boundary cases are not reached",
                  replays > 0u && replays < (size_t)SAMPLES);
        mu_assert("no boundary sample shows the integer evaluation alone being wrong",
                  handed_over_and_wrong > 0u);
    }
    return NULL;
}

/* The pixel-window samples alone: the selection is the reference on them. */
static char *test_window_samples_selected_value(void)
{
    (void)fill_samples(GAIN_LIMIT[0]);
    (void)run_path(GAIN_LIMIT[0], PATH_SELECTED, got_sv, got_gg);
    for (unsigned i = RANDOM_SAMPLES; i < SAMPLES; i++)
        mu_assert("a pixel-window sample differs from the reference",
                  got_sv[i] == ref_sv[i] && got_gg[i] == ref_gg[i]);
    return NULL;
}

/* An fp32 gain, which the twin had before, is off by one on a share of the
 * samples: the test's reference sees it (the planted regression of the
 * contract is the source form of this). */
static char *test_fp32_gain_would_differ(void)
{
    (void)fill_samples(GAIN_LIMIT[0]);
    unsigned wrong = 0u;
    for (unsigned i = 0u; i < SAMPLES; i++) {
        const uint32_t *s = &sigmas[(size_t)i * 3u];
        const float g = (float)(int32_t)s[2] / ((float)(int32_t)s[0] + 65536.0f * 1.0e-10f);
        int sv = (int)s[1] - (int)(g * (float)(int32_t)s[2]);
        sv = MAX(sv, 0);
        const float gl = MIN(g, 100.0f);
        const int64_t gg = (int64_t)(gl * gl * (float)(int32_t)s[0]);
        wrong += ((uint32_t)sv != ref_sv[i] || gg != ref_gg[i]) ? 1u : 0u;
    }
    mu_assert("the fp32 gain agrees with the reference everywhere: the samples are not sharp",
              wrong > 0u);
    return NULL;
}

static char *test_gain_limit_layout_and_parts(void)
{
    mu_assert("VmafMtlGainLimit is not 32 bytes", sizeof(VmafMtlGainLimit) == 32u);
    mu_assert("VmafMtlGainLimit.value is not at offset 0", offsetof(VmafMtlGainLimit, value) == 0u);
    mu_assert("VmafMtlGainLimit.integer is not at offset 16",
              offsetof(VmafMtlGainLimit, integer) == 16u);
    mu_assert("VmafMtlGainLimit.hi is not at offset 20", offsetof(VmafMtlGainLimit, hi) == 20u);
    mu_assert("VmafMtlGainLimit.lo is not at offset 24", offsetof(VmafMtlGainLimit, lo) == 24u);
    for (unsigned l = 0u; l < LIMIT_COUNT; l++) {
        const VmafMtlGainLimit g = vmaf_mtl_ivif_make_gain_limit(GAIN_LIMIT[l]);
        const double value = ldexp((double)g.value.mant, g.value.exp);
        const int is_integer = GAIN_LIMIT[l] == floor(GAIN_LIMIT[l]);
        mu_assert("the limit's fp64 parts are not the limit", value == GAIN_LIMIT[l]);
        mu_assert("the limit's significand is not normalised",
                  g.value.mant >= (UINT64_C(1) << 52) && g.value.mant < (UINT64_C(1) << 53));
        mu_assert("the limit's integer form is wrong",
                  g.integer == (is_integer ? (uint32_t)GAIN_LIMIT[l] : 0u));
        mu_assert("the limit's fp32 pair is not the limit",
                  g.hi == (float)GAIN_LIMIT[l] &&
                      fabs((double)g.hi + (double)g.lo - GAIN_LIMIT[l]) <= 1.0e-13 * GAIN_LIMIT[l]);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_gain_limit_layout_and_parts);
    mu_run_test(test_window_samples_are_log_branch_pixels);
    mu_run_test(test_replay_is_the_fp64_arithmetic);
    mu_run_test(test_integer_evaluation_decides_or_hands_over);
    mu_run_test(test_window_samples_selected_value);
    mu_run_test(test_fp32_gain_would_differ);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
