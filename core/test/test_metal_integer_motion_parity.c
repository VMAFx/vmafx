/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Metal kernel coverage round 3 — integer_motion CPU vs. Metal parity
 * (ADR-0421 first-kernel; ADR-0214 cross-backend gate).
 *
 * PR #351 added a registration audit covering all 8 Metal extractors; PR #379
 * added parity tests for motion_v2, integer_psnr, float_psnr, float_ssim.
 * This round-3 test fills one of the four remaining gaps by running the
 * scalar-integer `motion` (CPU) extractor against `integer_motion_metal` on
 * a two-frame YUV420P fixture and comparing
 *
 *     VMAF_integer_feature_motion2_score
 *
 * at frame index 0. The Metal kernel's reduction is over `uint32_t` SAD
 * partial sums (no float ordering noise), so the same places=4 (1e-4)
 * cross-backend gate from ADR-0214 applies — and in practice should
 * round-trip bit-exact.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/test/test_metal_motion_v2_parity.c (sibling, motion_v2 variant)
 *   - core/src/feature/metal/integer_motion_metal.mm
 *   - core/src/feature/integer_motion.c
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u
#define PARITY_TOL 1e-4

static int fill_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 11u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

/* Both sides feed the same fixture pair, so the sequence lives here once.
 * Extracting it also keeps each run_* function inside the branch budget the
 * lint profile sets, which is the refactor ADR-0141 asks for rather than a
 * suppression. */
static char *feed_fixture_pair(VmafContext *vmaf, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_fixture(&ref, frame);
    if (err)
        return "fill_fixture(ref) failed";
    err = fill_fixture(&dist, frame);
    if (err)
        return "fill_fixture(dist) failed";
    err = vmaf_read_pictures(vmaf, &ref, &dist, frame);
    if (err)
        return "vmaf_read_pictures failed";
    return NULL;
}

static char *run_cpu_integer_motion(double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "motion", NULL);
    mu_assert("CPU: vmaf_use_feature(motion) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        char *feed_err = feed_fixture_pair(vmaf, i);
        if (feed_err)
            return feed_err;
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion2_score", out_score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(motion2, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_integer_motion(double *out_score)
{
    *out_score = NAN;
    int err = 0;

    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("Metal: vmaf_init failed", !err);

    err = vmaf_metal_import_state(vmaf, mstate);
    mu_assert("Metal: vmaf_metal_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "integer_motion_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(integer_motion_metal) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        char *feed_err = feed_fixture_pair(vmaf, i);
        if (feed_err)
            return feed_err;
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion2_score", out_score, 0u);
    mu_assert("Metal: vmaf_feature_score_at_index(motion2, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);

    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_integer_motion_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;

    char *msg = run_cpu_integer_motion(&cpu_score);
    if (msg)
        return msg;
    msg = run_metal_integer_motion(&metal_score);
    if (msg)
        return msg;

    if (isnan(metal_score))
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\ninteger_motion parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("integer_motion CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_motion_cpu_metal_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
