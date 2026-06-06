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
 * ADR-0883 round-2 — integer motion (v1) CPU vs. HIP parity test.
 *
 * The original integer motion (v1) feature is computed by
 * integer_motion.c (CPU) and by integer_motion_hip.c +
 * integer_motion/motion_score.hip (HIP).  Existing tests cover the
 * motion3 (motion_v2) HIP variant; this test closes the v1 gap so
 * any future divergence between the Gaussian-blur convolution +
 * |ref(t) - ref(t-1)| sum-of-absolute-differences kernels is caught
 * at CI time.
 *
 * Asserts the single emitted `VMAF_integer_feature_motion_score`
 * channel.  Requires 2 frames so the motion delta is non-zero
 * (frame 0 emits a forced-zero score by spec).
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
#include "libvmaf/feature.h"
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
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + salt * 23u) & 0xFFu);
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

static int feed_two_frames(VmafContext *vmaf)
{
    /* Motion needs t-1, so frame 1 carries the score we read at index 1. */
    for (unsigned i = 0; i < 2; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, i);
        if (err)
            return err;
        err = fill_pic(&dist, i + 1u);
        if (err) {
            vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return err;
    }
    return 0;
}

static char *run_cpu_motion(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    /* integer_motion only writes VMAF_integer_feature_motion_score when
     * debug=true; without it only motion_sad_score is emitted.  Enable debug
     * so the same named channel is available on both the CPU and HIP paths. */
    VmafFeatureDictionary *opts = NULL;
    err = vmaf_feature_dictionary_set(&opts, "debug", "true");
    mu_assert("CPU: vmaf_feature_dictionary_set(debug) failed", !err);
    err = vmaf_use_feature(vmaf, "motion", opts);
    mu_assert("CPU: vmaf_use_feature(motion) failed", !err);
    err = feed_two_frames(vmaf);
    mu_assert("CPU: feed_two_frames failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion_score", score, 1u);
    mu_assert("CPU: motion_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_hip_motion(double *score)
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
    err = vmaf_use_feature(vmaf, "motion_hip", NULL);
    mu_assert("HIP: vmaf_use_feature(motion_hip) failed", !err);
    err = feed_two_frames(vmaf);
    mu_assert("HIP: feed_two_frames failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion_score", score, 1u);
    mu_assert("HIP: motion_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_motion_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_hip");
    mu_assert("motion_hip extractor must be registered", fex != NULL);
    mu_assert("motion_hip name matches", !strcmp(fex->name, "motion_hip"));
    return NULL;
}

static char *test_motion_cpu_hip_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;
    char *msg = run_cpu_motion(&cpu);
    if (msg)
        return msg;
    msg = run_hip_motion(&gpu);
    if (msg)
        return msg;
    if (isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nmotion parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n", cpu,
                      gpu, delta, PARITY_TOL);
    }
    mu_assert("motion CPU vs. HIP delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_hip_registered);
    mu_run_test(test_motion_cpu_hip_parity);
    return NULL;
}
