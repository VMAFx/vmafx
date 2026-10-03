/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_vif CPU vs. Metal: the twin must return the CPU's scores bit for bit
 * (T-GPU-FLOAT-VIF-CPU-ARITHMETIC-2026-10-01; the CUDA, SYCL and HIP twins are
 * exact, ADR-1412, ADR-1422, ADR-1444). First added as a places=4 test on one
 * 8-bit frame (ADR-0421).
 *
 * float_vif.c filters with the taps vif_get_filter() computes, evaluates the
 * statistic with log2f_approx() and vif_sigma_nsq in double, and adds each row
 * and then the rows in fp32. A twin with the removed decimal tap table, the
 * device log2 or per-block sums is off by up to 3.8e-5 on video. The fixtures,
 * the comparison and the cases are float_vif_twin_parity.h's, every output at
 * `==`. The macOS tester bundle runs this test and reports each case
 * (ADR-1496).
 *
 * Skip behaviour: exits 77 when there is no Metal device.
 */

#include "metal_twin.h"

#include "float_vif_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const VifTwin twin = {
    .extractor = METAL_TWIN("float_vif_metal", "float_vif"),
    .backend = METAL_TWIN_BACKEND,
    .open = metal_twin_open,
    .import = metal_twin_import,
    .close = metal_twin_close,
};

static char *test_float_vif_metal_registered(void)
{
    return vif_twin_registered(&twin);
}

static char *test_float_vif_default_identical(void)
{
    return vif_twin_default_identical(&twin);
}

static char *test_float_vif_debug_identical(void)
{
    return vif_twin_debug_identical(&twin);
}

static char *test_float_vif_model_options_identical(void)
{
    return vif_twin_model_options_identical(&twin);
}

static char *test_float_vif_skip_scale0_identical(void)
{
    return vif_twin_skip_scale0_identical(&twin);
}

static char *test_float_vif_scale_minimums_identical(void)
{
    return vif_twin_scale_minimums_identical(&twin);
}

static char *test_float_vif_10bit_identical(void)
{
    return vif_twin_10bit_identical(&twin);
}

static char *test_float_vif_small_odd_frame_identical(void)
{
    return vif_twin_small_odd_frame_identical(&twin);
}

char *run_tests(void)
{
    metal_run_case(test_float_vif_metal_registered);
    metal_run_case(test_float_vif_default_identical);
    metal_run_case(test_float_vif_debug_identical);
    metal_run_case(test_float_vif_model_options_identical);
    metal_run_case(test_float_vif_skip_scale0_identical);
    metal_run_case(test_float_vif_scale_minimums_identical);
    metal_run_case(test_float_vif_10bit_identical);
    metal_run_case(test_float_vif_small_odd_frame_identical);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
