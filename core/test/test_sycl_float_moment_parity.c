/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_moment CPU vs. SYCL: the twin returns the CPU's four moments bit for
 * bit (ADR-1449; first added as a places=4 parity test, ADR-0957, and at 10
 * bits, ADR-1212).
 *
 * float_moment is float_moment.c / moment.c on the CPU and
 * integer_moment_sycl.cpp::vmaf_fex_float_moment_sycl on SYCL. The twin adds
 * four int64 sums on the device and the host divides them. Its second sums
 * were the exact integer squares of the raw samples, which is the CPU's sum
 * up to 12 bits and not at 16, where the CPU's float square is the integer
 * square rounded to 24 bits: on full-range 16-bit noise the second moments
 * were 2.7e-5 off, on a bright 16-bit 1920x1080 frame 1.0e-4. The kernel adds
 * the float squares now (moment_float_square()).
 *
 * The fixtures, the comparison and the cases are
 * float_moment_twin_parity.h's. On the old twin the 8-, 10- and 12-bit cases
 * pass and both 16-bit equality cases fail.
 *
 * Skip behaviour: exits 77 when there is no SYCL device.
 */

#ifdef FIXTURE_W
/* The `_large` variant re-runs the noise cases at 960x540. The cases past
 * 2^53 have their own sizes and run in the default binary only. */
#define FLOAT_MOMENT_TWIN_LARGE_VARIANT 1
#endif

#include "libvmaf/libvmaf_sycl.h"

#include "float_moment_twin_parity.h"
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

static const FloatMomentTwin twin = {
    .extractor = "float_moment_sycl",
    .backend = "SYCL",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_moment_sycl_registered(void)
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
        MU_TEST(test_float_moment_sycl_registered),
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
