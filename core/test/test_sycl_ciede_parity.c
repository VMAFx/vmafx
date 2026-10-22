/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ciede2000 CPU vs. SYCL parity (first added as a places=4 test, ADR-0884;
 * the CPU's arithmetic since ADR-1436).
 *
 * The CIEDE2000 colour difference is ciede.c on the CPU and
 * integer_ciede_sycl.cpp on SYCL. Since ADR-1436 the kernel evaluates the
 * reference's statements with every fp64 value as an fp32 pair
 * (feature/sycl/sycl_ciede_math.h; a SYCL kernel has no fp64 type, ADR-0220)
 * and the host adds the per-pixel values in the reference's raster order.
 * What still differs is a pixel in a million that rounds to the neighbouring
 * float, so this test asserts a tolerance of 1e-8 where it asserted 1e-4.
 *
 * Before ADR-1436 the twin computed in fp32 with another form of the formula
 * and added per 16x16 block; it was 7e-8 to 3e-7 from the CPU on these
 * fixtures, so the cases below fail on it.
 *
 * The fixtures, the comparison and the cases are ciede_twin_parity.h's. They
 * include what the build used to compile this file four times for: the odd
 * 577x325 4:2:0 frame (ceil chroma width 289), 4:2:2 at 10 bits and 4:4:4.
 *
 * Skip behaviour: exits 77 when there is no SYCL device.
 */

#include "libvmaf/libvmaf_sycl.h"

#include "ciede_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static int twin_open(void **state)
{
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    const int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    *state = sycl_state;
    return err;
}

static int twin_import(VmafContext *vmaf, void *state)
{
    return vmaf_sycl_import_state(vmaf, (VmafSyclState *)state);
}

static int twin_close(void *state)
{
    VmafSyclState *sycl_state = (VmafSyclState *)state;
    vmaf_sycl_state_free(&sycl_state);
    return 0;
}

static const CiedeTwin twin = {
    .extractor = "ciede_sycl",
    .backend = "SYCL",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_ciede_sycl_registered(void)
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

static char *test_ciede_odd_depths(void)
{
    return ciede_twin_odd_depths(&twin);
}

static char *run_bit_depth_cases(void)
{
    mu_run_test(test_ciede_8bit);
    mu_run_test(test_ciede_10bit);
    mu_run_test(test_ciede_12bit);
    mu_run_test(test_ciede_16bit);
    mu_run_test(test_ciede_odd_depths);
    return NULL;
}

static char *run_layout_cases(void)
{
    mu_run_test(test_ciede_odd_frame);
    mu_run_test(test_ciede_odd_ceil_chroma);
    mu_run_test(test_ciede_422);
    mu_run_test(test_ciede_422_10bit_odd);
    return NULL;
}

static char *run_wide_cases(void)
{
    mu_run_test(test_ciede_444);
    mu_run_test(test_ciede_1080p);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ciede_sycl_registered);
    mu_assert_msg(run_bit_depth_cases());
    mu_assert_msg(run_layout_cases());
    mu_assert_msg(run_wide_cases());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
