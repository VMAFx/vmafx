/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * psnr_hvs CPU vs. Metal: held to the CPU's scores bit for bit, as the CUDA,
 * SYCL and HIP twins are (ADR-1397, ADR-1401). First added as a places=4 test
 * on one 8-bit frame (ADR-0421). No state row records a Metal defect here;
 * the twin has not run against `==` on a device before the macOS tester
 * bundle (ADR-1496), so this test is that measurement.
 *
 * calc_psnrhvs() adds the 64 terms of each 8x8 block into one running float in
 * the CPU's order; a twin that sums per block in another order differs in the
 * last place. The fixtures, the comparison and the cases are
 * psnr_hvs_twin_parity.h's.
 *
 * Skip behaviour: exits 77 when there is no Metal device.
 */

#include "metal_twin.h"

#include "psnr_hvs_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const HvsTwin twin = {
    .extractor = METAL_TWIN("integer_psnr_hvs_metal", "psnr_hvs"),
    .backend = METAL_TWIN_BACKEND,
    .open = metal_twin_open,
    .import = metal_twin_import,
    .close = metal_twin_close,
};

static char *test_psnr_hvs_metal_registered(void)
{
    return hvs_twin_registered(&twin);
}

static char *test_psnr_hvs_cpu_metal_identical(void)
{
    return hvs_twin_identical(&twin);
}

static char *test_psnr_hvs_every_depth_identical(void)
{
    return hvs_twin_every_depth_identical(&twin);
}

static char *test_psnr_hvs_every_layout_identical(void)
{
    return hvs_twin_every_layout_identical(&twin);
}

static char *test_psnr_hvs_luma_only_identical(void)
{
    return hvs_twin_luma_only_identical(&twin);
}

static char *test_psnr_hvs_2160p_identical(void)
{
    return hvs_twin_2160p_identical(&twin);
}

char *run_tests(void)
{
    metal_run_case(test_psnr_hvs_metal_registered);
    metal_run_case(test_psnr_hvs_cpu_metal_identical);
    metal_run_case(test_psnr_hvs_every_depth_identical);
    metal_run_case(test_psnr_hvs_every_layout_identical);
    metal_run_case(test_psnr_hvs_luma_only_identical);
    metal_run_case(test_psnr_hvs_2160p_identical);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
