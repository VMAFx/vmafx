/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The host side of core/src/feature/metal/metal_portable.h (ADR-1498): the
 * prelude the Metal twins' arithmetic headers compile on, in a kernel and on
 * the host. Each primitive must be the operation its Metal spelling is
 * (Metal Shading Language Specification 4.1): as_type<> a bit cast both ways,
 * clz() the leading zero bits of a 64-bit value with 64 for zero, fma() one
 * rounding. A host test of a Metal arithmetic header is only as good as these.
 */

#include <math.h>
#include <stdint.h>

#include "test.h"

#include "feature/metal/metal_portable.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static char *test_bit_casts_round_trip(void)
{
    mu_assert("1.0f is 0x3f800000", VMAF_MTL_F2U(1.0f) == 0x3f800000u);
    mu_assert("0x3f800000 is 1.0f", VMAF_MTL_U2F(0x3f800000u) == 1.0f);
    mu_assert("-0.0f keeps its sign bit", VMAF_MTL_F2U(-0.0f) == 0x80000000u);
    mu_assert("the smallest subnormal survives", VMAF_MTL_U2F(1u) > 0.0f);
    return NULL;
}

static char *test_clz64(void)
{
    mu_assert("clz(0) is 64", VMAF_MTL_CLZ64(0u) == 64u);
    mu_assert("clz(1) is 63", VMAF_MTL_CLZ64(1u) == 63u);
    mu_assert("clz(2^52) is 11", VMAF_MTL_CLZ64(VMAF_MTL_U64(1) << 52) == 11u);
    mu_assert("clz(2^63) is 0", VMAF_MTL_CLZ64(VMAF_MTL_U64(1) << 63) == 0u);
    mu_assert("clz(2^53 - 1) is 11", VMAF_MTL_CLZ64((VMAF_MTL_U64(1) << 53) - 1u) == 11u);
    return NULL;
}

/* (1 + 2^-23)^2 - (1 + 2^-22) = 2^-46 exactly: one rounding keeps it, the
 * product rounded first loses it. */
static char *test_fma_rounds_once(void)
{
    const float a = 1.0f + 0x1p-23f;
    const float c = -(1.0f + 0x1p-22f);
    mu_assert("fma keeps the low product bits", VMAF_MTL_FMA(a, a, c) == 0x1p-46f);
    volatile float product = a * a;
    mu_assert("the separate product drops them", product + c == 0.0f);
    return NULL;
}

/* vmaf_mtl_f64_equal() answers as `==` does, for every class of input. */
static char *test_f64_equal_agrees_with_equals(void)
{
    mu_assert("a value equals itself", vmaf_mtl_f64_equal(0.1, 0.1));
    mu_assert("+0 equals -0", vmaf_mtl_f64_equal(0.0, -0.0));
    mu_assert("-0 equals +0", vmaf_mtl_f64_equal(-0.0, 0.0));
    mu_assert("an infinity equals itself", vmaf_mtl_f64_equal(INFINITY, INFINITY));
    return NULL;
}

static char *test_f64_equal_refuses_what_equals_refuses(void)
{
    mu_assert("+inf is not -inf", !vmaf_mtl_f64_equal(INFINITY, -INFINITY));
    mu_assert("a NaN equals nothing", !vmaf_mtl_f64_equal(NAN, NAN));
    mu_assert("a NaN is not a number", !vmaf_mtl_f64_equal(NAN, 1.0));
    mu_assert("neighbours differ", !vmaf_mtl_f64_equal(1.0, nextafter(1.0, 2.0)));
    mu_assert("neighbours differ the other way", !vmaf_mtl_f64_equal(nextafter(1.0, 2.0), 1.0));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_bit_casts_round_trip);
    mu_run_test(test_clz64);
    mu_run_test(test_fma_rounds_once);
    mu_run_test(test_f64_equal_agrees_with_equals);
    mu_run_test(test_f64_equal_refuses_what_equals_refuses);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
