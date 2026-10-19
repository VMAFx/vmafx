/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The scale-0 decouple of the CUDA and HIP ADM twins against the CPU's
 * adm_decouple_band() with integer_adm.h's div_lookup, on the host
 * (T-GPU-ADM-DECOUPLE-FP32-RECIPROCAL-2026-10-03).
 *
 * The twin's device function decouple_r_s0() is compiled here for the host: the
 * test defines the device qualifiers away and includes the kernel's own
 * header, named by ADM_TWIN_HEADER (one executable per twin). The cases:
 *   - decouple_r_s0() returns the CPU's sample for every operand against 27
 *     distorted values each, and against every distorted value for every
 *     operand whose fp32 quotient 2^30f / float(o) truncates to another
 *     integer than div_lookup holds (the twin used that quotient before).
 * Each case runs at gain limits 1 and 1.5 with the angle flag set and clear.
 *   - decouple_r_s123() returns adm_decouple_band_s123()'s sample, and both angle-flag
 *     functions the CPU's flag, on 400000 pseudo-random draws each (the scale 1-3 decouple
 *     and the flags are the other functions of the header the CUDA and HIP kernels include).
 *
 * Host-only: no device.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "test.h"

/* cl.exe has no __builtin_clz; the shim the CPU's own users include gives it
 * one (`__clz` below maps to it). check-msvc-clz-shim.sh requires the include
 * (T-ADM-DECOUPLE-RECIP-TEST-WINDOWS-2026-10-06). */
#include "feature/compat_builtin.h"
#include "test_adm_decouple_recip_cpu.h"

