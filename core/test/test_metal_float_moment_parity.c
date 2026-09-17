/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Metal kernel coverage round 3 — float_moment CPU vs. Metal parity
 * (ADR-0214 cross-backend gate).
 *
 * `float_moment_metal` emits four per-frame moment scores:
 *   - float_moment_ref1st (E[ref])
 *   - float_moment_dis1st (E[dis])
 *   - float_moment_ref2nd (E[ref^2])
 *   - float_moment_dis2nd (E[dis^2])
 *
 * This test runs the CPU `float_moment` extractor against `float_moment_metal`
 * over a single-frame YUV420P fixture and asserts that all four output keys
 * match the CPU twin within places=4 (1e-4). The kernel reduces float32
 * sums over workgroup partials; the residual is dominated by partial-sum
 * order, well below the 1e-4 bound.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/src/feature/metal/float_moment_metal.mm
 *   - core/src/feature/float_moment.c
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
#define PARITY_TOL 1e-4

static const char *kMomentKeys[4] = {
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
};

static int fill_fixture(VmafPicture *pic, unsigned variant)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const int v = (int)((row + col) & 0xFFu);
            const int d = (variant != 0u) ? (((row * 9u + col) & 0x7) - 4) : 0;
            int clamped = v + d;
            if (clamped < 0)
                clamped = 0;
            if (clamped > 255)
                clamped = 255;
            y[row * pic->stride[0] + col] = (uint8_t)clamped;
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
static char *feed_fixture_pair(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_fixture(&ref, 0u);
    if (err)
        return "fill_fixture(ref) failed";
    err = fill_fixture(&dist, 1u);
    if (err)
        return "fill_fixture(dist) failed";
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (err)
        return "vmaf_read_pictures failed";
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err)
        return "vmaf_read_pictures(EOS) failed";
    return NULL;
}

static char *run_cpu_float_moment(double out_scores[4])
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_moment", NULL);
    mu_assert("CPU: vmaf_use_feature(float_moment) failed", !err);

    char *feed_err = feed_fixture_pair(vmaf);
    if (feed_err)
        return feed_err;

    for (unsigned i = 0; i < 4; i++) {
        err = vmaf_feature_score_at_index(vmaf, kMomentKeys[i], &out_scores[i], 0u);
        mu_assert("CPU: vmaf_feature_score_at_index(moment) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_float_moment(double out_scores[4], int *skipped)
{
    *skipped = 0;
    int err = 0;

    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        *skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("Metal: vmaf_init failed", !err);
    err = vmaf_metal_import_state(vmaf, mstate);
    mu_assert("Metal: vmaf_metal_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "float_moment_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(float_moment_metal) failed", !err);

    char *feed_err = feed_fixture_pair(vmaf);
    if (feed_err)
        return feed_err;

    for (unsigned i = 0; i < 4; i++) {
        err = vmaf_feature_score_at_index(vmaf, kMomentKeys[i], &out_scores[i], 0u);
        mu_assert("Metal: vmaf_feature_score_at_index(moment) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);
    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_float_moment_cpu_metal_parity(void)
{
    double cpu_scores[4] = {0.0, 0.0, 0.0, 0.0};
    double metal_scores[4] = {0.0, 0.0, 0.0, 0.0};
    int skipped = 0;

    char *msg = run_cpu_float_moment(cpu_scores);
    if (msg)
        return msg;
    msg = run_metal_float_moment(metal_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    for (unsigned i = 0; i < 4; i++) {
        const double delta = fabs(cpu_scores[i] - metal_scores[i]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr, "\n%s parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                          kMomentKeys[i], cpu_scores[i], metal_scores[i], delta, PARITY_TOL);
        }
        mu_assert("float_moment CPU vs. Metal exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_moment_cpu_metal_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
