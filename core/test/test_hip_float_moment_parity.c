/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_moment CPU vs. HIP: the twin returns the CPU's four moments bit for
 * bit (ADR-1447, past 2^53 units ADR-1497; first added as a places=4 parity
 * test, ADR-1212).
 *
 * moment.c adds the samples, and their squares, into one double per output.
 * It forms each square in float. Up to 12 bits a square has at most 24
 * significant bits, so the float is the integer square; at 16 bits it is the
 * square rounded to 24 bits. `float_moment_hip` added the exact integer
 * squares, which is the CPU's sum up to 12 bits and not at 16: on full-range
 * 16-bit noise the second moments were 2.8e-5 off. It adds the float squares
 * now, and past 2^53 units it forms the CPU's sequentially rounded sum
 * (feature/float_moment_sum.h).
 *
 * The fixtures, the comparison and the cases are
 * float_moment_twin_parity.h's. On the twin of ADR-1447 every case passes
 * except the cases past 2^53 that the CPU rounds.
 *
 * Skip behaviour: without a HIP device, or on a build without the device
 * kernels (-ENOSYS), a case reports the skip and the run exits 77.
 */

#ifdef FIXTURE_W
/* The `_large` variant re-runs the noise cases at 960x540. The cases past
 * 2^53 have their own sizes and run in the default binary only. */
#define FLOAT_MOMENT_TWIN_LARGE_VARIANT 1
#endif

#include "libvmaf/libvmaf_hip.h"

#include "float_moment_twin_parity.h"
#include "mu_table.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static int twin_open(void **state)
{
    VmafHipState *hip_state = NULL;
    const VmafHipConfiguration hip_cfg = {.device_index = -1};
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

static const FloatMomentTwin twin = {
    .extractor = "float_moment_hip",
    .backend = "HIP",
    .open = twin_open,
    .import = twin_import,
    .close = twin_close,
};

static char *test_float_moment_hip_registered(void)
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
        MU_TEST(test_float_moment_hip_registered),
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
