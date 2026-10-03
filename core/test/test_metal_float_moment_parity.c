/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_moment CPU vs. Metal: the twin must return the CPU's four moments bit
 * for bit (T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02; the CUDA, SYCL and HIP
 * twins are exact, ADR-1453, ADR-1449, ADR-1447). First added as a places=4
 * test at 8 and 10 bits (ADR-0421, ADR-1212), where the exact integer square
 * of a sample and moment.c's float square are the same number.
 *
 * At 16 bits the CPU's float square is the integer square rounded to 24 bits,
 * and a twin that adds exact integer squares is off on full-range content (the
 * CUDA twin was 2.8e-5 off on noise, 1.0e-4 on a bright 1080p frame). The
 * fixtures, the comparison and the cases are float_moment_twin_parity.h's,
 * all at `==` except the one past 2^53 units, held to the header's derived
 * bound. The macOS tester bundle runs this test and reports each case
 * (ADR-1496).
 *
 * Skip behaviour: exits 77 when there is no Metal device.
 */

#include "metal_twin.h"

#include "float_moment_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const FloatMomentTwin twin = {
    .extractor = METAL_TWIN("float_moment_metal", "float_moment"),
    .backend = METAL_TWIN_BACKEND,
    .open = metal_twin_open,
    .import = metal_twin_import,
    .close = metal_twin_close,
};

static char *test_float_moment_metal_registered(void)
{
    return float_moment_twin_registered(&twin);
}

static char *test_float_moment_8bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 8u);
}

static char *test_float_moment_10bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 10u);
}

static char *test_float_moment_12bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 12u);
}

static char *test_float_moment_16bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 16u);
}

static char *test_float_moment_16bit_bright_exact(void)
{
    return float_moment_twin_bright_1080p_exact(&twin);
}

static char *test_float_moment_16bit_past_2_53_exact(void)
{
    return float_moment_twin_past_2_53_exact(&twin);
}

char *run_tests(void)
{
    metal_run_case(test_float_moment_metal_registered);
    metal_run_case(test_float_moment_8bit_exact);
    metal_run_case(test_float_moment_10bit_exact);
    metal_run_case(test_float_moment_12bit_exact);
    metal_run_case(test_float_moment_16bit_exact);
    metal_run_case(test_float_moment_16bit_bright_exact);
    metal_run_case(test_float_moment_16bit_past_2_53_exact);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
