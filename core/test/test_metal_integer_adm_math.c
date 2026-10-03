/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The decouple of integer_adm_metal (core/src/feature/metal/
 * metal_integer_adm_math.h, ADR-1498; T-METAL-ADM-GAIN-LIMIT-FLOAT32-
 * 2026-10-01) against the CPU's adm_decouple_band() and
 * adm_decouple_band_s123() (integer_adm_kernels.h, with integer_adm.h's
 * div_lookup), on the host. The header is the kernel's arithmetic, compiled
 * here as C; its gain limit is adm_gain_limit_product() of the shared
 * adm_gain_limit.h on the limit adm_gain_limit_split() returns.
 *
 * The cases, at gain limits 1, 1.2, 1.5 and 100 (and two more at scales 1-3)
 * with the angle flag set and clear:
 *   - scale 0: every reference operand o of the int16 band against 27
 *     distorted values each (the range's edges, small values, and values next
 *     to o, where the clamp and the gain limit decide);
 *   - scale 0: every o whose fp32 reciprocal 2^30 / (float)o, which the
 *     kernel used to take, truncates to another integer than div_lookup's,
 *     against every distorted value: the header returns the CPU's sample on
 *     all of them, the former reciprocal does not;
 *   - scales 1-3: two million int32 pairs over every magnitude class plus the
 *     edges: the header returns the CPU's sample, while the former binary32
 *     product of the restored sample and the limit does not at 1.2, 1.5 and
 *     100.
 *
 * Host-only: no device.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "test.h"

#include "feature/integer_adm.h"
#include "feature/integer_adm_kernels.h"
#include "feature/metal/metal_integer_adm_math.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const double GAINS[4] = {1.0, 1.2, 1.5, 100.0};
static const double GAINS_S123[6] = {1.0, 1.2, 1.5, 100.0, 1.37, 99.99};

