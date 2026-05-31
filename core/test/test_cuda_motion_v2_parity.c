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
 * Round-2 GPU-kernel coverage gap-fill — motion_v2 CPU vs. CUDA parity.
 *
 * motion_v2 is the SAD-weighted variant of the motion feature (used by the
 * libvmaf-2.x.x "vmaf_v0.6.1neg" lineage models). It is implemented by
 * integer_motion_v2.c (CPU scalar) and integer_motion_v2_cuda.c +
 * integer_motion_v2/motion_v2_score.cu (CUDA kernel). The CUDA path runs
 * an additional SAD reduction over the 5-tap Gaussian convolution result —
 * a regression in the SAD reduction would silently bias every motion2_v2
 * column produced from CHUG re-extracts.
 *
 * Round-1 (PR #351) covered psnr_cuda + ciede_cuda. The existing
 * test_cuda_motion3_parity covers the *motion3* post-process (frame index
 * 1, EOS-flush path) — it does NOT exercise motion_v2's SAD reduction.
 *
 * Skip behaviour: skips with "[skip: no CUDA device]" when no CUDA driver.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u

#define PARITY_TOL 1e-4

/* motion_v2 emits the SAD score every frame and the motion2_v2 score for
 * frame indices >= 1 (mid-stream — same as motion3). */
static const char *const MOTION_V2_FEATURES[] = {
    "VMAF_integer_feature_motion_v2_sad_score",
    "VMAF_integer_feature_motion2_v2_score",
};
#define NUM_MOTION_V2_FEATURES 2u

static int fill_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 17u) & 0xFFu);
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

static char *feed_stream(VmafContext *vmaf)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_fixture(&ref, i);
        mu_assert("fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i);
        mu_assert("fill_fixture(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("vmaf_read_pictures failed", !err);
    }
    int err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);
    return NULL;
}

static char *run_cpu(double scores_out[NUM_MOTION_V2_FEATURES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "motion_v2", NULL);
    mu_assert("CPU: vmaf_use_feature(motion_v2) failed", !err);

    char *msg = feed_stream(vmaf);
    if (msg)
        return msg;

    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, MOTION_V2_FEATURES[k], &scores_out[k], 1u);
        mu_assert("CPU: vmaf_feature_score_at_index(motion_v2, idx=1) failed", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda(double scores_out[NUM_MOTION_V2_FEATURES])
{
    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++)
        scores_out[k] = NAN;

    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    int err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CUDA: vmaf_init failed", !err);

    err = vmaf_cuda_import_state(vmaf, cu_state);
    mu_assert("CUDA: vmaf_cuda_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "motion_v2_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(motion_v2_cuda) failed", !err);

    char *msg = feed_stream(vmaf);
    if (msg)
        return msg;

    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, MOTION_V2_FEATURES[k], &scores_out[k], 1u);
        mu_assert("CUDA: vmaf_feature_score_at_index(motion_v2, idx=1) failed", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_motion_v2_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2_cuda");
    mu_assert("motion_v2_cuda extractor must be registered", fex != NULL);
    mu_assert("motion_v2_cuda name matches", !strcmp(fex->name, "motion_v2_cuda"));
    return NULL;
}

static char *test_motion_v2_cpu_cuda_parity(void)
{
    double cpu[NUM_MOTION_V2_FEATURES] = {0.0, 0.0};
    double gpu[NUM_MOTION_V2_FEATURES] = {NAN, NAN};

    char *msg = run_cpu(cpu);
    if (msg)
        return msg;
    msg = run_cuda(gpu);
    if (msg)
        return msg;
    if (isnan(gpu[0]))
        return NULL;

    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nmotion_v2 parity FAIL (%s): cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                          MOTION_V2_FEATURES[k], cpu[k], gpu[k], delta, PARITY_TOL);
        }
        mu_assert("motion_v2 CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_v2_cuda_registered);
    mu_run_test(test_motion_v2_cpu_cuda_parity);
    return NULL;
}
