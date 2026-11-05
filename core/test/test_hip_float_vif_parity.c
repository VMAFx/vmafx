/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_vif CPU vs. HIP: the twin returns the CPU's outputs bit for bit
 * (ADR-1444; first added as a places=4 parity test, ADR-1217).
 *
 * The CPU path is float_vif.c over vif_tools.c; the HIP path is
 * float_vif_hip.c. Since ADR-1444 the twin filters with vif_get_filter()'s
 * taps, evaluates vif_pixel_statistic_s() in the CPU's types
 * (feature/float_vif_gpu_common.h, the arithmetic of the CUDA twin) and adds
 * each row and then the rows in fp32, as vif_statistic_s() does. So this test
 * asserts equality, not a tolerance.
 *
 * Before ADR-1444 the default case differed from the CPU on every frame (the
 * kernel carried a table of Gaussian taps that vif_get_filter() does not
 * compute, called the device log2f, took vif_sigma_nsq as a float and reduced
 * per wave and per block), and the twin had no `vif_scale1..3_min_val`
 * options, so every case here fails on the old twin.
 *
 * The fixtures, the comparison and the cases are float_vif_twin_parity.h's.
 *
 * Skip behaviour: exits 77 when there is no HIP device, and when the kernels
 * are not built (enable_hipcc=false: the extractor returns -ENOSYS).
 */

#include <errno.h>

#include "libvmaf/libvmaf_hip.h"

#include "float_vif_twin_parity.h"
#include "mu_table.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

/* One frame of the smallest case through the twin. Returns the first error of
 * the run; -ENOSYS is the build without device kernels. */
static int probe_run(VmafHipState *hip_state)
{
    static const VifTwinCase probe = {.name = "probe", .w = 64u, .h = 64u, .bpc = 8u};
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafPicture ref;
    VmafPicture dist;
    int err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = vmaf_hip_import_state(vmaf, hip_state);
    if (!err)
        err = vmaf_use_feature(vmaf, "float_vif_hip", NULL);
    if (!err)
        err = vif_twin_fill_picture(&ref, &probe, 0u, false);
    if (!err) {
        err = vif_twin_fill_picture(&dist, &probe, 0u, true);
        if (err)
            (void)vmaf_picture_unref(&ref);
    }
    if (!err)
        err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (!err)
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0u);
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* A device state, or non-zero when the cases cannot run: no device, or a
 * build whose extractor has no kernels. Any other failure of the probe is
 * left for the case itself to report. */
static int twin_open(void **state)
{
    VmafHipState *hip_state = NULL;
    const VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err == 0 && hip_state != NULL && probe_run(hip_state) == -ENOSYS) {
        vmaf_hip_state_free(&hip_state);
        err = -ENOSYS;
    }
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

static const VifTwin twin = {
    .extractor = "float_vif_hip",
    .backend = "HIP",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_vif_hip_registered(void)
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
    mu_run_test(test_float_vif_hip_registered);
    mu_run_test(test_float_vif_default_identical);
    mu_run_test(test_float_vif_debug_identical);
    mu_assert_msg(run_option_tests());
    mu_assert_msg(run_bit_depth_tests());
    mu_run_test(test_float_vif_small_odd_frame_identical);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
