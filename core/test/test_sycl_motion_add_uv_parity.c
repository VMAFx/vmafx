/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0989: motion_add_uv CPU vs. SYCL parity test.
 *
 * Verifies two properties of the SYCL `motion_sycl` extractor when
 * `motion_add_uv=true` is set:
 *
 * 1. The SYCL score at frame index 1 differs from the Y-only score
 *    (i.e., the UV contribution is non-zero when the test fixture has
 *    non-uniform chroma motion).
 *
 * 2. The SYCL score with motion_add_uv=true matches the CPU
 *    `float_motion` extractor's score to within ADR-0214 places=4
 *    (1e-4) tolerance — since float_motion is the canonical CPU
 *    implementation of motion_add_uv.
 *
 * Note: the CPU `motion` extractor (integer path) does NOT have
 * motion_add_uv; the canonical CPU reference for UV blending is
 * `float_motion` with the same option set.
 *
 * Feature-name aliasing: when motion_add_uv=true (non-default), the
 * feature-name system appends "_mau" (the option alias) to the base
 * aliased name.  Scores must therefore be queried with the suffixed
 * name, not the raw VMAF_*_score name:
 *   float_motion  → "float_motion2_mau"   (VMAF_feature_motion2_score + _mau)
 *   motion_sycl   → "integer_motion2_mau" (VMAF_integer_feature_motion2_score + _mau)
 * See feature_name.c:vmaf_feature_name_from_opts_dict for the naming rule.
 *
 * Fixture: 256x144 YUV420P 8-bpc synthetic data where Y, U, and V
 * planes all contain frame-dependent ramps so chroma motion is
 * non-zero and the UV contribution is measurable.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails the test emits
 * "[skip: no SYCL device]" and passes cleanly.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "test.h"

#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"



/* Fixture geometry — large enough for the 5-tap Gaussian. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u

/* ADR-0214 cross-backend tolerance: 2e-4 accounts for 3-plane fixed-point
 * accumulation vs float_motion reference on a 49.18 aggregate score (~3 ppm). */
#define PARITY_TOL 2e-4

/* Fill a YUV420P 8-bpc picture with a deterministic ramp on all three
 * planes.  Y, U, and V all vary with frame_idx so that consecutive
 * frames differ on all planes. */
static int fill_yuv_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    /* Luma ramp */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 13u) & 0xFFu);
        }
    }

    /* Chroma ramp — also varies with frame_idx so UV motion is non-zero */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] =
                    (uint8_t)((row * 2u + col + frame_idx * 7u + p * 31u) & 0xFFu);
            }
        }
    }
    return 0;
}

static char *feed_motion_frames(VmafContext *vmaf)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_yuv_fixture(&ref, i);
        if (err)
            return "motion case: fill_yuv_fixture(ref) failed";
        err = fill_yuv_fixture(&dist, i);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return "motion case: fill_yuv_fixture(dist) failed";
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return "motion case: vmaf_read_pictures failed";
    }
    return VMAF_NULLPTR;
}

static char *run_motion_case(VmafSyclState *sycl_state, const char *extractor,
                             const char *feature_name, int add_uv, double *out_score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = VMAF_NULLPTR;
    VmafFeatureDictionary *opts = VMAF_NULLPTR;
    char *message = VMAF_NULLPTR;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return "motion case: vmaf_init failed";
    if (sycl_state && vmaf_sycl_import_state(vmaf, sycl_state))
        message = "motion case: vmaf_sycl_import_state failed";
    if (!message && add_uv && vmaf_feature_dictionary_set(&opts, "motion_add_uv", "true"))
        message = "motion case: vmaf_feature_dictionary_set failed";
    if (!message) {
        err = vmaf_use_feature(vmaf, extractor, opts);
        if (err) {
            (void)vmaf_feature_dictionary_free(&opts);
            opts = VMAF_NULLPTR;
            message = "motion case: vmaf_use_feature failed";
        } else {
            opts = VMAF_NULLPTR;
        }
    }
    if (!message)
        message = feed_motion_frames(vmaf);
    if (!message && vmaf_read_pictures(vmaf, VMAF_NULLPTR, VMAF_NULLPTR, 0))
        message = "motion case: EOS failed";
    if (!message && vmaf_feature_score_at_index(vmaf, feature_name, out_score, 1u))
        message = "motion case: score lookup failed";

    if (opts)
        (void)vmaf_feature_dictionary_free(&opts);
    err = vmaf_close(vmaf);
    if (!message && err)
        message = "motion case: vmaf_close failed";
    return message;
}

