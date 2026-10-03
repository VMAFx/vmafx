/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ciede2000 CPU vs. Metal: the twin must run ciede.c's arithmetic and add the
 * per-pixel values in raster order (T-GPU-CIEDE-CPU-ARITHMETIC-2026-10-01; the
 * CUDA, SYCL and HIP twins do, ADR-1426, ADR-1436, ADR-1448). First added as a
 * places=4 test on one 8-bit frame (ADR-0421), which an fp32 evaluation of
 * another form of the formula passes; such a twin is 1e-5 off on video.
 *
 * The twins differ from the CPU only where a pixel's value rounds to the
 * neighbouring float (a math-library call, or a value an fp32 pair does not
 * decide), so the comparison is a bound: the parity gate's LIBM_TWINS bound
 * for ciede, 1e-9 (scripts/ci/cross_backend_calibration.py), tighter than the
 * shared header's 1e-8. The fixtures and the cases are ciede_twin_parity.h's.
 * The macOS tester bundle runs this test and reports each case (ADR-1496).
 *
 * Skip behaviour: exits 77 when there is no Metal device.
 */

/* LIBM_TWINS["ciede"] (ADR-1426): the bound every ciede twin is gated at. */
#define CIEDE_TWIN_TOL 1e-9

#include "metal_twin.h"

#include "ciede_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const CiedeTwin twin = {
    .extractor = METAL_TWIN("integer_ciede_metal", "ciede"),
    .backend = METAL_TWIN_BACKEND,
    .open = metal_twin_open,
    .import = metal_twin_import,
    .close = metal_twin_close,
};

static char *test_ciede_metal_registered(void)
{
    return ciede_twin_registered(&twin);
}

static char *test_ciede_8bit(void)
{
    return ciede_twin_bit_depth(&twin, 8u);
}

static char *test_ciede_10bit(void)
{
    return ciede_twin_bit_depth(&twin, 10u);
}

static char *test_ciede_12bit(void)
{
    return ciede_twin_bit_depth(&twin, 12u);
}

static char *test_ciede_16bit(void)
{
    return ciede_twin_bit_depth(&twin, 16u);
}

static char *test_ciede_odd_frame(void)
{
    return ciede_twin_odd_frame(&twin);
}

static char *test_ciede_odd_ceil_chroma(void)
{
    return ciede_twin_odd_ceil_chroma(&twin);
}

static char *test_ciede_422(void)
{
    return ciede_twin_422(&twin);
}

static char *test_ciede_422_10bit_odd(void)
{
    return ciede_twin_422_10bit_odd(&twin);
}

static char *test_ciede_444(void)
{
    return ciede_twin_444(&twin);
}

static char *test_ciede_1080p(void)
{
    return ciede_twin_1080p(&twin);
}

static void run_layout_cases(void)
{
    metal_run_case(test_ciede_odd_frame);
    metal_run_case(test_ciede_odd_ceil_chroma);
    metal_run_case(test_ciede_422);
    metal_run_case(test_ciede_422_10bit_odd);
    metal_run_case(test_ciede_444);
    metal_run_case(test_ciede_1080p);
}

char *run_tests(void)
{
    metal_run_case(test_ciede_metal_registered);
    metal_run_case(test_ciede_8bit);
    metal_run_case(test_ciede_10bit);
    metal_run_case(test_ciede_12bit);
    metal_run_case(test_ciede_16bit);
    run_layout_cases();
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
