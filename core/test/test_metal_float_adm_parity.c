/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_adm CPU vs. Metal: the twin must return the CPU's scores bit for bit
 * and refuse what the CPU refuses. First added as a places=4 test on one
 * 8-bit frame (ADR-0421). Three state rows meet here:
 *
 *  - T-GPU-FLOAT-ADM-CPU-ARITHMETIC-2026-10-01: the CUDA, SYCL and HIP twins
 *    are exact (ADR-1420, ADR-1434, ADR-1458); every `_exact` case below.
 *  - T-GPU-FLOAT-ADM-FRAME-SUM-FLOOR-2026-10-01: with adm_noise_weight = 0 a
 *    flat 16-bit frame leaves sums below the twin's old floor
 *    (test_float_adm_small_sums_are_not_floored).
 *  - T-GPU-FLOAT-ADM-TINY-FRAME-FLOOR-2026-10-01: below 17x17 the CPU's
 *    init() returns -EINVAL (test_float_adm_metal_rejects_frames_below_17).
 *
 * The fixtures, the comparison and the cases are float_adm_twin_parity.h's.
 * adm_p_norm = 2 raises the terms with powf() on both sides and keeps the
 * header's tolerance for that case only. The macOS tester bundle runs this
 * test and reports each case (ADR-1496).
 *
 * Skip behaviour: exits 77 when there is no Metal device; the frame-size case
 * needs none but skips too, so the hosted macOS runner measures nothing.
 */

#include "metal_twin.h"

#include "float_adm_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const AdmTwin twin = {
    .extractor = METAL_TWIN("float_adm_metal", "float_adm"),
    .backend = METAL_TWIN_BACKEND,
    .open = metal_twin_open,
    .import = metal_twin_import,
    .close = metal_twin_close,
};

static char *test_float_adm_metal_registered(void)
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

static char *test_float_adm_10bit_exact(void)
{
    return adm_twin_10bit_exact(&twin);
}

static char *test_float_adm_12bit_exact(void)
{
    return adm_twin_12bit_exact(&twin);
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

/* The CPU float_adm refuses frames below 17x17 with -EINVAL from init(); the
 * Metal twin accepted them. init() is called without a device state. */
static char *test_float_adm_metal_rejects_frames_below_17(void)
{
    if (!metal_twin_have_device()) {
        return NULL;
    }
    return adm_twin_rejects_frames_below_17(&twin);
}

static void run_bit_depth_cases(void)
{
    metal_run_case(test_float_adm_default_exact);
    metal_run_case(test_float_adm_noise_exact);
    metal_run_case(test_float_adm_10bit_exact);
    metal_run_case(test_float_adm_12bit_exact);
    metal_run_case(test_float_adm_16bit_exact);
}

static void run_geometry_cases(void)
{
    metal_run_case(test_float_adm_odd_frame_exact);
    metal_run_case(test_float_adm_smallest_frame_exact);
    metal_run_case(test_float_adm_narrow_frame_exact);
    metal_run_case(test_float_adm_1080p_exact);
}

static void run_option_cases(void)
{
    metal_run_case(test_float_adm_gain_limit_exact);
    metal_run_case(test_float_adm_bypass_cm_exact);
    metal_run_case(test_float_adm_skip_aim_scale_exact);
    metal_run_case(test_float_adm_view_dist_exact);
    metal_run_case(test_float_adm_csf_scale_is_a_watson_mode_noop);
    metal_run_case(test_float_adm_p_norm_one_exact);
    metal_run_case(test_float_adm_p_norm_reaches_kernel);
}

char *run_tests(void)
{
    metal_run_case(test_float_adm_metal_registered);
    run_bit_depth_cases();
    run_geometry_cases();
    run_option_cases();
    metal_run_case(test_float_adm_small_sums_are_not_floored);
    metal_run_case(test_float_adm_metal_rejects_frames_below_17);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
