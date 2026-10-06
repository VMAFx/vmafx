/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The CPU side of test_adm_decouple_recip.cpp: integer_adm.h's div_lookup and
 * integer_adm_kernels.h's adm_decouple_band(), behind a C interface. The
 * twin's header and the CPU kernels both define get_best15_from32(), so the
 * two cannot share a translation unit.
 */

#include <stdint.h>

#include "feature/integer_adm.h"
#include "feature/integer_adm_kernels.h"

#include "test_adm_decouple_recip_cpu.h"

/* div_lookup, filled once. On Windows div_lookup_generator() has no once
 * guard and refills all 65 537 entries on every call, so calling it per
 * sample, as these helpers did, took each executable past its 120 s timeout
 * on the UCRT64 leg (T-ADM-DECOUPLE-RECIP-TEST-WINDOWS-2026-10-06). The test
 * is single-threaded. */
static const int32_t *cpu_div_lookup(void)
{
    static int filled = 0;
    if (!filled) {
        div_lookup_generator();
        filled = 1;
    }
    return div_lookup;
}

/* The CPU's reciprocal of the operand `o` (1 <= |o| <= 32768). */
int32_t adm_recip_cpu_table(int o)
{
    return cpu_div_lookup()[o + 32768];
}

/* The CPU's decoupled sample for the band `o` / `t`. */
int adm_recip_cpu_sample(int o, int t, int angle_flag, double gain)
{
    return adm_decouple_band(cpu_div_lookup(), gain, angle_flag, (int16_t)o, (int16_t)t);
}
