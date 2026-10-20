/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_psnr CPU vs. HIP: the twin returns the CPU's score bit for bit
 * (ADR-1440, past 2^53 units ADR-1499; first added as a places=4 parity
 * test, ADR-0945).
 *
 * float_psnr.c forms each squared difference in float and adds the terms in
 * double, row by row. `float_psnr_hip` added each 16x16 block in fp32, which
 * rounded at 10, 12 and 16 bits once a block's rms difference reached 256
 * code values (ADR-1440: integer block sums), and then added the blocks of
 * the whole frame, which past 2^53 units is not the CPU's sum of its rows
 * (ADR-1499: one block per row segment, the rows' exact sums added in the
 * CPU's order).
 *
 * The fixtures, the comparison and the cases are float_psnr_twin_parity.h's,
 * shared with the CUDA and SYCL tests. On the twin of ADR-1440 every case
 * passes except the cases past 2^53 that the CPU rounds.
 *
 * Skip behaviour: without a HIP device, or on a build without the device
 * kernels (-ENOSYS), a case reports the skip and the run exits 77.
 */

#ifdef FIXTURE_W
/* The `_large` variant re-runs the noise cases at 960x540. The cases past
 * 2^53 have their own sizes and run in the default binary only. */
#define FLOAT_PSNR_TWIN_LARGE_VARIANT 1
#endif

#include "libvmaf/libvmaf_hip.h"

#include "float_psnr_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static int twin_open(void **state)
{
    VmafHipState *hip_state = NULL;
    const VmafHipConfiguration hip_cfg = {.device_index = -1};
    const int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    *state = hip_state;
    return err;
}

static int twin_import(VmafContext *vmaf, void *state)
{
    return vmaf_hip_import_state(vmaf, (VmafHipState *)state);
}

static int twin_close(void *state)
{
    VmafHipState *hip_state = (VmafHipState *)state;
    vmaf_hip_state_free(&hip_state);
    return 0;
}

static const FloatPsnrTwin twin = {
    .extractor = "float_psnr_hip",
    .backend = "HIP",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_psnr_hip_registered(void)
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
    mu_run_test(test_float_psnr_hip_registered);
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
