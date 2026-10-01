/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * psnr_hvs CPU vs. HIP: the twin returns the CPU's scores bit for bit
 * (ADR-1397, ADR-1401; first added as a places=4 parity test, ADR-0883).
 *
 * CPU is in third_party/xiph/psnr_hvs.c (Xiph reference port); the HIP path
 * is integer_psnr_hvs_hip.c + integer_psnr_hvs/psnr_hvs_score.hip.
 *
 * The fixtures, the comparison and the cases are psnr_hvs_twin_parity.h's,
 * shared with the CUDA and SYCL twins: 8 to 12 bits, 4:2:0 / 4:2:2 / 4:4:4,
 * and 3840x2160, each compared exactly on psnr_hvs_y / psnr_hvs_cb /
 * psnr_hvs_cr and the combined psnr_hvs. The twin refuses 4:0:0 input, so
 * that layout is not compared.
 *
 * Skip behaviour: exits 77 when there is no HIP device or the twin is built
 * without its device kernels (enable_hipcc=false, -ENOSYS).
 */

#include "libvmaf/libvmaf_hip.h"

#include "psnr_hvs_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static int twin_open(void **state)
{
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
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

static const HvsTwin twin = {
    .extractor = "psnr_hvs_hip",
    .backend = "HIP",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
    .scores_yuv400 = 0,
};

static char *test_psnr_hvs_hip_registered(void)
{
    return hvs_twin_registered(&twin);
}

static char *test_psnr_hvs_cpu_hip_identical(void)
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

static char *test_psnr_hvs_2160p_identical(void)
{
    return hvs_twin_2160p_identical(&twin);
}

char *run_tests(void)
{
    mu_run_test(test_psnr_hvs_hip_registered);
    mu_run_test(test_psnr_hvs_cpu_hip_identical);
    mu_run_test(test_psnr_hvs_every_depth_identical);
    mu_run_test(test_psnr_hvs_every_layout_identical);
    mu_run_test(test_psnr_hvs_2160p_identical);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
