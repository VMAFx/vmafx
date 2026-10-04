/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer VIF's residual variance takes x86's value on every target
 * (ADR-1561).
 *
 * integer_vif.c converts the fp64 difference sigma2_sq - g * sigma12 to an
 * integer and clamps it at 0. The difference reaches about -2^45; x86 returns
 * INT32_MIN for a value below int32_t's range (cvttsd2si, cvttpd2dq), so the
 * clamp gives 0, while an aarch64 build that vectorises the loop keeps the low
 * 32 bits of a 64-bit conversion. x86_sv_sq() spells x86's behaviour out and
 * is the reference here:
 *
 *   - vif_sv_sq() on the boundaries: (0, 1), 0, (-1, 0), INT32_MIN, far
 *     below it, 2^31 and INT32_MAX;
 *   - vif_sv_sq() in a loop a compiler may vectorise, on random variances;
 *   - vif_compute_line_residuals(), the scalar statistic the AVX2, AVX-512
 *     and NEON kernels share, on lines whose moments are those random
 *     variances (constant moment lines with zero means).
 *
 * About a quarter of the random variances put the difference below INT32_MIN;
 * the test asserts it sees some.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test.h"
#include "mu_table.h"
#include "feature/integer_vif.h"
#include "feature/integer_vif_sv_sq.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

enum { SAMPLES = 4096, LINE_W = 24, PAD = 16 };

static const int32_t sigma_nsq = 65536 << 1;

/* What x86 computes for upstream's `int32_t sv_sq = sigma2_sq - g * sigma12;
 * sv_sq = (uint32_t)(MAX(sv_sq, 0));`: a value outside int32_t's range
 * converts to INT32_MIN, then the clamp. */
