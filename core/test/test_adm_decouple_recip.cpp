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

} // namespace

extern "C" const char *run_tests(void)
{
    mu_run_test(test_every_operand);
    mu_run_test(test_fp32_reciprocal_operands);
    return nullptr;
}
