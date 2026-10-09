/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * motion_cuda and motion_v2_cuda compute motion_five_frame_window with the
 * CPU's bits (ADR-1491).
 *
 * The cases, the fixture and the comparison are
 * motion_five_frame_twin_parity.h: every output of every frame of `motion`
 * and `motion_v2` with the option, alone and with the other motion options,
 * for 11, 1, 2 and 3 frames, compared with == against the CPU extractor.
 * Before ADR-1491 the twins did not compute the option at all (`motion_cuda`
 * returned -ENOTSUP, `motion_v2_cuda` did not declare it), so every case
 * fails on the old twins.
 *
 * Skip behaviour: exits 77 when there is no CUDA device.
 */

#include <stdbool.h>
#include <stddef.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"

#include "motion_five_frame_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static int cuda_open(void **state)
{
    VmafCudaState *cu_state = NULL;
    const VmafCudaConfiguration cfg = {0};
    const int err = vmaf_cuda_state_init(&cu_state, cfg);
    *state = cu_state;
    return err;
}

static int cuda_import(VmafContext *vmaf, void *state)
{
    return vmaf_cuda_import_state(vmaf, state);
}

static int cuda_close(void *state)
{
    return vmaf_cuda_state_free(state);
}

static const MftBackend backend = {
    .label = "cuda",
    .motion = "motion_cuda",
    .motion_v2 = "motion_v2_cuda",
    .open = cuda_open,
    .import = cuda_import,
    .close = cuda_close,
    /* ADR-2090: `motion_cuda` reads its SADs back in batches of eight
     * (ADR-0845): frame 7 waits for the SAD of frame 8, read back by frame
     * 15's collect in read 16. motion_v2_cuda collects every frame one read
     * later. */
    .lag_motion = 9u,
    .lag_motion_v2 = 2u,
};

static unsigned failed_cases_at(unsigned bpc)
{
    bool skipped = false;
    const unsigned failed = mft_failed_cases(&backend, bpc, &skipped);
    if (skipped) {
        mu_skipped = 1;
    }
    return failed;
}

static char *test_five_frame_window_8bit(void)
{
    mu_assert("a CUDA motion twin does not return the CPU's five-frame-window scores at 8 bits",
              failed_cases_at(8u) == 0u);
    return NULL;
}

static char *test_five_frame_window_10bit(void)
{
    mu_assert("a CUDA motion twin does not return the CPU's five-frame-window scores at 10 bits",
              failed_cases_at(10u) == 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_five_frame_window_8bit);
    if (!mu_skipped) {
        mu_run_test(test_five_frame_window_10bit);
    }
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
