/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * CHUG audit gap-fill — motion3 CPU vs. SYCL parity test (T3-15(c) / ADR-0219).
 *
 * The motion3 post-process is a host-side moving-average derived from
 * motion2, reproduced independently in integer_motion.c (CPU path) and
 * integer_motion_sycl.cpp (SYCL path). No cross-backend assertion existed
 * before this test; boundary-condition drift in the moving-average formula
 * would silently pollute CHUG-extracted motion3_mean/std columns.
 *
 * This test allocates a 256x144 YUV420P 8-bpc synthetic fixture,
 * feeds two frames through both extractors, and asserts that
 * VMAF_integer_feature_motion3_score at frame index 1 matches to within
 * 1e-4 (places=4, per ADR-0214 cross-backend gate).
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no SYCL device visible)
 * the test emits "[skip: no SYCL device]" and passes cleanly.
 * This mirrors the pattern used in test_sycl.c.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* Test fixture geometry — large enough for the 5-tap Gaussian, small enough
 * for a fast CI run. Motion3 requires ≥ 2 frames (index 0 and index 1). */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u

/* Tolerance matching ADR-0214 cross-backend gate (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

/* Fill a YUV420P 8-bpc picture with a deterministic pattern so that
 * frame 0 and frame 1 differ, causing a non-zero motion score. */
static int fill_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    /* Luma plane: simple ramp with frame-dependent offset so successive
     * frames differ. Chroma planes are uniform (motion is luma-only). */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 13u) & 0xFFu);
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

