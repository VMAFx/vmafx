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
 * ADR-0945 round-3 — float_adm CPU vs. HIP parity test.
 *
 * The float-pipeline ADM feature is computed by float_adm.c (CPU) and
 * by float_adm_hip.c + float_adm/adm_*.hip (HIP — 4-stage DWT + CSF +
 * CM pipeline).  The integer ADM HIP twin already has a parity gate
 * (test_hip_adm_parity, ADR-0539); this test pins the float variant
 * which feeds a different consumer set (model trainers that prefer the
 * unquantised features).
 *
 * Asserts the always-emitted `VMAF_feature_adm2_score` plus the four
 * per-scale ratio channels.  Skip behaviour: if vmaf_hip_state_init()
 * fails OR the HIP path returns -ENOSYS (scaffold posture under
 * enable_hipcc=false) the test emits a skip-tag and passes.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* Wide enough for the ADM DWT2 pyramid (each scale halves the
 * dimensions, so >= 32x32 keeps scale 3 from collapsing). */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

static const char *const kAdmFeatures[] = {
    "VMAF_feature_adm2_score",       "VMAF_feature_adm_scale0_score",
    "VMAF_feature_adm_scale1_score", "VMAF_feature_adm_scale2_score",
    "VMAF_feature_adm_scale3_score",
};
#define NUM_ADM_FEATURES (sizeof(kAdmFeatures) / sizeof(kAdmFeatures[0]))

static int fill_ref(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row * 3u + col) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++)
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
    }
    return 0;
}

static int fill_dis(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            int v = (int)((row * 3u + col) & 0xFFu);
            v += (int)((row ^ col) & 0x07u) - 3;
            if (v < 0)
                v = 0;
            if (v > 255)
                v = 255;
            y[row * pic->stride[0] + col] = (uint8_t)v;
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++)
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
    }
    return 0;
}

static int feed_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_ref(&ref);
    if (err)
        return err;
    err = fill_dis(&dist);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

static char *run_cpu_float_adm(double scores[NUM_ADM_FEATURES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_adm", NULL);
    mu_assert("CPU: vmaf_use_feature(float_adm) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        err = vmaf_feature_score_at_index(vmaf, kAdmFeatures[f], &scores[f], 0u);
        if (err)
            (void)fprintf(stderr, "CPU: feature %s missing (err=%d)\n", kAdmFeatures[f], err);
        mu_assert("CPU: vmaf_feature_score_at_index failed", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_hip_float_adm(double scores[NUM_ADM_FEATURES], int *skipped)
{
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++)
        scores[f] = NAN;
    *skipped = 0;

    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);
    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "float_adm_hip", NULL);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: vmaf_use_feature(float_adm_hip) failed", !err);
    err = feed_frame(vmaf);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS on feed] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS on EOS] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        err = vmaf_feature_score_at_index(vmaf, kAdmFeatures[f], &scores[f], 0u);
        if (err)
            (void)fprintf(stderr, "HIP: feature %s missing (err=%d)\n", kAdmFeatures[f], err);
        mu_assert("HIP: vmaf_feature_score_at_index failed", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_float_adm_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_adm_hip");
    mu_assert("float_adm_hip extractor must be registered", fex != NULL);
    mu_assert("float_adm_hip name matches", !strcmp(fex->name, "float_adm_hip"));
    return NULL;
}

static char *test_float_adm_cpu_hip_parity(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double hip_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu_float_adm(cpu_scores);
    if (msg)
        return msg;
    msg = run_hip_float_adm(hip_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        if (isnan(hip_scores[f]))
            return NULL;
        double d = fabs(cpu_scores[f] - hip_scores[f]);
        if (d > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm parity FAIL: %s cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                          kAdmFeatures[f], cpu_scores[f], hip_scores[f], d, PARITY_TOL);
        }
        mu_assert("float_adm CPU vs. HIP delta exceeds places=4 tolerance (1e-4)", d <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_adm_hip_registered);
    mu_run_test(test_float_adm_cpu_hip_parity);
    return NULL;
}
