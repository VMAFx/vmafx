/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * psnr_hvs CPU vs. SYCL: the twin returns the CPU's scores bit for bit
 * (ADR-1397, ADR-1401; first added as a places=4 parity test, ADR-0946).
 *
 * CPU is in third_party/xiph/psnr_hvs.c (Xiph reference port); the SYCL path
 * is integer_psnr_hvs_sycl.cpp. Its kernel has no fp64 (ADR-0220): the
 * masking threshold the CPU takes as a double product and root comes from
 * sqrt_prod_rn() (feature/sycl/sycl_exact_fp.h), and a threshold that is one
 * fp32 step off moves a few frames of real content by 5e-7 dB, which these
 * exact comparisons catch.
 *
 * The fixtures, the comparison and the cases are psnr_hvs_twin_parity.h's,
 * shared with the CUDA and HIP twins: 8 to 12 bits, 4:2:0 / 4:2:2 / 4:4:4,
 * and 3840x2160, each compared exactly on psnr_hvs_y / psnr_hvs_cb /
 * psnr_hvs_cr and the combined psnr_hvs. The twin refuses 4:0:0 input, so
 * that layout is not compared.
 *
 * Skip behaviour: exits 77 when there is no SYCL device.
 */

#include "libvmaf/libvmaf_sycl.h"

#include "psnr_hvs_twin_parity.h"

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

static const HvsTwin twin = {
    .extractor = "psnr_hvs_sycl",
    .backend = "SYCL",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
    .scores_yuv400 = 0,
};

static char *test_psnr_hvs_sycl_registered(void)
{
    return hvs_twin_registered(&twin);
}

static char *test_psnr_hvs_cpu_sycl_identical(void)
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
    mu_run_test(test_psnr_hvs_sycl_registered);
    mu_run_test(test_psnr_hvs_cpu_sycl_identical);
    mu_run_test(test_psnr_hvs_every_depth_identical);
    mu_run_test(test_psnr_hvs_every_layout_identical);
    mu_run_test(test_psnr_hvs_2160p_identical);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
