/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * psnr_hvs CPU vs. CUDA: the twin returns the CPU's scores bit for bit
 * (ADR-1397).
 *
 * PSNR-HVS is a perceptual peak-signal-to-noise variant that applies the
 * HVS contrast-sensitivity weighting before the MSE reduction. CPU is in
 * third_party/xiph/psnr_hvs.c (Xiph reference port); CUDA path is in
 * integer_psnr_hvs_cuda.c + integer_psnr_hvs/psnr_hvs_score.cu.
 *
 * The fixtures, the comparison and the cases are psnr_hvs_twin_parity.h's,
 * shared with the SYCL and HIP twins: 8 to 12 bits, 4:0:0 / 4:2:0 / 4:2:2 /
 * 4:4:4, enable_chroma=false, and 3840x2160, each compared exactly on
 * psnr_hvs_y / psnr_hvs_cb / psnr_hvs_cr and the combined psnr_hvs.
 *
 * Skip behaviour: exits 77 when there is no CUDA device.
 */

#include "libvmaf/libvmaf_cuda.h"

#include "psnr_hvs_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static int twin_open(void **state)
{
    VmafCudaState *cuda_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    const int err = vmaf_cuda_state_init(&cuda_state, cuda_cfg);
    *state = cuda_state;
    return err;
}

static int twin_import(VmafContext *vmaf, void *state)
{
    return vmaf_cuda_import_state(vmaf, (VmafCudaState *)state);
}

static int twin_close(void *state)
{
    return vmaf_cuda_state_free((VmafCudaState *)state);
}

static const HvsTwin twin = {
    .extractor = "psnr_hvs_cuda",
    .backend = "CUDA",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_psnr_hvs_cuda_registered(void)
{
    return hvs_twin_registered(&twin);
}

static char *test_psnr_hvs_cpu_cuda_identical(void)
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

static char *test_psnr_hvs_luma_only_identical(void)
{
    return hvs_twin_luma_only_identical(&twin);
}

static char *test_psnr_hvs_2160p_identical(void)
{
    return hvs_twin_2160p_identical(&twin);
}

char *run_tests(void)
{
    mu_run_test(test_psnr_hvs_cuda_registered);
    mu_run_test(test_psnr_hvs_cpu_cuda_identical);
    mu_run_test(test_psnr_hvs_every_depth_identical);
    mu_run_test(test_psnr_hvs_every_layout_identical);
    mu_run_test(test_psnr_hvs_luma_only_identical);
    mu_run_test(test_psnr_hvs_2160p_identical);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
