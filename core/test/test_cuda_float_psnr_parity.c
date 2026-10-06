/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_psnr CPU vs. CUDA: the twin returns the CPU's score bit for bit
 * (ADR-1455, past 2^53 units ADR-1499; first added as a places=4 parity test,
 * ADR-0947).
 *
 * float_psnr is float_psnr.c on the CPU and cuda/float_psnr_cuda.c on CUDA.
 * The CPU squares each sample difference in float and adds the squares in
 * double, which is exact; the twin added each 16x16 block in fp32, which is
 * exact at 8 bits and rounds at 10, 12 and 16 bits once the differences in a
 * block are large. On full-range noise it was up to 1.2e-7 dB off. The kernel
 * adds the float squares as integers now, one uint64 per block, each block a
 * segment of one row whose exact sum the host adds in the CPU's row order.
 *
 * The fixtures, the comparison and the cases are float_psnr_twin_parity.h's.
 * On the old twin the 8-bit and identical-frame cases pass and the 10-, 12-
 * and 16-bit cases fail.
 *
 * Skip behaviour: exits 77 when there is no CUDA device.
 */

#ifdef FIXTURE_W
/* The `_large` variant re-runs the noise cases at 960x540. The cases past
 * 2^53 have their own sizes and run in the default binary only. */
#define FLOAT_PSNR_TWIN_LARGE_VARIANT 1
#endif

#include "libvmaf/libvmaf_cuda.h"

#include "float_psnr_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static int twin_open(void **state)
{
    VmafCudaState *cu_state = NULL;
    const VmafCudaConfiguration cuda_cfg = {0};
    const int err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    *state = cu_state;
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

static const FloatPsnrTwin twin = {
    .extractor = "float_psnr_cuda",
    .backend = "CUDA",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_psnr_cuda_registered(void)
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
    mu_run_test(test_float_psnr_cuda_registered);
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