/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the device header
 * is written in the toolchain's own keywords (__device__, __forceinline__, __clz); a host
 * build of it has to define exactly those names. ADR-1416 (the twin's arithmetic is the
 * CPU's, held to it on the host), ADR-1142 (the standards apply to test code). */
#define __device__
#define __host__
#define __forceinline__ inline
#define __clz __builtin_clz
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
using std::max;
using std::min;

#include ADM_TWIN_HEADER

namespace
{

constexpr double kGains[2] = {1.0, 1.5};

int clamp16(int v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

/* The twin's sample for the band `o` / `t` (the other bands are inert). */
int twin_sample(int o, int t, int angle_flag, double gain)
{
    return decouple_r_s0((int16_t)o, 0, 0, (int16_t)t, 0, 0, 0, angle_flag, gain);
}

/* Samples of the twin that differ from the CPU's at one (o, t). */
unsigned mismatches_at(int o, int t)
{
    unsigned bad = 0u;
    for (const double gain : kGains) {
        for (int af = 0; af < 2; af++) {
            const int cpu = adm_recip_cpu_sample(o, t, af, gain);
            bad += twin_sample(o, t, af, gain) != cpu;
        }
    }
    return bad;
}

/* The quotient the twin used before: fp32, truncated. */
bool fp32_reciprocal_differs(int o)
{
    return o != 0 && (int32_t)(1073741824.0f / (float)o) != adm_recip_cpu_table(o);
}

const char *test_every_operand()
{
    static const int fixed_t[18] = {-32768, -32767, -20000, -4097, -1000, -65,  -64,  -3,    -2,
                                    0,      2,      3,      64,    65,    1000, 4097, 20000, 32767};
    unsigned bad = 0u;
    for (int o = -32768; o <= 32767; o++) {
        const int near_t[9] = {o - 1, o, o + 1, o / 2, -o, 2 * o, o / 3, 1, -1};
        for (const int t : fixed_t) {
            bad += mismatches_at(o, t);
        }
        for (const int t : near_t) {
            bad += mismatches_at(o, clamp16(t));
        }
    }
    if (bad) {
        (void)std::fprintf(stderr, "\n  %u samples differ from adm_decouple_band()\n", bad);
    }
    mu_assert("the scale-0 decouple is not the CPU's", bad == 0u);
    return nullptr;
}

const char *test_fp32_reciprocal_operands()
{
    unsigned operands = 0u;
    unsigned bad = 0u;
    for (int o = -32768; o <= 32767; o++) {
        if (!fp32_reciprocal_differs(o)) {
            continue;
        }
        operands++;
        for (int t = -32768; t <= 32767; t++) {
            bad += mismatches_at(o, t);
        }
    }
    (void)std::fprintf(stderr, "[%u operands] ", operands);
    mu_assert("the fp32 quotient is exact everywhere: the case tests nothing", operands > 0u);
    mu_assert("the scale-0 decouple is not the CPU's on a reciprocal operand", bad == 0u);
    return nullptr;
}

/* xorshift64*: a fixed stream, the same on every host. */
uint64_t next_random(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(0x2545f4914f6cdd1d);
}

/* A band sample of the 32-bit pipeline: a random sign and a magnitude of 1 to 31 bits,
 * so that both sides of the 2^15 split of the reciprocal are drawn. */
int32_t random_band32(uint64_t *state)
{
    const uint64_t bits = 1u + (next_random(state) % 31u);
    const int64_t magnitude = (int64_t)(next_random(state) >> (64u - bits));
    return (int32_t)(((next_random(state) & 1u) != 0u) ? -magnitude : magnitude);
}

/* decouple_r_s123() against adm_decouple_band_s123() (the scales 1-3 band, which also holds
 * get_best15_from32() and the gain-limited product). */
const char *test_scales_one_to_three()
{
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    unsigned bad = 0u;
    for (unsigned i = 0u; i < 400000u; i++) {
        const int32_t o = random_band32(&state);
        const int32_t t = ((next_random(&state) & 3u) == 0u) ? o : random_band32(&state);
        for (const double gain : kGains) {
            for (int af = 0; af < 2; af++) {
                const int32_t twin = decouple_r_s123(o, 0, 0, t, 0, 0, 0, af, gain);
                bad += twin != adm_recip_cpu_sample_s123(o, t, af, gain);
            }
        }
    }
    if (bad) {
        (void)std::fprintf(stderr, "\n  %u samples differ from adm_decouple_band_s123()\n", bad);
    }
    mu_assert("the scales 1-3 decouple is not the CPU's", bad == 0u);
    return nullptr;
}

/* A pair of 2-D vectors `bits` wide whose angle is a few degrees at most, so that the
 * 1-degree boundary is crossed in both directions. */
unsigned angle_flag_mismatches(uint64_t *state, unsigned bits, bool wide)
{
    unsigned bad = 0u;
    for (unsigned i = 0u; i < 400000u; i++) {
        const double radius = (double)((next_random(state) >> (64u - bits)) | 1u);
        const double angle = (double)(next_random(state) % 100000u) / 100000.0 * 0.07;
        const double tilt = (double)(next_random(state) % 100000u) / 100000.0 * 6.283185307179586;
        const double scale = 0.5 + (double)(next_random(state) % 1000u) / 2000.0;
        const int64_t oh = (int64_t)(radius * std::cos(tilt));
        const int64_t ov = (int64_t)(radius * std::sin(tilt));
        const int64_t th = (int64_t)(scale * radius * std::cos(tilt + angle));
        const int64_t tv = (int64_t)(scale * radius * std::sin(tilt + angle));
        const int64_t dot = oh * th + ov * tv;
        const int64_t o_sq = oh * oh + ov * ov;
        const int64_t t_sq = th * th + tv * tv;
        const int cpu = adm_recip_cpu_angle_flag(dot, o_sq, t_sq);
        const int twin =
            wide ? decouple_angle_flag_s123((int32_t)oh, (int32_t)ov, (int32_t)th, (int32_t)tv) :
                   decouple_angle_flag_s0((int16_t)oh, (int16_t)ov, (int16_t)th, (int16_t)tv);
        bad += twin != cpu;
    }
    return bad;
}

const char *test_angle_flags()
{
    uint64_t state = UINT64_C(0xd1b54a32d192ed03);
    const unsigned bad_s0 = angle_flag_mismatches(&state, 15u, false);
    const unsigned bad_s123 = angle_flag_mismatches(&state, 30u, true);
    if (bad_s0 || bad_s123) {
        (void)std::fprintf(stderr,
                           "\n  angle flags differ from the CPU's: %u (16-bit) %u (32-bit)\n",
                           bad_s0, bad_s123);
    }
    mu_assert("the scale-0 angle flag is not the CPU's", bad_s0 == 0u);
    mu_assert("the scales 1-3 angle flag is not the CPU's", bad_s123 == 0u);
    return nullptr;
}

/* The corners of the int16 range for the four bands. The twins' scale-0 angle flag adds two
 * int32 products; a sum reaches 2^31 only with every band at -32768, and the CPU's sums are int64
 * (T-GPU-ADM-ANGLE-FLAG-S0-INT32-CORNER-2026-10-06: 17 of 256 corners differed with int32 sums).
 * The twins form them so that the flag equals the CPU's at every corner. */
constexpr unsigned kWrapCornerMismatches = 0u;

/* One corner: whether the twin's flag differs from the CPU's, and whether a sum of the corner
 * is 2^31 (the value an int32 cannot hold). */
struct CornerVerdict {
    bool differs;
    bool wraps;
};

CornerVerdict judge_corner(int16_t oh, int16_t ov, int16_t th, int16_t tv)
{
    constexpr int64_t kWrap = INT64_C(1) << 31;
    const int64_t dot = (int64_t)oh * th + (int64_t)ov * tv;
    const int64_t o_sq = (int64_t)oh * oh + (int64_t)ov * ov;
    const int64_t t_sq = (int64_t)th * th + (int64_t)tv * tv;
    return {.differs =
                decouple_angle_flag_s0(oh, ov, th, tv) != adm_recip_cpu_angle_flag(dot, o_sq, t_sq),
            .wraps = dot == kWrap || o_sq == kWrap || t_sq == kWrap};
}

/* The corners that differ from the CPU's flag, split by whether a sum of the corner is 2^31. */
void count_corner_mismatches(unsigned *bad, unsigned *wrap_bad)
{
    static const int16_t kEdge[4] = {-32768, -32767, 32766, 32767};
    for (unsigned i = 0u; i < 256u; i++) {
        const CornerVerdict v = judge_corner(kEdge[i & 3u], kEdge[(i >> 2) & 3u],
                                             kEdge[(i >> 4) & 3u], kEdge[(i >> 6) & 3u]);
        *wrap_bad += (v.differs && v.wraps) ? 1u : 0u;
        *bad += (v.differs && !v.wraps) ? 1u : 0u;
    }
}

const char *test_angle_flag_corners()
{
    unsigned bad = 0u;
    unsigned wrap_bad = 0u;
    count_corner_mismatches(&bad, &wrap_bad);
    if (bad || wrap_bad != kWrapCornerMismatches) {
        (void)std::fprintf(stderr,
                           "\n  corner flags that differ from the CPU's: %u in range (expected 0), "
                           "%u of the 2^31 sums (expected %u)\n",
                           bad, wrap_bad, kWrapCornerMismatches);
    }
    mu_assert("the scale-0 angle flag is not the CPU's at the int16 corners", bad == 0u);
    mu_assert("the scale-0 angle flag is not the CPU's at the corners whose sums are 2^31",
              wrap_bad == kWrapCornerMismatches);
    return nullptr;
}

} // namespace

extern "C" const char *run_tests(void)
{
    mu_run_test(test_every_operand);
    mu_run_test(test_fp32_reciprocal_operands);
    mu_run_test(test_scales_one_to_three);
    mu_run_test(test_angle_flags);
    mu_run_test(test_angle_flag_corners);
    return nullptr;
}
