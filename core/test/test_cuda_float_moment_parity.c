/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_moment CPU vs. CUDA: the twin returns the CPU's four moments bit for
 * bit (ADR-1453; first added as a places=4 parity test, ADR-0947, and at 10
 * bits, ADR-1212).
 *
 * float_moment is float_moment.c / moment.c on the CPU and
 * cuda/integer_moment_cuda.c (registered as `float_moment_cuda`) on CUDA. The
 * twin adds four uint64 sums on the device and the host divides them. Its
 * second sums were the exact integer squares of the raw samples, which is the
 * CPU's sum up to 12 bits and not at 16, where the CPU's float square is the
 * integer square rounded to 24 bits: on full-range 16-bit noise the second
 * moments were 2.8e-5 off, on a bright 16-bit 1920x1080 frame 1.0e-4. The
 * 16bpc kernel adds the float squares now (moment_float_square()).
 *
 * The fixtures, the comparison and the cases are
 * float_moment_twin_parity.h's. On the old twin the 8-, 10- and 12-bit cases
 * pass and both 16-bit equality cases fail.
 *
 * Skip behaviour: exits 77 when there is no CUDA device.
 */

#ifdef FIXTURE_W
/* The `_large` variant re-runs the noise cases at 960x540. The cases past
 * 2^53 have their own sizes and run in the default binary only. */
#define FLOAT_MOMENT_TWIN_LARGE_VARIANT 1
#endif

#include "libvmaf/libvmaf_cuda.h"

#include "float_moment_twin_parity.h"
#include "mu_table.h"

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

static const FloatMomentTwin twin = {
    .extractor = "float_moment_cuda",
    .backend = "CUDA",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_moment_cuda_registered(void)
{
    return float_moment_twin_registered(&twin);
}

static char *test_float_moment_8bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 8u);
}

static char *test_float_moment_9bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 9u);
}

static char *test_float_moment_10bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 10u);
}

static char *test_float_moment_11bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 11u);
}

static char *test_float_moment_12bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 12u);
}

static char *test_float_moment_13bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 13u);
}

static char *test_float_moment_14bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 14u);
}

static char *test_float_moment_15bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 15u);
}

static char *test_float_moment_16bit_exact(void)
{
    return float_moment_twin_noise_exact(&twin, 16u);
}

static char *test_float_moment_16bit_bright_exact(void)
{
    return float_moment_twin_bright_1080p_exact(&twin);
}

#ifndef FLOAT_MOMENT_TWIN_LARGE_VARIANT
static char *test_float_moment_16bit_past_2_53_exact(void)
{
    return float_moment_twin_past_2_53_exact(&twin);
}
#endif

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_float_moment_cuda_registered),
        MU_TEST(test_float_moment_8bit_exact),
        MU_TEST(test_float_moment_9bit_exact),
        MU_TEST(test_float_moment_10bit_exact),
        MU_TEST(test_float_moment_11bit_exact),
        MU_TEST(test_float_moment_12bit_exact),
        MU_TEST(test_float_moment_13bit_exact),
        MU_TEST(test_float_moment_14bit_exact),
        MU_TEST(test_float_moment_15bit_exact),
        MU_TEST(test_float_moment_16bit_exact),
        MU_TEST(test_float_moment_16bit_bright_exact),
#ifndef FLOAT_MOMENT_TWIN_LARGE_VARIANT
        MU_TEST(test_float_moment_16bit_past_2_53_exact),
#endif
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
