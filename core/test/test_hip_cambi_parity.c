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
 * ADR-0945 round-3 — CAMBI CPU vs. HIP parity test.
 *
 * The Contrast-Aware Multiscale Banding Index is computed by cambi.c
 * (CPU) and by integer_cambi_hip.c + integer_cambi/cambi_score.hip
 * (HIP — Strategy II hybrid: three GPU kernels plus CPU residual via
 * cambi_internal.h).  Round-2 (PR #372) deferred this kernel pending
 * verification that the encoder-metadata path works with a synthetic
 * fixture; the deferral is now discharged.
 *
 * Fixture must clear the CAMBI_MIN_WIDTH_HEIGHT (216) floor on at
 * least one dimension — 320×240 satisfies both extractors without
 * setting an explicit `enc_width`/`enc_height` because both init()
 * paths fall back to source dimensions when those params are 0.
 *
 * Tolerance is places=3 (1e-3) — the CAMBI pooling tree accumulates
 * per-window rounding comparable to MS-SSIM (round-2 used the same
 * budget for ms_ssim).
 *
 * Skip behaviour: if vmaf_hip_state_init() fails OR the HIP path
 * returns -ENOSYS (scaffold posture under enable_hipcc=false) the
 * test emits a skip-tag and passes.  CAMBI keeps a working CPU
 * residual code path under `!HAVE_HIPCC` so the typical
 * vmaf-dev-mcp-with-AMD-GPU posture will execute end-to-end.
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

/* 320×240 clears CAMBI_MIN_WIDTH_HEIGHT (216) on both dims for cambi.c
 * and CAMBI_HIP_MIN_WIDTH_HEIGHT (216) for integer_cambi_hip.c. */
#ifndef FIXTURE_W
#define FIXTURE_W 320u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 240u
#endif
#define FIXTURE_BPC 8u

/* Filtered feature with multi-scale pooling — places=3 per ADR-0214. */
#define PARITY_TOL 1e-3

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    /* 8-step quantised gradient — classic banding artefact pattern. */
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (col / 32u) * 32u + salt * 4u;
            y[row * pic->stride[0] + col] = (uint8_t)(base & 0xFFu);
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
    int err = fill_pic(&ref, 0u);
    if (err)
        return err;
    err = fill_pic(&dist, 1u);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

static char *run_cpu_cambi(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "cambi", NULL);
    mu_assert("CPU: vmaf_use_feature(cambi) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "Cambi_feature_cambi_score", score, 0u);
    mu_assert("CPU: Cambi_feature_cambi_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_hip_cambi(double *score, int *skipped)
{
    *score = NAN;
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
    err = vmaf_use_feature(vmaf, "cambi_hip", NULL);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: vmaf_use_feature(cambi_hip) failed", !err);
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
    err = vmaf_feature_score_at_index(vmaf, "Cambi_feature_cambi_score", score, 0u);
    mu_assert("HIP: Cambi_feature_cambi_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_cambi_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("cambi_hip");
    mu_assert("cambi_hip extractor must be registered", fex != NULL);
    mu_assert("cambi_hip name matches", !strcmp(fex->name, "cambi_hip"));
    return NULL;
}

static char *test_cambi_cpu_hip_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;
    int skipped = 0;

    char *msg = run_cpu_cambi(&cpu);
    if (msg)
        return msg;
    msg = run_hip_cambi(&gpu, &skipped);
    if (msg)
        return msg;
    if (skipped || isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\ncambi parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n", cpu,
                      gpu, delta, PARITY_TOL);
    }
    mu_assert("cambi CPU vs. HIP delta exceeds places=3 tolerance (1e-3)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cambi_hip_registered);
    mu_run_test(test_cambi_cpu_hip_parity);
    return NULL;
}
