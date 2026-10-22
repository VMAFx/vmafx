/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_psnr CPU vs. SYCL: the twin returns the CPU's score bit for bit
 * (ADR-1450, past 2^53 units ADR-1499; first added as a places=4 parity
 * test, ADR-0946).
 *
 * float_psnr is float_psnr.c on the CPU and
 * float_psnr_sycl.cpp::vmaf_fex_float_psnr_sycl on SYCL. The CPU forms each
 * squared difference in float and adds the terms in double, which is exact.
 * The twin added each 16x16 work-group's terms in fp32: exact at 8 bits, and
 * at 10, 12 and 16 bits only while a group's sum fits 24 bits in units of
 * 1 / scaler^2. On independent full-range noise it was 1.1e-8 dB off at 10
 * bits, 2.4e-8 at 12 and 7.4e-9 at 16, and 7.4e-8 on a bright 16-bit
 * 1920x1080 pair. The kernel forms the same terms as integers now, each
 * work-group adds a segment of one row, and the host adds each row's exact
 * sum in the CPU's row order.
 *
 * The fixtures, the comparison and the cases are float_psnr_twin_parity.h's.
 *
 * Skip behaviour: exits 77 when there is no SYCL device.
 */

#ifdef FIXTURE_W
/* The `_large` variant re-runs the noise cases at 960x540. The cases past
 * 2^53 have their own sizes and run in the default binary only. */
#define FLOAT_PSNR_TWIN_LARGE_VARIANT 1
#endif

#include "libvmaf/libvmaf_sycl.h"

#include "float_psnr_twin_parity.h"

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

static const FloatPsnrTwin twin = {
    .extractor = "float_psnr_sycl",
    .backend = "SYCL",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_psnr_sycl_registered(void)
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

static char *test_float_psnr_odd_depths_exact(void)
{
    return float_psnr_twin_odd_depths_exact(&twin);
}

static char *test_float_psnr_identical_8bit(void)
{
    return float_psnr_twin_identical_exact(&twin, 8u, 60.0);
}

static char *test_float_psnr_identical_16bit(void)
{
    return float_psnr_twin_identical_exact(&twin, 16u, 108.0);
}

#ifndef FLOAT_PSNR_TWIN_LARGE_VARIANT
static char *test_float_psnr_16bit_past_2_53_exact(void)
{
    return float_psnr_twin_past_2_53_exact(&twin);
}
#endif

static char *run_noise_cases(void)
{
    mu_run_test(test_float_psnr_8bit_exact);
    mu_run_test(test_float_psnr_10bit_exact);
    mu_run_test(test_float_psnr_12bit_exact);
    mu_run_test(test_float_psnr_16bit_exact);
    mu_run_test(test_float_psnr_10bit_uncapped_exact);
    mu_run_test(test_float_psnr_odd_depths_exact);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_psnr_sycl_registered);
    mu_assert_msg(run_noise_cases());
    mu_run_test(test_float_psnr_16bit_bright_exact);
    mu_run_test(test_float_psnr_identical_8bit);
    mu_run_test(test_float_psnr_identical_16bit);
#ifndef FLOAT_PSNR_TWIN_LARGE_VARIANT
    mu_run_test(test_float_psnr_16bit_past_2_53_exact);
#endif
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
