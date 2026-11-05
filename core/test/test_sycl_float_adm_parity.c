/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_adm CPU vs. SYCL: the twin returns the CPU's outputs bit for bit
 * (ADR-1434; first added as a places=4 parity test, ADR-0946).
 *
 * The CPU path is float_adm.c over adm.c and adm_tools.c; the SYCL path is
 * float_adm_sycl.cpp. Since ADR-1434 the kernels run the reference's
 * arithmetic operation for operation without an fp64 type on the device
 * (feature/sycl/sycl_float_adm_math.h), the rows are added in the reference's
 * order and the host concludes with the reference's own routines. So this
 * test asserts equality, not a tolerance.
 *
 * Before ADR-1434 the twin multiplied the enhancement gain in fp32, used fp32
 * 1/30 and 1/15 constants, associated the angle threshold differently, formed
 * the masking threshold in another order, reduced each row per sub-group and
 * floored the frame sums at 1e-2 where the reference floors them at 1e-10.
 * It also lacked `adm_skip_scale0`, `adm_skip_aim_scale` and the per-scale
 * CSF weight overrides, and accepted frames below 17x17, which the CPU
 * refuses. Every exact case below fails on that twin; the
 * isolated-sample case fails by a whole unit of adm2.
 *
 * The fixtures, the comparison and the cases are float_adm_twin_parity.h's.
 *
 * Skip behaviour: exits 77 when there is no SYCL device.
 */

#include "libvmaf/libvmaf_sycl.h"

#include "float_adm_twin_parity.h"
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

static const AdmTwin twin = {
    .extractor = "float_adm_sycl",
    .backend = "SYCL",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_adm_sycl_registered(void)
{
    return adm_twin_registered(&twin);
}

static char *test_float_adm_default_exact(void)
{
    return adm_twin_default_exact(&twin);
}

static char *test_float_adm_noise_exact(void)
{
    return adm_twin_noise_exact(&twin);
}

static char *test_float_adm_9bit_exact(void)
{
    return adm_twin_9bit_exact(&twin);
}

static char *test_float_adm_10bit_exact(void)
{
    return adm_twin_10bit_exact(&twin);
}

static char *test_float_adm_11bit_exact(void)
{
    return adm_twin_11bit_exact(&twin);
}

static char *test_float_adm_12bit_exact(void)
{
    return adm_twin_12bit_exact(&twin);
}

static char *test_float_adm_13bit_exact(void)
{
    return adm_twin_13bit_exact(&twin);
}

static char *test_float_adm_14bit_exact(void)
{
    return adm_twin_14bit_exact(&twin);
}

static char *test_float_adm_15bit_exact(void)
{
    return adm_twin_15bit_exact(&twin);
}

static char *test_float_adm_16bit_exact(void)
{
    return adm_twin_16bit_exact(&twin);
}

static char *test_float_adm_odd_frame_exact(void)
{
    return adm_twin_odd_frame_exact(&twin);
}

static char *test_float_adm_smallest_frame_exact(void)
{
    return adm_twin_smallest_frame_exact(&twin);
}

static char *test_float_adm_narrow_frame_exact(void)
{
    return adm_twin_narrow_frame_exact(&twin);
}

static char *test_float_adm_1080p_exact(void)
{
    return adm_twin_1080p_exact(&twin);
}

static char *test_float_adm_gain_limit_exact(void)
{
    return adm_twin_gain_limit_exact(&twin);
}

static char *test_float_adm_bypass_cm_exact(void)
{
    return adm_twin_bypass_cm_exact(&twin);
}

static char *test_float_adm_skip_aim_scale_exact(void)
{
    return adm_twin_skip_aim_scale_exact(&twin);
}

static char *test_float_adm_skip_scale0_exact(void)
{
    return adm_twin_skip_scale0_exact(&twin);
}

static char *test_float_adm_view_dist_exact(void)
{
    return adm_twin_view_dist_exact(&twin);
}

static char *test_float_adm_weight_overrides_exact(void)
{
    return adm_twin_weight_overrides_exact(&twin);
}

static char *test_float_adm_csf_scale_is_a_watson_mode_noop(void)
{
    return adm_twin_csf_scale_is_a_watson_mode_noop(&twin);
}

static char *test_float_adm_p_norm_one_exact(void)
{
    return adm_twin_p_norm_one_exact(&twin);
}

static char *test_float_adm_p_norm_reaches_kernel(void)
{
    return adm_twin_p_norm_reaches_kernel(&twin);
}

static char *test_float_adm_small_sums_are_not_floored(void)
{
    return adm_twin_small_sums_are_not_floored(&twin);
}

static char *run_exact_bit_depth_cases(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_float_adm_default_exact), MU_TEST(test_float_adm_noise_exact),
        MU_TEST(test_float_adm_9bit_exact),    MU_TEST(test_float_adm_10bit_exact),
        MU_TEST(test_float_adm_11bit_exact),   MU_TEST(test_float_adm_12bit_exact),
        MU_TEST(test_float_adm_13bit_exact),   MU_TEST(test_float_adm_14bit_exact),
        MU_TEST(test_float_adm_15bit_exact),   MU_TEST(test_float_adm_16bit_exact),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

static char *run_exact_geometry_cases(void)
{
    mu_run_test(test_float_adm_odd_frame_exact);
    mu_run_test(test_float_adm_smallest_frame_exact);
    mu_run_test(test_float_adm_narrow_frame_exact);
    mu_run_test(test_float_adm_1080p_exact);
    return NULL;
}

static char *run_exact_scale_option_cases(void)
{
    mu_run_test(test_float_adm_gain_limit_exact);
    mu_run_test(test_float_adm_bypass_cm_exact);
    mu_run_test(test_float_adm_skip_aim_scale_exact);
    mu_run_test(test_float_adm_skip_scale0_exact);
    return NULL;
}

static char *run_exact_weight_option_cases(void)
{
    mu_run_test(test_float_adm_view_dist_exact);
    mu_run_test(test_float_adm_weight_overrides_exact);
    mu_run_test(test_float_adm_csf_scale_is_a_watson_mode_noop);
    mu_run_test(test_float_adm_p_norm_one_exact);
    return NULL;
}

/* The CPU float_adm refuses frames below 17x17; the twin accepted them. */
static char *test_float_adm_sycl_rejects_frames_below_17(void)
{
    return adm_twin_rejects_frames_below_17(&twin);
}

static char *run_other_cases(void)
{
    mu_run_test(test_float_adm_p_norm_reaches_kernel);
    mu_run_test(test_float_adm_small_sums_are_not_floored);
    mu_run_test(test_float_adm_sycl_rejects_frames_below_17);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_adm_sycl_registered);
    mu_assert_msg(run_exact_bit_depth_cases());
    mu_assert_msg(run_exact_geometry_cases());
    mu_assert_msg(run_exact_scale_option_cases());
    mu_assert_msg(run_exact_weight_option_cases());
    mu_assert_msg(run_other_cases());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
