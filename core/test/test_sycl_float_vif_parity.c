/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_vif CPU vs. SYCL: the twin returns the CPU's outputs bit for bit
 * (ADR-1422; first added as a places=4 parity test, ADR-0946).
 *
 * The CPU path is float_vif.c over vif_tools.c; the SYCL path is
 * float_vif_sycl.cpp. Since ADR-1422 the twin filters with vif_get_filter()'s
 * taps, evaluates vif_pixel_statistic_s() in the CPU's types without an fp64
 * type on the device (feature/sycl/sycl_float_vif_math.h) and adds each row
 * and then the rows in fp32, as vif_statistic_s() does. So this test asserts
 * equality, not a tolerance.
 *
 * Before ADR-1422 the default case differed from the CPU on every frame (the
 * kernel carried a table of Gaussian taps that vif_get_filter() does not
 * compute, called the device log2 and reduced per sub-group), and the twin
 * had no `vif_scale1..3_min_val` options, so every case here fails on the old
 * twin.
 *
 * The fixtures, the comparison and the cases are float_vif_twin_parity.h's.
 *
 * Skip behaviour: exits 77 when there is no SYCL device.
 */

#include "libvmaf/libvmaf_sycl.h"

#include "float_vif_twin_parity.h"
#include "mu_table.h"

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

static const VifTwin twin = {
    .extractor = "float_vif_sycl",
    .backend = "SYCL",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_vif_sycl_registered(void)
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

static char *test_float_vif_9bit_identical(void)
{
    return vif_twin_9bit_identical(&twin);
}

static char *test_float_vif_10bit_identical(void)
{
    return vif_twin_10bit_identical(&twin);
}

static char *test_float_vif_11bit_identical(void)
{
    return vif_twin_11bit_identical(&twin);
}

static char *test_float_vif_12bit_identical(void)
{
    return vif_twin_12bit_identical(&twin);
}

static char *test_float_vif_13bit_identical(void)
{
    return vif_twin_13bit_identical(&twin);
}

static char *test_float_vif_14bit_identical(void)
{
    return vif_twin_14bit_identical(&twin);
}

static char *test_float_vif_15bit_identical(void)
{
    return vif_twin_15bit_identical(&twin);
}

static char *test_float_vif_16bit_identical(void)
{
    return vif_twin_16bit_identical(&twin);
}

static char *test_float_vif_small_odd_frame_identical(void)
{
    return vif_twin_small_odd_frame_identical(&twin);
}

static char *run_option_tests(void)
{
    mu_run_test(test_float_vif_model_options_identical);
    mu_run_test(test_float_vif_skip_scale0_identical);
    mu_run_test(test_float_vif_scale_minimums_identical);
    return NULL;
}

static char *run_bit_depth_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_float_vif_9bit_identical),  MU_TEST(test_float_vif_10bit_identical),
        MU_TEST(test_float_vif_11bit_identical), MU_TEST(test_float_vif_12bit_identical),
        MU_TEST(test_float_vif_13bit_identical), MU_TEST(test_float_vif_14bit_identical),
        MU_TEST(test_float_vif_15bit_identical), MU_TEST(test_float_vif_16bit_identical),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

char *run_tests(void)
{
    mu_run_test(test_float_vif_sycl_registered);
    mu_run_test(test_float_vif_default_identical);
    mu_run_test(test_float_vif_debug_identical);
    mu_assert_msg(run_option_tests());
    mu_assert_msg(run_bit_depth_tests());
    mu_run_test(test_float_vif_small_odd_frame_identical);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
