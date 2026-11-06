/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Bit-depth scaler for float GPU twins (ADR-2145, #2476).
 *
 * Match picture_copy() at every supported depth (8..16).
 * Positive: scaler(bpc) == 1 << (bpc - 8) for every bpc 9..16, and == 1 for 8.
 * Negative: bpc 8 is not scaled (scaler == 1.0, not scaled up).
 * Boundary: 16 bits gives 256.0 (1 << 8).
 */

#include <stdint.h>

#include "mu_table.h"
#include "float_bits.h"
#include "test.h"

#include "feature/float_moment_scaler.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

static char *test_float_gpu_scaler_odd_and_even_depths(void)
{
    mu_assert("bpc 8 scaler must be 1.0",
              vmaf_test_identical_f64(vmaf_float_moment_scaler(8u), 1.0));
    for (unsigned bpc = 9u; bpc <= 16u; ++bpc) {
        const double want = (double)(1u << (bpc - 8u));
        mu_assert("scaler(bpc) must equal 1 << (bpc - 8)",
                  vmaf_test_identical_f64(vmaf_float_moment_scaler(bpc), want));
    }
    return NULL;
}

static char *test_float_gpu_scaler_negative(void)
{
    /* bpc 8 is unscaled (1.0, not scaled up to >1.0). */
    mu_assert("bpc 8 is unscaled", vmaf_test_identical_f64(vmaf_float_moment_scaler(8u), 1.0));
    mu_assert("bpc 8 is not scaled up", vmaf_float_moment_scaler(8u) < 2.0);
    /* Depths <= 8 return 1.0 (unscaled). */
    mu_assert("bpc 0 is unscaled", vmaf_test_identical_f64(vmaf_float_moment_scaler(0u), 1.0));
    mu_assert("bpc 7 is unscaled", vmaf_test_identical_f64(vmaf_float_moment_scaler(7u), 1.0));
    return NULL;
}

static char *test_float_gpu_scaler_boundary_16(void)
{
    /* Boundary 16 bits: 1 << 8 = 256.0. */
    mu_assert("bpc 16 boundary scaler is 256.0",
              vmaf_test_identical_f64(vmaf_float_moment_scaler(16u), 256.0));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_float_gpu_scaler_odd_and_even_depths),
        MU_TEST(test_float_gpu_scaler_negative),
        MU_TEST(test_float_gpu_scaler_boundary_16),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