static int clamp16(int v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

/* The kernel's former scale-0 decouple: the fp32 reciprocal and the binary32
 * gain product (integer_adm.metal before ADR-1498). */
static int old_decouple_s0(int o, int t, int af, double gain)
{
    const float q = 1073741824.0f / (float)o;
    const int tmp_k = (o == 0) ? 32768 : (int)((((int64_t)(int32_t)q * t) + 16384) >> 15);
    const int k = tmp_k < 0 ? 0 : (tmp_k > 32768 ? 32768 : tmp_k);
    const float egl = af ? (float)gain : 1.0f;
    int rst = (int)((float)(((k * o) + 16384) >> 15) * egl);
    const int rst_s = k * o;
    if (af && rst_s > 0) {
        rst = rst < t ? rst : t;
    }
    if (af && rst_s < 0) {
        rst = rst > t ? rst : t;
    }
    return rst;
}

/* Samples of the header that differ from the CPU's at one (o, t), over the
 * gains and both angle flags. */
static unsigned s0_mismatches(int o, int t)
{
    unsigned bad = 0u;
    for (unsigned gi = 0; gi < 4u; gi++) {
        const struct AdmGainLimit g = adm_gain_limit_split(GAINS[gi]);
        for (int af = 0; af < 2; af++) {
            const int cpu = adm_decouple_band(div_lookup, GAINS[gi], af, (int16_t)o, (int16_t)t);
            bad += vmaf_mtl_iadm_decouple_s0(o, t, af, g) != cpu;
        }
    }
    return bad;
}

static char *test_decouple_s0_every_operand(void)
{
    static const int fixed_t[18] = {-32768, -32767, -20000, -4097, -1000, -65,  -64,  -3,    -2,
                                    0,      2,      3,      64,    65,    1000, 4097, 20000, 32767};
    div_lookup_generator();
    unsigned bad = 0u;
    for (int o = -32768; o <= 32767; o++) {
        const int near_t[9] = {o - 1, o, o + 1, o / 2, -o, 2 * o, o / 3, 1, -1};
        for (unsigned i = 0; i < 18u; i++) {
            bad += s0_mismatches(o, fixed_t[i]);
        }
        for (unsigned i = 0; i < 9u; i++) {
            bad += s0_mismatches(o, clamp16(near_t[i]));
        }
    }
    if (bad) {
        (void)fprintf(stderr, "\n  scale 0: %u samples differ from adm_decouple_band()\n", bad);
    }
    mu_assert("the scale-0 decouple is not the CPU's", bad == 0u);
    return NULL;
}

/* Every distorted value at one operand whose fp32 reciprocal is wrong: the
 * header's mismatches (`bad`) and the former kernel's (`old_bad`). */
static void reciprocal_operand(int o, unsigned *bad, unsigned *old_bad)
{
    const struct AdmGainLimit g = adm_gain_limit_split(100.0);
    for (int t = -32768; t <= 32767; t++) {
        const int cpu = adm_decouple_band(div_lookup, 100.0, 0, (int16_t)o, (int16_t)t);
        *bad += vmaf_mtl_iadm_decouple_s0(o, t, 0, g) != cpu;
        *old_bad += old_decouple_s0(o, t, 0, 100.0) != cpu;
    }
}

static char *test_decouple_s0_reciprocal_operands(void)
{
    div_lookup_generator();
    unsigned operands = 0u;
    unsigned bad = 0u;
    unsigned old_bad = 0u;
    for (int o = -32768; o <= 32767; o++) {
        if (o == 0 || (int32_t)(1073741824.0f / (float)o) == div_lookup[o + 32768]) {
            continue;
        }
        operands++;
        reciprocal_operand(o, &bad, &old_bad);
    }
    (void)fprintf(stderr, "[%u operands, former kernel off on %u samples] ", operands, old_bad);
    mu_assert("the fp32 reciprocal is exact everywhere: the case tests nothing", operands > 0u);
    mu_assert("the former reciprocal never moves k: the case tests nothing", old_bad > 0u);
    mu_assert("the scale-0 decouple is not the CPU's on a reciprocal operand", bad == 0u);
    return NULL;
}

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* An int32 operand of a random magnitude class (never INT32_MIN, whose abs()
 * the CPU does not define). */
static int32_t s123_operand(uint64_t bits)
{
    int32_t v = (int32_t)(uint32_t)bits;
    v /= (int32_t)(1 << ((bits >> 40) % 31u));
    return v == INT32_MIN ? INT32_MIN + 1 : v;
}

/* The kernel's former scales-1-3 restored sample and gain product. */
static int32_t old_decouple_s123(int32_t o, int32_t t, int af, double gain)
{
    const int64_t k = vmaf_mtl_iadm_k_s123(o, t);
    const float egl = af ? (float)gain : 1.0f;
    int32_t rst = (int32_t)((float)(((k * o) + 16384) >> 15) * egl);
    const float rst_f = ((float)k / 32768.0f) * ((float)o / 64.0f);
    if (af && rst_f > 0.0f) {
        rst = rst < t ? rst : t;
    }
    if (af && rst_f < 0.0f) {
        rst = rst > t ? rst : t;
    }
    return rst;
}

/* The header against adm_decouple_band_s123() at one gain; `old_bad` counts
 * the former kernel's mismatches with the angle flag set. */
static unsigned s123_mismatches(double gain, int32_t o, int32_t t, unsigned *old_bad)
{
    const struct AdmGainLimit g = adm_gain_limit_split(gain);
    unsigned bad = 0u;
    for (int af = 0; af < 2; af++) {
        const int32_t cpu = adm_decouple_band_s123(div_lookup, gain, af, o, t);
        bad += vmaf_mtl_iadm_decouple_s123(o, t, af, g) != cpu;
        *old_bad += af && old_decouple_s123(o, t, af, gain) != cpu;
    }
    return bad;
}

static char *test_decouple_s123_pairs(void)
{
    static const int32_t edges[10] = {0,     1,     -1,     32767,     -32767,
                                      32768, 65535, -65536, INT32_MAX, INT32_MIN + 1};
    div_lookup_generator();
    unsigned bad = 0u;
    unsigned old_bad[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint64_t state = 0x9E3779B97F4A7C15u;
    for (unsigned i = 0; i < 2000000u; i++) {
        const uint64_t a = xorshift64(&state);
        const uint64_t b = xorshift64(&state);
        const int32_t o = (i < 100u) ? edges[i % 10u] : s123_operand(a);
        const int32_t t = (i < 100u) ? edges[i / 10u] : s123_operand(b);
        for (unsigned gi = 0; gi < 6u; gi++) {
            bad += s123_mismatches(GAINS_S123[gi], o, t, &old_bad[gi]);
        }
    }
    (void)fprintf(stderr, "[former kernel off at 1.2/1.5/100: %u/%u/%u] ", old_bad[1], old_bad[2],
                  old_bad[3]);
    mu_assert("the scales-1-3 decouple is not the CPU's", bad == 0u);
    mu_assert("the former binary32 product matches at 1.2, 1.5 and 100: the case tests nothing",
              old_bad[1] > 0u && old_bad[2] > 0u && old_bad[3] > 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_decouple_s0_every_operand);
    mu_run_test(test_decouple_s0_reciprocal_operands);
    mu_run_test(test_decouple_s123_pairs);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
