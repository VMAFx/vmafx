/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ssim (integer_ssim.c) CPU vs. Metal: the twin must return the CPU's score
 * bit for bit (T-GPU-SSIM-FRAME-SUM-ORDER-2026-10-01; the CUDA, HIP and SYCL
 * twins are exact, ADR-1424, ADR-1438, ADR-1443). First added as a places=4
 * test on one 8-bit frame (ADR-0421).
 *
 * calc_ssim() adds every pixel's double term in raster order. A twin that
 * adds float partials per work-group is off in the last digits (2.3e-14 on
 * the Netflix pair, 1.1e-11 on the 10 px checkerboard on CUDA), and enable_db
 * magnifies a last-place difference. The identical-frame cases also measure
 * item 1 of T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30 for this twin: an
 * identical window is not forced to 1, the CPU's dB value is what it is. The
 * fixtures, the comparison and the cases are ssim_twin_parity.h's, all at
 * `==`. The macOS tester bundle runs this test and reports each case
 * (ADR-1496).
 *
 * Skip behaviour: exits 77 when there is no Metal device.
 */

#include "metal_twin.h"

#include "ssim_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const SsimTwin twin = {
    .extractor = METAL_TWIN("integer_ssim_metal", "ssim"),
    .backend = METAL_TWIN_BACKEND,
    .open = metal_twin_open,
    .import = metal_twin_import,
    .close = metal_twin_close,
};

static char *test_ssim_metal_registered(void)
{
    return ssim_twin_registered(&twin);
}

static char *test_ssim_8bit(void)
{
    return ssim_twin_bit_depth(&twin, 8u);
}

static char *test_ssim_10bit(void)
{
    return ssim_twin_bit_depth(&twin, 10u);
}

static char *test_ssim_12bit(void)
{
    return ssim_twin_bit_depth(&twin, 12u);
}

static char *test_ssim_16bit(void)
{
    return ssim_twin_bit_depth(&twin, 16u);
}

static char *test_ssim_odd_frame(void)
{
    return ssim_twin_odd_frame(&twin);
}

static char *test_ssim_tiny_frame(void)
{
    return ssim_twin_tiny_frame(&twin, 8u);
}

static char *test_ssim_tiny_frame_16bit(void)
{
    return ssim_twin_tiny_frame(&twin, 16u);
}

static char *test_ssim_one_pixel(void)
{
    return ssim_twin_one_pixel(&twin);
}

static char *test_ssim_1080p(void)
{
    return ssim_twin_1080p(&twin);
}

static char *test_ssim_enable_db(void)
{
    return ssim_twin_enable_db(&twin);
}

static char *test_ssim_inverted(void)
{
    return ssim_twin_inverted(&twin, 8u);
}

static char *test_ssim_inverted_16bit(void)
{
    return ssim_twin_inverted(&twin, 16u);
}

static char *test_ssim_identical(void)
{
    return ssim_twin_identical(&twin, 323u, 181u, NULL);
}

static char *test_ssim_identical_clipped(void)
{
    return ssim_twin_identical(&twin, 323u, 181u, "clip_db");
}

static char *test_ssim_identical_tiny(void)
{
    return ssim_twin_identical(&twin, 3u, 3u, NULL);
}

static void run_bit_depth_cases(void)
{
    metal_run_case(test_ssim_8bit);
    metal_run_case(test_ssim_10bit);
    metal_run_case(test_ssim_12bit);
    metal_run_case(test_ssim_16bit);
}

static void run_geometry_cases(void)
{
    metal_run_case(test_ssim_odd_frame);
    metal_run_case(test_ssim_tiny_frame);
    metal_run_case(test_ssim_tiny_frame_16bit);
    metal_run_case(test_ssim_one_pixel);
    metal_run_case(test_ssim_1080p);
}

static void run_db_cases(void)
{
    metal_run_case(test_ssim_enable_db);
    metal_run_case(test_ssim_inverted);
    metal_run_case(test_ssim_inverted_16bit);
    metal_run_case(test_ssim_identical);
    metal_run_case(test_ssim_identical_clipped);
    metal_run_case(test_ssim_identical_tiny);
}

char *run_tests(void)
{
    metal_run_case(test_ssim_metal_registered);
    run_bit_depth_cases();
    run_geometry_cases();
    run_db_cases();
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
