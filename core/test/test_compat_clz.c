/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Netflix/vmaf#1551 / Netflix/vmaf#1422 regression test — the MSVC
 *  __builtin_clz / __builtin_clzll shim in core/src/feature/compat_builtin.h.
 *
 *  The shim used to be implemented with MSVC's `__lzcnt` / `__lzcnt64`
 *  (the form Netflix/vmaf#1422 proposes).  MSVC emits the LZCNT instruction
 *  unconditionally, with no runtime feature gate; on an x86-64 without
 *  ABM/LZCNT the `F3` prefix is ignored and the encoding retires as BSR,
 *  which returns the INDEX of the most-significant set bit rather than the
 *  leading-zero COUNT.  The two scalar call sites — integer_vif.h::log2_32
 *  (`k = 16 - clz`) and integer_adm.c::get_best15_from32 (`k = 17 - clz`) —
 *  then silently mis-normalise every VIF and ADM log2, and for large inputs
 *  shift by a negative count.  Netflix/vmaf#1551 is upstream's own retraction
 *  of that form; the fork now uses `_BitScanReverse`, which IS BSR by
 *  definition and exists on every x86-64 part.
 *
 *  The MSVC bodies cannot be compiled here, so this file pins the two halves
 *  that are portable:
 *
 *    1. `vmaf_compat_clz{32,64}_from_msb` — the `31 - msb` / `63 - msb`
 *       arithmetic that converts BSR's answer into a leading-zero count.
 *       This is exactly what the `__lzcnt` form got wrong.  The helpers are
 *       compiled on every platform and are what the MSVC shim calls.
 *
 *    2. Whatever `__builtin_clz` / `__builtin_clzll` resolve to in this
 *       translation unit (the compiler builtin here; the shim on the MSVC CI
 *       legs) must agree with a portable reference over a wide sweep.
 *
 *  The "shim must not be reimplemented with __lzcnt" half is enforced
 *  separately and unconditionally by scripts/ci/check-msvc-clz-shim.sh.
 */

#include <stdint.h>

#include "test.h"

#include "feature/compat_builtin.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but the Windows
 * MSVC legs compile the test tree with cl.exe, whose documented /std:clatest
 * C23 feature set does not include `nullptr`. Same carve-out and reasoning as
 * core/src/feature/float_motion.c. ADR-1138. */

/* Portable reference: count leading zeros of a 32-bit value. */
static int ref_clz32(uint32_t x)
{
    if (x == 0u)
        return 32;
    int n = 0;
    while ((x & 0x80000000u) == 0u) {
        x <<= 1;
        n++;
    }
    return n;
}

static int ref_clz64(uint64_t x)
{
    if (x == 0u)
        return 64;
    int n = 0;
    while ((x & 0x8000000000000000ull) == 0ull) {
        x <<= 1;
        n++;
    }
    return n;
}

/* Portable reference for BSR: index of the most-significant set bit. */
static int ref_msb32(uint32_t x, unsigned *idx)
{
    if (x == 0u)
        return 0;
    *idx = (unsigned)(31 - ref_clz32(x));
    return 1;
}

static int ref_msb64(uint64_t x, unsigned *idx)
{
    if (x == 0ull)
        return 0;
    *idx = (unsigned)(63 - ref_clz64(x));
    return 1;
}

/* ------------------------------------------------------------------ */

static char *test_clz32_from_msb_matches_reference(void)
{
    mu_assert("clz32_from_msb(0) must be 32", vmaf_compat_clz32_from_msb(0, 0) == 32);

    for (unsigned bit = 0; bit < 32u; bit++) {
        const uint32_t v = 1u << bit;
        unsigned idx = 0;
        const int found = ref_msb32(v, &idx);
        mu_assert("clz32_from_msb disagrees with the reference on a power of two",
                  vmaf_compat_clz32_from_msb(found, idx) == ref_clz32(v));
    }

    /* Values that straddle the 2^k boundaries, where the BSR-vs-LZCNT
     * confusion produced the 2048-LSB (factor-of-two) VIF log2 error. */
    static const uint32_t kCases[] = {
        1u, 2u, 3u, 0xFFFFu, 0x10000u, 0x1FFFFu, 0x100000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        unsigned idx = 0;
        const int found = ref_msb32(kCases[i], &idx);
        mu_assert("clz32_from_msb disagrees with the reference",
                  vmaf_compat_clz32_from_msb(found, idx) == ref_clz32(kCases[i]));
    }
    return NULL;
}

static char *test_clz64_from_msb_matches_reference(void)
{
    mu_assert("clz64_from_msb(0) must be 64", vmaf_compat_clz64_from_msb(0, 0) == 64);

    for (unsigned bit = 0; bit < 64u; bit++) {
        const uint64_t v = 1ull << bit;
        unsigned idx = 0;
        const int found = ref_msb64(v, &idx);
        mu_assert("clz64_from_msb disagrees with the reference on a power of two",
                  vmaf_compat_clz64_from_msb(found, idx) == ref_clz64(v));
    }
    return NULL;
}

/* On the MSVC CI legs this exercises the shim itself; elsewhere it anchors the
 * reference against the compiler builtin the shim has to imitate. */
static char *test_builtin_clz_matches_reference(void)
{
    static const uint32_t kCases32[] = {
        1u,       2u,       3u,        255u,        256u,        0xFFFFu,
        0x10000u, 0x1FFFFu, 0x100000u, 0x40000000u, 0x80000000u, 0xFFFFFFFFu,
    };
    for (size_t i = 0; i < sizeof(kCases32) / sizeof(kCases32[0]); i++) {
        mu_assert("__builtin_clz disagrees with the reference",
                  __builtin_clz(kCases32[i]) == ref_clz32(kCases32[i]));
    }

    static const uint64_t kCases64[] = {
        1ull,
        0x20000ull,
        0xFFFFFFFFull,
        0x100000000ull,
        0x8000000000000000ull,
        0xFFFFFFFFFFFFFFFFull,
    };
    for (size_t i = 0; i < sizeof(kCases64) / sizeof(kCases64[0]); i++) {
        mu_assert("__builtin_clzll disagrees with the reference",
                  __builtin_clzll(kCases64[i]) == ref_clz64(kCases64[i]));
    }

    /* The two shift expressions the defect actually corrupted. */
    mu_assert("integer_vif log2_32 shift must be 1 for 0x00010000",
              16 - __builtin_clz(0x00010000u) == 1);
    mu_assert("integer_vif log2_32 shift must be 16 for 0x80000000",
              16 - __builtin_clz(0x80000000u) == 16);
    mu_assert("integer_adm get_best15_from32 shift must be 2 for 0x00010000",
              17 - __builtin_clz(0x00010000u) == 2);
    mu_assert("integer_adm get_best15_from32 shift must be 16 for 0x40000000",
              17 - __builtin_clz(0x40000000u) == 16);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_clz32_from_msb_matches_reference);
    mu_run_test(test_clz64_from_msb_matches_reference);
    mu_run_test(test_builtin_clz_matches_reference);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
