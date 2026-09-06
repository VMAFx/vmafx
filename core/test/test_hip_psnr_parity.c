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
 * GPU-kernel coverage gap-fill — psnr CPU vs. HIP parity test.
 *
 * The 8-bit integer PSNR sum-of-squared-error is computed by
 * integer_psnr.c (CPU) and by integer_psnr_hip.c + integer_psnr/psnr_score.hip
 * (HIP/ROCm). Before this test, only motion3 and ADM had CPU-vs-HIP
 * cross-backend gates; a regression in the per-plane SSE reduction
 * on AMD GPUs would have escaped CI undetected.
 *
 * Skip behaviour: if vmaf_hip_state_init() fails (no HIP runtime or
 * no device visible) the test emits "[skip: no HIP device]" and passes,
 * mirroring test_hip_motion3_parity.c / test_hip_adm_parity.c.
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

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
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
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + salt * 13u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] = (uint8_t)((row * 3u + col + salt * 9u) & 0xFFu);
            }
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

static char *run_cpu_psnr(double *y, double *cb, double *cr)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "psnr", NULL);
    mu_assert("CPU: vmaf_use_feature(psnr) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_y", y, 0u);
    mu_assert("CPU: psnr_y missing", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cb", cb, 0u);
    mu_assert("CPU: psnr_cb missing", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cr", cr, 0u);
    mu_assert("CPU: psnr_cr missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_hip_psnr(double *y, double *cb, double *cr)
{
    *y = NAN;
    *cb = NAN;
    *cr = NAN;
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
    err = vmaf_use_feature(vmaf, "psnr_hip", NULL);
    mu_assert("HIP: vmaf_use_feature(psnr_hip) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("HIP: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_y", y, 0u);
    mu_assert("HIP: psnr_y missing", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cb", cb, 0u);
    mu_assert("HIP: psnr_cb missing", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cr", cr, 0u);
    mu_assert("HIP: psnr_cr missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *check_pair(const char *label, double cpu, double gpu)
{
    if (isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\n%s parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n", label,
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("psnr CPU vs. HIP delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

static char *test_psnr_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hip");
    mu_assert("psnr_hip extractor must be registered", fex != NULL);
    mu_assert("psnr_hip name matches", !strcmp(fex->name, "psnr_hip"));
    return NULL;
}

static char *test_psnr_cpu_hip_parity(void)
{
    double cy = 0.0;
    double cb = 0.0;
    double cr = 0.0;
    double hy = NAN;
    double hb = NAN;
    double hr = NAN;
    char *msg = run_cpu_psnr(&cy, &cb, &cr);
    if (msg)
        return msg;
    msg = run_hip_psnr(&hy, &hb, &hr);
    if (msg)
        return msg;
    msg = check_pair("psnr_y", cy, hy);
    if (msg)
        return msg;
    msg = check_pair("psnr_cb", cb, hb);
    if (msg)
        return msg;
    msg = check_pair("psnr_cr", cr, hr);
    if (msg)
        return msg;
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_psnr_hip_registered);
    mu_run_test(test_psnr_cpu_hip_parity);
    return NULL;
}