/* ------------------------------------------------------------------ */
/* CPU path — run the "motion" extractor for NUM_FRAMES frames.        */
/* Returns the motion3_score at frame index 1 via *out_score.         */
/* ------------------------------------------------------------------ */
static char *run_cpu_motion3(double *out_score)
{
    int err = 0;

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "motion", NULL);
    mu_assert("CPU: vmaf_use_feature(motion) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_fixture(&ref, i);
        mu_assert("CPU: fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i);
        mu_assert("CPU: fill_fixture(dist) failed", !err);

        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }

    /* Signal end-of-stream so flush() runs and emits motion3 at index 1. */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion3_score", out_score, 1u);
    mu_assert("CPU: vmaf_feature_score_at_index(motion3, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SYCL path — run the "motion_sycl" extractor for NUM_FRAMES frames. */
/* Returns the motion3_score at frame index 1 via *out_score.         */
/* Returns a skip sentinel (out_score = NaN) if no SYCL device.      */
/* ------------------------------------------------------------------ */
static char *run_sycl_motion3(double *out_score)
{
    *out_score = NAN;
    int err = 0;

    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        /* No SYCL GPU available — caller treats NaN as skip. */
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);

    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "motion_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(motion_sycl) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_fixture(&ref, i);
        mu_assert("SYCL: fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i);
        mu_assert("SYCL: fill_fixture(dist) failed", !err);

        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("SYCL: vmaf_read_pictures failed", !err);
    }

    /* Signal end-of-stream so flush() runs and emits motion3 at index 1. */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion3_score", out_score, 1u);
    mu_assert("SYCL: vmaf_feature_score_at_index(motion3, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);

    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Top-level parity assertion.                                         */
/* ------------------------------------------------------------------ */
static char *test_motion3_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;

    char *msg = run_cpu_motion3(&cpu_score);
    if (msg)
        return msg;

    msg = run_sycl_motion3(&sycl_score);
    if (msg)
        return msg;

    /* If no SYCL device was found, sycl_score is NaN — skip the assertion. */
    if (isnan(sycl_score))
        return NULL;

    double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nmotion3 parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("motion3 CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 1080p Checkerboard parity test (10-px blocks, motion_max_val=18.0)  */
/* ------------------------------------------------------------------ */
#define CHK_W 1920u
#define CHK_H 1080u
#define CHK_FRAMES 3u

static int fill_checkerboard_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, CHK_W, CHK_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        unsigned const row_blk = row / 10u;
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Shift left by frame_idx pixels so each successive frame moves by 1px */
            unsigned const col_blk = (col + 100u - frame_idx) / 10u;
            unsigned const phase = (row_blk + col_blk) & 1u;
            y[row * pic->stride[0] + col] = phase ? 235u : 16u;
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

/* NOLINTNEXTLINE(readability-function-size): test harness setup and per-frame loop */
static char *run_cpu_checkerboard(double *m2_f1, double *m2_f2, double *m3_f1, double *m3_f2)
{
    int err = 0;

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    VmafFeatureDictionary *opts = NULL;
    err = vmaf_feature_dictionary_set(&opts, "motion_max_val", "18.0");
    mu_assert("CPU: vmaf_feature_dictionary_set(motion_max_val) failed", !err);

    err = vmaf_use_feature(vmaf, "motion", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("CPU: vmaf_use_feature(motion) failed", !err);

    for (unsigned i = 0; i < CHK_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_checkerboard_fixture(&ref, i);
        mu_assert("CPU: fill_checkerboard_fixture(ref) failed", !err);
        err = fill_checkerboard_fixture(&dist, i);
        mu_assert("CPU: fill_checkerboard_fixture(dist) failed", !err);

        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "integer_motion2_mmxv_18", m2_f1, 1u);
    mu_assert("CPU: motion2 at idx=1 failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "integer_motion2_mmxv_18", m2_f2, 2u);
    mu_assert("CPU: motion2 at idx=2 failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "integer_motion3_mmxv_18", m3_f1, 1u);
    mu_assert("CPU: motion3 at idx=1 failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "integer_motion3_mmxv_18", m3_f2, 2u);
    mu_assert("CPU: motion3 at idx=2 failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

/* NOLINTNEXTLINE(readability-function-size): test harness setup and per-frame loop */
static char *run_sycl_checkerboard(double *m2_f1, double *m2_f2, double *m3_f1, double *m3_f2)
{
    *m2_f1 = NAN;
    *m2_f2 = NAN;
    *m3_f1 = NAN;
    *m3_f2 = NAN;
    int err = 0;

    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);

    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);

    VmafFeatureDictionary *opts = NULL;
    err = vmaf_feature_dictionary_set(&opts, "motion_max_val", "18.0");
    mu_assert("SYCL: vmaf_feature_dictionary_set(motion_max_val) failed", !err);

    err = vmaf_use_feature(vmaf, "motion_sycl", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("SYCL: vmaf_use_feature(motion_sycl) failed", !err);

    for (unsigned i = 0; i < CHK_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_checkerboard_fixture(&ref, i);
        mu_assert("SYCL: fill_checkerboard_fixture(ref) failed", !err);
        err = fill_checkerboard_fixture(&dist, i);
        mu_assert("SYCL: fill_checkerboard_fixture(dist) failed", !err);

        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("SYCL: vmaf_read_pictures failed", !err);
    }

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "integer_motion2_mmxv_18", m2_f1, 1u);
    mu_assert("SYCL: motion2 at idx=1 failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "integer_motion2_mmxv_18", m2_f2, 2u);
    mu_assert("SYCL: motion2 at idx=2 failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "integer_motion3_mmxv_18", m3_f1, 1u);
    mu_assert("SYCL: motion3 at idx=1 failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "integer_motion3_mmxv_18", m3_f2, 2u);
    mu_assert("SYCL: motion3 at idx=2 failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);

    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_motion_checkerboard_1080p_parity(void)
{
    double cpu_m2_f1 = 0.0;
    double cpu_m2_f2 = 0.0;
    double cpu_m3_f1 = 0.0;
    double cpu_m3_f2 = 0.0;

    double sycl_m2_f1 = NAN;
    double sycl_m2_f2 = NAN;
    double sycl_m3_f1 = NAN;
    double sycl_m3_f2 = NAN;

    char *msg = run_cpu_checkerboard(&cpu_m2_f1, &cpu_m2_f2, &cpu_m3_f1, &cpu_m3_f2);
    if (msg)
        return msg;

    msg = run_sycl_checkerboard(&sycl_m2_f1, &sycl_m2_f2, &sycl_m3_f1, &sycl_m3_f2);
    if (msg)
        return msg;

    if (isnan(sycl_m2_f1))
        return NULL;

    /* Verify motion_max_val=18.0 clipping on both CPU and SYCL */
    mu_assert("CPU motion2 frame 1 must be clipped to 18.0", fabs(cpu_m2_f1 - 18.0) < 1e-6);
    mu_assert("CPU motion2 frame 2 must be clipped to 18.0", fabs(cpu_m2_f2 - 18.0) < 1e-6);
    mu_assert("SYCL motion2 frame 1 must be clipped to 18.0", fabs(sycl_m2_f1 - 18.0) < 1e-6);
    mu_assert("SYCL motion2 frame 2 must be clipped to 18.0", fabs(sycl_m2_f2 - 18.0) < 1e-6);

    /* Verify exact parity between CPU and SYCL */
    mu_assert("motion2 frame 1 CPU vs SYCL parity", fabs(cpu_m2_f1 - sycl_m2_f1) < 1e-6);
    mu_assert("motion2 frame 2 CPU vs SYCL parity", fabs(cpu_m2_f2 - sycl_m2_f2) < 1e-6);
    mu_assert("motion3 frame 1 CPU vs SYCL parity", fabs(cpu_m3_f1 - sycl_m3_f1) <= PARITY_TOL);
    mu_assert("motion3 frame 2 CPU vs SYCL parity", fabs(cpu_m3_f2 - sycl_m3_f2) <= PARITY_TOL);

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion3_cpu_sycl_parity);
    mu_run_test(test_motion_checkerboard_1080p_parity);
    return NULL;
}