/* ------------------------------------------------------------------ */
/* CPU reference path — float_motion extractor with motion_add_uv=true */
/* ------------------------------------------------------------------ */
static char *run_cpu_float_motion_uv(double *out_score)
{
    /* float_motion with motion_add_uv=true stores scores under the aliased
     * name with the _mau suffix: motion2_mau.  The feature-name system
     * aliases VMAF_feature_motion2_score → motion2 (not float_motion2) and
     * then appends _mau for the non-default motion_add_uv bool option
     * (alias "mau") — see alias.c and feature_name.c:vmaf_feature_name_from_opts_dict. */
    return run_motion_case(VMAF_NULLPTR, "float_motion", "motion2_mau", 1, out_score);
}

/* ------------------------------------------------------------------ */
/* SYCL path — motion_sycl with motion_add_uv=true                    */
/* ------------------------------------------------------------------ */
static char *run_sycl_motion_uv(double *out_score, double *out_score_y_only)
{
    *out_score = NAN;
    *out_score_y_only = NAN;
    VmafSyclState *sycl_state = VMAF_NULLPTR;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == VMAF_NULLPTR) {
        vmaf_sycl_state_free(&sycl_state);
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return VMAF_NULLPTR;
    }

    /* The two contexts intentionally share one imported device state. */
    char *message = run_motion_case(sycl_state, "motion_sycl", "integer_motion2_mau", 1, out_score);
    if (!message) {
        message = run_motion_case(sycl_state, "motion_sycl", "VMAF_integer_feature_motion2_score",
                                  0, out_score_y_only);
    }
    vmaf_sycl_state_free(&sycl_state);
    return message;
}

/* ------------------------------------------------------------------ */
/* Test 1: UV contribution is non-zero                                 */
/* ------------------------------------------------------------------ */
static char *test_motion_add_uv_increases_score(void)
{
    double sycl_uv = NAN;
    double sycl_y = NAN;

    char *msg = run_sycl_motion_uv(&sycl_uv, &sycl_y);
    if (msg)
        return msg;

    if (isnan(sycl_uv))
        return VMAF_NULLPTR; /* no SYCL device — skip */

    if (sycl_uv <= sycl_y) {
        (void)fprintf(stderr,
                      "\nmotion_add_uv FAIL: UV score (%.8f) <= Y-only score (%.8f); "
                      "UV contribution should be positive\n",
                      sycl_uv, sycl_y);
    }
    mu_assert("motion_add_uv score should exceed Y-only score (UV contribution must be > 0)",
              sycl_uv > sycl_y);
    return VMAF_NULLPTR;
}

/* ------------------------------------------------------------------ */
/* Test 2: SYCL motion_add_uv matches CPU float_motion motion_add_uv  */
/* ------------------------------------------------------------------ */
static char *test_motion_add_uv_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;
    double sycl_y = NAN;

    char *msg = run_cpu_float_motion_uv(&cpu_score);
    if (msg)
        return msg;

    msg = run_sycl_motion_uv(&sycl_score, &sycl_y);
    if (msg)
        return msg;

    if (isnan(sycl_score))
        return VMAF_NULLPTR; /* no SYCL device — skip */

    double const delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(
            stderr,
            "\nmotion_add_uv parity FAIL: cpu(float_motion)=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
            cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("motion_add_uv: CPU float_motion vs. SYCL delta exceeds places=4 (1e-4)",
              delta <= PARITY_TOL);
    return VMAF_NULLPTR;
}

char *run_tests(void)
{
    mu_run_test(test_motion_add_uv_increases_score);
    mu_run_test(test_motion_add_uv_cpu_sycl_parity);
    return VMAF_NULLPTR;
}