static uint32_t x86_sv_sq(int32_t sigma2_sq, double g, int32_t sigma12)
{
    const double sv = sigma2_sq - g * sigma12;
    const int32_t converted = (sv > -2147483649.0 && sv < 2147483648.0) ? (int32_t)sv : INT32_MIN;
    return converted > 0 ? (uint32_t)converted : 0u;
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

typedef struct Variances {
    int32_t s1[SAMPLES];
    int32_t s2[SAMPLES];
    int32_t s12[SAMPLES];
} Variances;

/* sigma1_sq in the log branch, sigma2_sq and sigma12 positive: the inputs
 * integer_vif.c hands the conversion. */
static void fill_variances(Variances *v)
{
    rng_state = 0x9E3779B97F4A7C15ull;
    for (unsigned i = 0; i < SAMPLES; i++) {
        v->s1[i] = (int32_t)(131072u + rng_next() % (2147483647u - 131072u));
        v->s2[i] = (int32_t)(1u + rng_next() % 2147483646u);
        v->s12[i] = (int32_t)(1u + rng_next() % 2147483646u);
    }
}

static double gain(int32_t sigma1_sq, int32_t sigma12)
{
    const double eps = 65536 * 1.0e-10;
    return sigma12 / (sigma1_sq + eps);
}

typedef struct SvCase {
    int32_t sigma2_sq;
    double g;
    int32_t sigma12;
    uint32_t want;
} SvCase;

static char *test_sv_sq_boundaries(void)
{
    static const SvCase cases[] = {
        {1000, 0.0, 5, 1000u},                    /* g = 0: sigma2_sq itself */
        {1, 0.5, 1, 0u},                          /* 0.5 truncates to 0 */
        {7, 1.0, 7, 0u},                          /* exactly 0 */
        {1, 1.5, 1, 0u},                          /* -0.5 */
        {0, 1.0, INT32_MAX, 0u},                  /* -2^31 + 1, in range */
        {1, 1.0, INT32_MAX, 0u},                  /* -2^31 + 2 */
        {0, 2.0, 1073741824, 0u},                 /* exactly INT32_MIN */
        {1, 16384.0, INT32_MAX, 0u},              /* about -3.5e13 */
        {0, 1.0, INT32_MIN, 0u},                  /* +2^31: out of range */
        {INT32_MAX, 0.0, 1, (uint32_t)INT32_MAX}, /* largest value kept */
        {INT32_MAX, 0.25, 4, (uint32_t)INT32_MAX - 1u},
    };
    for (size_t i = 0; i < MU_TABLE_LEN(cases); i++) {
        const SvCase *c = &cases[i];
        mu_assert("x86_sv_sq() disagrees with the table: the reference is wrong",
                  x86_sv_sq(c->sigma2_sq, c->g, c->sigma12) == c->want);
        mu_assert("vif_sv_sq() is not x86's value on a boundary",
                  vif_sv_sq(c->sigma2_sq, c->g, c->sigma12) == c->want);
    }
    return NULL;
}

static Variances variances;
static uint32_t loop_sv[SAMPLES];

/* A plain loop over arrays: the shape an aarch64 compiler vectorises. */
static void sv_sq_loop(const Variances *v, uint32_t *out)
{
    for (unsigned i = 0; i < SAMPLES; i++)
        out[i] = vif_sv_sq(v->s2[i], gain(v->s1[i], v->s12[i]), v->s12[i]);
}

static char *test_sv_sq_vector_loop(void)
{
    fill_variances(&variances);
    sv_sq_loop(&variances, loop_sv);
    unsigned below = 0;
    unsigned bad = 0;
    for (unsigned i = 0; i < SAMPLES; i++) {
        const int32_t s1 = variances.s1[i];
        const int32_t s2 = variances.s2[i];
        const int32_t s12 = variances.s12[i];
        const double g = gain(s1, s12);
        below += (s2 - g * s12) < -2147483648.0;
        bad += loop_sv[i] != x86_sv_sq(s2, g, s12);
    }
    mu_assert("no sample put the difference below INT32_MIN", below > 0);
    mu_assert("vif_sv_sq() in a loop is not x86's value", bad == 0);
    return NULL;
}

static VifPublicState state;
static uint32_t line_mem[5][LINE_W + 2 * PAD];

/* Every column of the five moment lines holds one value, the means are 0, so
 * every pixel of the line has the moments (0, 0, xx, yy, xy). */
static void set_moment_lines(uint32_t xx, uint32_t yy, uint32_t xy)
{
    for (unsigned j = 0; j < LINE_W + 2 * PAD; j++) {
        line_mem[0][j] = 0u;
        line_mem[1][j] = 0u;
        line_mem[2][j] = xx;
        line_mem[3][j] = yy;
        line_mem[4][j] = xy;
    }
    state.buf.tmp.mu1 = line_mem[0] + PAD;
    state.buf.tmp.mu2 = line_mem[1] + PAD;
    state.buf.tmp.ref = line_mem[2] + PAD;
    state.buf.tmp.dis = line_mem[3] + PAD;
    state.buf.tmp.ref_dis = line_mem[4] + PAD;
}

/* A moment value after vif_horizontal_pixel()'s filter and rounding. */
static int32_t filtered(uint32_t value)
{
    uint64_t taps = 0;
    for (int fj = 0; fj < vif_filter1d_width[0]; fj++)
        taps += vif_filter1d_table[0][fj];
    return (int32_t)(uint32_t)((taps * value + 32768u) >> 16);
}

/* One pixel's accumulators, with x86's residual variance. */
static VifResiduals expected_pixel(int32_t s1, int32_t s2, int32_t s12, double limit)
{
    VifResiduals r = {0};
    r.accum_den_log = log2_32(state.log2_table, (uint32_t)sigma_nsq + (uint32_t)s1) - 2048 * 17;
    const double g = gain(s1, s12);
    const uint32_t numer1 = x86_sv_sq(s2, g, s12) + (uint32_t)sigma_nsq;
    const double gl = g < limit ? g : limit;
    const int64_t numer1_tmp = (int64_t)(gl * gl * s1) + numer1;
    r.accum_num_log =
        log2_64(state.log2_table, (uint64_t)numer1_tmp) - log2_64(state.log2_table, numer1);
    return r;
}

static char *check_line(int32_t s1, int32_t s2, int32_t s12)
{
    const VifResiduals want = expected_pixel(s1, s2, s12, state.vif_enhn_gain_limit);
    const VifResiduals got = vif_compute_line_residuals(&state, 0, LINE_W, 0);
    if (got.accum_den_log != want.accum_den_log * LINE_W)
        return "vif_compute_line_residuals(): denominator differs from the reference";
    if (got.accum_num_log != want.accum_num_log * LINE_W)
        return "vif_compute_line_residuals(): numerator is not x86's value";
    if (got.accum_num_non_log != 0 || got.accum_den_non_log != 0)
        return "vif_compute_line_residuals(): a log-branch pixel took the non-log branch";
    return NULL;
}

static char *test_line_residuals_take_x86_value(void)
{
    vif_log2_table_generate(state.log2_table);
    state.vif_enhn_gain_limit = DEFAULT_VIF_ENHN_GAIN_LIMIT;
    fill_variances(&variances);
    unsigned checked = 0;
    for (unsigned i = 0; i < SAMPLES; i++) {
        const int32_t s1 = filtered((uint32_t)variances.s1[i]);
        const int32_t s2 = filtered((uint32_t)variances.s2[i]);
        const int32_t s12 = filtered((uint32_t)variances.s12[i]);
        if (s1 < sigma_nsq || s2 <= 0 || s12 <= 0)
            continue;
        set_moment_lines((uint32_t)variances.s1[i], (uint32_t)variances.s2[i],
                         (uint32_t)variances.s12[i]);
        char *msg = check_line(s1, s2, s12);
        if (msg)
            return msg;
        checked++;
    }
    mu_assert("too few lines reached the log branch", checked > SAMPLES / 2);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_sv_sq_boundaries),
        MU_TEST(test_sv_sq_vector_loop),
        MU_TEST(test_line_residuals_take_x86_value),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
