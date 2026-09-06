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
#define FIXTURE_W 256u
#define FIXTURE_H 144u
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

/* ------------------------------------------------------------------ */
/* CPU reference path — float_motion extractor with motion_add_uv=true */
/* ------------------------------------------------------------------ */
/* NOLINTNEXTLINE(readability-function-size): test harness — setup +
 * per-frame loop + teardown; splitting would obscure the single linear
 * pipeline under test.  ADR-0141 §2 load-bearing test invariant. */
static char *run_cpu_float_motion_uv(double *out_score)
{
    int err = 0;

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    /* Pass motion_add_uv=true to float_motion. */
    VmafFeatureDictionary *opts = NULL;
    err = vmaf_feature_dictionary_set(&opts, "motion_add_uv", "true");
    mu_assert("CPU: vmaf_feature_dictionary_set(motion_add_uv) failed", !err);

    err = vmaf_use_feature(vmaf, "float_motion", opts);
    /* On success ownership transfers to vmaf — do not free. On failure free it. */
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("CPU: vmaf_use_feature(float_motion) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_yuv_fixture(&ref, i);
        mu_assert("CPU: fill_yuv_fixture(ref) failed", !err);
        err = fill_yuv_fixture(&dist, i);
        mu_assert("CPU: fill_yuv_fixture(dist) failed", !err);

        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: EOS failed", !err);

    /* float_motion with motion_add_uv=true stores scores under the aliased
     * name with the _mau suffix: motion2_mau.  The feature-name system
     * aliases VMAF_feature_motion2_score → motion2 (not float_motion2) and
     * then appends _mau for the non-default motion_add_uv bool option
     * (alias "mau") — see alias.c and feature_name.c:vmaf_feature_name_from_opts_dict. */
    err = vmaf_feature_score_at_index(vmaf, "motion2_mau", out_score, 1u);
    mu_assert("CPU: vmaf_feature_score_at_index(motion2_mau, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SYCL path — motion_sycl with motion_add_uv=true                    */
/* ------------------------------------------------------------------ */
/* NOLINTNEXTLINE(readability-function-size): test harness — two SYCL
 * context passes (UV-on, UV-off) must share one sycl_state lifetime;
 * splitting the passes into helpers would require passing the opaque
 * sycl_state pointer through the mu_assert return-by-pointer protocol,
 * obscuring the ownership model.  ADR-0141 §2 load-bearing test
 * invariant. */
static char *run_sycl_motion_uv(double *out_score, double *out_score_y_only)
{
    *out_score = NAN;
    *out_score_y_only = NAN;
    int err = 0;

    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }

    /* --- Pass 1: motion_add_uv=true --- */
    {
        VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
        VmafContext *vmaf = NULL;
        err = vmaf_init(&vmaf, cfg);
        mu_assert("SYCL+UV: vmaf_init failed", !err);

        err = vmaf_sycl_import_state(vmaf, sycl_state);
        mu_assert("SYCL+UV: vmaf_sycl_import_state failed", !err);

        VmafFeatureDictionary *opts = NULL;
        err = vmaf_feature_dictionary_set(&opts, "motion_add_uv", "true");
        mu_assert("SYCL+UV: vmaf_feature_dictionary_set failed", !err);

        err = vmaf_use_feature(vmaf, "motion_sycl", opts);
        /* On success ownership transfers to vmaf — do not free. On failure free it. */
        if (err)
            (void)vmaf_feature_dictionary_free(&opts);
        mu_assert("SYCL+UV: vmaf_use_feature failed", !err);

        for (unsigned i = 0; i < NUM_FRAMES; i++) {
            VmafPicture ref;
            VmafPicture dist;
            err = fill_yuv_fixture(&ref, i);
            mu_assert("SYCL+UV: fill_yuv_fixture(ref) failed", !err);
            err = fill_yuv_fixture(&dist, i);
            mu_assert("SYCL+UV: fill_yuv_fixture(dist) failed", !err);

            err = vmaf_read_pictures(vmaf, &ref, &dist, i);
            mu_assert("SYCL+UV: vmaf_read_pictures failed", !err);
        }

        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
        mu_assert("SYCL+UV: EOS failed", !err);

        /* motion_sycl with motion_add_uv=true stores scores under the aliased
         * name: integer_motion2_mau (VMAF_integer_feature_motion2_score aliased
         * to integer_motion2, with _mau appended for the non-default bool
         * option).  See feature_name.c:vmaf_feature_name_from_opts_dict. */
        err = vmaf_feature_score_at_index(vmaf, "integer_motion2_mau", out_score, 1u);
        mu_assert("SYCL+UV: vmaf_feature_score_at_index(integer_motion2_mau, idx=1) failed", !err);

        err = vmaf_close(vmaf);
        mu_assert("SYCL+UV: vmaf_close failed", !err);
    }

    /* --- Pass 2: motion_add_uv=false (Y-only baseline) --- */
    {
        VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
        VmafContext *vmaf2 = NULL;
        err = vmaf_init(&vmaf2, cfg);
        mu_assert("SYCL-Y: vmaf_init failed", !err);

        err = vmaf_sycl_import_state(vmaf2, sycl_state);
        mu_assert("SYCL-Y: vmaf_sycl_import_state failed", !err);

        err = vmaf_use_feature(vmaf2, "motion_sycl", NULL);
        mu_assert("SYCL-Y: vmaf_use_feature failed", !err);

        for (unsigned i = 0; i < NUM_FRAMES; i++) {
            VmafPicture ref;
            VmafPicture dist;
            err = fill_yuv_fixture(&ref, i);
            mu_assert("SYCL-Y: fill_yuv_fixture(ref) failed", !err);
            err = fill_yuv_fixture(&dist, i);
            mu_assert("SYCL-Y: fill_yuv_fixture(dist) failed", !err);

            err = vmaf_read_pictures(vmaf2, &ref, &dist, i);
            mu_assert("SYCL-Y: vmaf_read_pictures failed", !err);
        }

        err = vmaf_read_pictures(vmaf2, NULL, NULL, 0);
        mu_assert("SYCL-Y: EOS failed", !err);

        /* motion_sycl with default options (motion_add_uv=false) has no
         * non-default FEATURE_PARAM options, so the feature-name system uses
         * the raw name (no aliasing, no suffix). */
        err = vmaf_feature_score_at_index(vmaf2, "VMAF_integer_feature_motion2_score",
                                          out_score_y_only, 1u);
        mu_assert("SYCL-Y: vmaf_feature_score_at_index(motion2, idx=1) failed", !err);

        err = vmaf_close(vmaf2);
        mu_assert("SYCL-Y: vmaf_close failed", !err);
    }

    vmaf_sycl_state_free(&sycl_state);
    return NULL;
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
        return NULL; /* no SYCL device — skip */

    if (sycl_uv <= sycl_y) {
        (void)fprintf(stderr,
                      "\nmotion_add_uv FAIL: UV score (%.8f) <= Y-only score (%.8f); "
                      "UV contribution should be positive\n",
                      sycl_uv, sycl_y);
    }
    mu_assert("motion_add_uv score should exceed Y-only score (UV contribution must be > 0)",
              sycl_uv > sycl_y);
    return NULL;
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
        return NULL; /* no SYCL device — skip */

    double const delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(
            stderr,
            "\nmotion_add_uv parity FAIL: cpu(float_motion)=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
            cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("motion_add_uv: CPU float_motion vs. SYCL delta exceeds places=4 (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_add_uv_increases_score);
    mu_run_test(test_motion_add_uv_cpu_sycl_parity);
    return NULL;
}
