/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * motion_sycl and motion_v2_sycl compute motion_five_frame_window with the
 * CPU's bits (ADR-1491).
 *
 * The cases, the fixture and the comparison are
 * motion_five_frame_twin_parity.h: every output of every frame of `motion`
 * and `motion_v2` with the option, alone and with the other motion options,
 * for 11, 1, 2 and 3 frames, compared with == against the CPU extractor.
 * Before ADR-1491 the twins did not compute the option at all (`motion_sycl`
 * returned -ENOTSUP, `motion_v2_sycl` did not declare it), so every case
 * fails on the old twins.
 *
 * Skip behaviour: exits 77 when there is no SYCL device.
 */

#include <stdbool.h>
#include <stddef.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"

#include "motion_five_frame_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static int sycl_open(void **state)
{
    VmafSyclState *sycl_state = NULL;
    const VmafSyclConfiguration cfg = {.device_index = -1};
    const int err = vmaf_sycl_state_init(&sycl_state, cfg);
    *state = sycl_state;
    return err;
}

static int sycl_import(VmafContext *vmaf, void *state)
{
    return vmaf_sycl_import_state(vmaf, state);
}

static int sycl_close(void *state)
{
    VmafSyclState *sycl_state = state;
    vmaf_sycl_state_free(&sycl_state);
    return 0;
}

static const MftBackend backend = {
    .label = "sycl",
    .motion = "motion_sycl",
    .motion_v2 = "motion_v2_sycl",
    .open = sycl_open,
    .import = sycl_import,
    .close = sycl_close,
    /* ADR-2090: a frame's SAD is collected in the next read. */
    .lag_motion = 2u,
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
    mu_assert("a SYCL motion twin does not return the CPU's five-frame-window scores at 8 bits",
              failed_cases_at(8u) == 0u);
    return NULL;
}

static char *test_five_frame_window_10bit(void)
{
    mu_assert("a SYCL motion twin does not return the CPU's five-frame-window scores at 10 bits",
              failed_cases_at(10u) == 0u);
    return NULL;
}

/* motion_add_uv is motion_sycl's own option: the CPU `motion` has no chroma
 * mode, so the five-frame window with chroma has no reference to equal and
 * the twin refuses the pair at its first frame instead of inventing one. */
static char *test_window_with_chroma_is_refused(void)
{
    void *state = NULL;
    if (sycl_open(&state) != 0 || state == NULL) {
        mu_skipped = 1;
        return NULL;
    }
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafFeatureDictionary *opts = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = sycl_import(vmaf, state);
    if (!err)
        err = vmaf_feature_dictionary_set(&opts, "motion_five_frame_window", "true");
    if (!err)
        err = vmaf_feature_dictionary_set(&opts, "motion_add_uv", "true");
    if (!err)
        err = vmaf_use_feature(vmaf, "motion_sycl", opts);
    const int setup_err = err;
    const int frame_err = setup_err ? 0 : mft_feed_frame(vmaf, 8u, 0u);
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    (void)sycl_close(state);
    mu_assert("setting up motion_sycl with both options failed", setup_err == 0);
    mu_assert("motion_sycl accepted the five-frame window together with motion_add_uv",
              frame_err != 0);
    mu_assert("closing the context failed", close_err == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_five_frame_window_8bit);
    if (!mu_skipped) {
        mu_run_test(test_five_frame_window_10bit);
    }
    if (!mu_skipped) {
        mu_run_test(test_window_with_chroma_is_refused);
    }
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
