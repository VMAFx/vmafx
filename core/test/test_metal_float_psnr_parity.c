/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_psnr CPU vs. Metal: the twin must return the CPU's score bit for bit
 * (T-METAL-FLOAT-PSNR-FP32-BLOCK-SUMS-2026-10-02; the CUDA, SYCL and HIP twins
 * are exact, ADR-1455, ADR-1450, ADR-1440). First added as a 1e-4 dB parity
 * test on one 8-bit frame (ADR-0421), which every reduction passes.
 *
 * float_psnr.c forms each squared difference in float and adds the squares in
 * double, which is exact. A twin that adds a threadgroup's squares in fp32 is
 * exact at 8 bits and rounds at 10, 12 and 16 bits once the differences in a
 * group are large: full-range noise and a bright 16-bit 1080p frame show it.
 * The fixtures, the comparison and the cases are float_psnr_twin_parity.h's,
 * all at `==` except the one past 2^53 units, where the CPU's own double sum
 * rounds and the twin is held to the derived bound. The macOS tester bundle
 * runs this test and reports each case (ADR-1496).
 *
 * Skip behaviour: exits 77 when there is no Metal device.
 */

#include "metal_twin.h"

#include "float_psnr_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const FloatPsnrTwin twin = {
    .extractor = METAL_TWIN("float_psnr_metal", "float_psnr"),
    .backend = METAL_TWIN_BACKEND,
    .open = metal_twin_open,
    .import = metal_twin_import,
    .close = metal_twin_close,
};

static char *test_float_psnr_metal_registered(void)
{
    return float_psnr_twin_registered(&twin);
}

static char *test_float_psnr_8bit_exact(void)
{
    return float_psnr_twin_noise_exact(&twin, 8u, NULL);
}

static char *test_float_psnr_10bit_exact(void)
{
    return float_psnr_twin_noise_exact(&twin, 10u, NULL);
}

static char *test_float_psnr_12bit_exact(void)
{
    return float_psnr_twin_noise_exact(&twin, 12u, NULL);
}

static char *test_float_psnr_16bit_exact(void)
{
    return float_psnr_twin_noise_exact(&twin, 16u, NULL);
}

static char *test_float_psnr_10bit_uncapped_exact(void)
{
    return float_psnr_twin_noise_exact(&twin, 10u, "uncapped");
}

static char *test_float_psnr_16bit_bright_exact(void)
{
    return float_psnr_twin_bright_1080p_exact(&twin);
}

static char *test_float_psnr_identical_8bit(void)
{
    return float_psnr_twin_identical_exact(&twin, 8u, 60.0);
}

static char *test_float_psnr_identical_16bit(void)
{
    return float_psnr_twin_identical_exact(&twin, 16u, 108.0);
}

static char *test_float_psnr_16bit_past_2_53_within_bound(void)
{
    return float_psnr_twin_past_2_53_within_bound(&twin);
}

char *run_tests(void)
{
    metal_run_case(test_float_psnr_metal_registered);
    metal_run_case(test_float_psnr_8bit_exact);
    metal_run_case(test_float_psnr_10bit_exact);
    metal_run_case(test_float_psnr_12bit_exact);
    metal_run_case(test_float_psnr_16bit_exact);
    metal_run_case(test_float_psnr_10bit_uncapped_exact);
    metal_run_case(test_float_psnr_16bit_bright_exact);
    metal_run_case(test_float_psnr_identical_8bit);
    metal_run_case(test_float_psnr_identical_16bit);
    metal_run_case(test_float_psnr_16bit_past_2_53_within_bound);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
