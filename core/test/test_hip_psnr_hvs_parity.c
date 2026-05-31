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
 * ADR-0883 round-2 — psnr_hvs CPU vs. HIP parity test.
 *
 * The PSNR-HVS (PSNR with Human Visual System weighting) metric is
 * computed by third_party/xiph/psnr_hvs.c (CPU) and by
 * integer_psnr_hvs_hip.c + integer_psnr_hvs/psnr_hvs_score.hip on AMD.
 * PR #351 closed the equivalent CUDA/SYCL coverage gap for the
 * plain PSNR kernel; psnr_hvs has its own DCT-based contrast-masking
 * path that needed its own gate.
 *
 * Asserts the chroma-summary `psnr_hvs` channel — the single user-facing
 * score consumed by the libvmaf-2.x.x default model and CHUG re-extract.
 *
 * Skip behaviour: if vmaf_hip_state_init() fails (no HIP runtime or
 * no device visible) the test emits "[skip: no HIP device]" and passes.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row * 3u + col * 2u + salt * 13u) & 0xFFu);
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

static char *run_cpu_psnr_hvs(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "psnr_hvs", NULL);
    mu_assert("CPU: vmaf_use_feature(psnr_hvs) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_hvs", score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(psnr_hvs) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_hip_psnr_hvs(double *score)
{
    *score = NAN;
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);
    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "psnr_hvs_hip", NULL);
    mu_assert("HIP: vmaf_use_feature(psnr_hvs_hip) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("HIP: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_hvs", score, 0u);
    mu_assert("HIP: vmaf_feature_score_at_index(psnr_hvs) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_psnr_hvs_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs_hip");
    mu_assert("psnr_hvs_hip extractor must be registered", fex != NULL);
    mu_assert("psnr_hvs_hip name matches", !strcmp(fex->name, "psnr_hvs_hip"));
    return NULL;
}

static char *test_psnr_hvs_cpu_hip_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;
    char *msg = run_cpu_psnr_hvs(&cpu);
    if (msg)
        return msg;
    msg = run_hip_psnr_hvs(&gpu);
    if (msg)
        return msg;
    if (isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\npsnr_hvs parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("psnr_hvs CPU vs. HIP delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_psnr_hvs_hip_registered);
    mu_run_test(test_psnr_hvs_cpu_hip_parity);
    return NULL;
}
