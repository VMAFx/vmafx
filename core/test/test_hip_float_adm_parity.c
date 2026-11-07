/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_adm CPU vs. HIP parity (first added as a places=4 test, ADR-0945; bit
 * for bit since ADR-1458).
 *
 * float_adm_hip runs adm_tools.c's arithmetic operation for operation
 * (feature/float_adm_gpu_common.h, the header of the CUDA twin), adds each
 * row and then the rows in fp32 as the reference does, divides where the
 * reference divides (ADR-1442) and concludes with the reference's own
 * routines. So every output has the CPU's bits, the per-scale numerators and
 * denominators of `debug=true` included, and the cases assert equality.
 *
 * Before ADR-1458 the twin associated the angle test's products differently,
 * reduced each row in strided partial sums and a wave tree, added the rows in
 * double, kept a copy of the CSF weights with fp32 intermediates, used fp32
 * constants and an fp32 gain limit, and floored the frame sums at 1e-2: it
 * matched the CPU on 224 of 1246 measured values and was up to 1.3e-5 from
 * it. The cases below fail on it.
 *
 * The fixtures, the comparison and the cases are float_adm_twin_parity.h's,
 * the ones the CUDA twin's options cover. adm_p_norm other than 1 or 3 is
 * not exact (powf on both sides) and keeps that header's tolerance.
 *
 * Skip behaviour: exits 77 when there is no HIP device, and when the kernels
 * are not built (enable_hipcc=false: the extractor returns -ENOSYS). The
 * frame-size case needs no device and always runs.
 */

#include <errno.h>

#include "libvmaf/libvmaf_hip.h"

#include "float_adm_twin_parity.h"
#include "mu_table.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

/* One small frame through the twin. Returns the first error of the run;
 * -ENOSYS is the build without device kernels. */
static int probe_run(VmafHipState *hip_state)
{
    static const AdmTwinCase probe = {
        .what = "probe", .w = 64u, .h = 64u, .bpc = 8u, .content = ADM_TWIN_TEXTURE};
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafPicture ref;
    VmafPicture dist;
    int err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = vmaf_hip_import_state(vmaf, hip_state);
    if (!err)
        err = vmaf_use_feature(vmaf, "float_adm_hip", NULL);
    if (!err)
        err = adm_twin_fill_picture(&ref, &probe, false);
    if (!err) {
        err = adm_twin_fill_picture(&dist, &probe, true);
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

static const AdmTwin twin = {
    .extractor = "float_adm_hip",
    .backend = "HIP",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_adm_hip_registered(void)
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

static char *test_float_adm_view_dist_exact(void)
{
    return adm_twin_view_dist_exact(&twin);
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

/* The CPU float_adm refuses frames below 17x17; the twin accepted them. */
static char *test_float_adm_hip_rejects_frames_below_17(void)
{
    return adm_twin_rejects_frames_below_17(&twin);
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

static char *run_exact_option_cases(void)
{
    mu_run_test(test_float_adm_gain_limit_exact);
    mu_run_test(test_float_adm_bypass_cm_exact);
    mu_run_test(test_float_adm_skip_aim_scale_exact);
    mu_run_test(test_float_adm_view_dist_exact);
    mu_run_test(test_float_adm_csf_scale_is_a_watson_mode_noop);
    mu_run_test(test_float_adm_p_norm_one_exact);
    return NULL;
}

static char *run_other_cases(void)
{
    mu_run_test(test_float_adm_p_norm_reaches_kernel);
    mu_run_test(test_float_adm_small_sums_are_not_floored);
    mu_run_test(test_float_adm_hip_rejects_frames_below_17);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_adm_hip_registered);
    mu_assert_msg(run_exact_bit_depth_cases());
    mu_assert_msg(run_exact_geometry_cases());
    mu_assert_msg(run_exact_option_cases());
    mu_assert_msg(run_other_cases());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
