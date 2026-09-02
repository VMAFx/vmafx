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
 * Round-2 GPU-kernel coverage gap-fill — integer_ssim CPU vs. CUDA parity.
 *
 * SSIM (Structural Similarity Index) is implemented in integer_ssim.c
 * (CPU scalar) and ssim_cuda.c + integer_ssim/integer_ssim_score.cu (CUDA
 * kernel). Both backends register under their respective feature names
 * (cpu: "ssim", cuda: "integer_ssim_cuda") and emit the "ssim" feature
 * via the feature_collector. SSIM is a standard reference companion to
 * VMAF — silent drift in the 11x11 Gaussian-windowed luminance / contrast
 * / structure correlation would mis-report quality in every cross-backend
 * comparison and CHUG sidecar.
 *
 * Round-1 (PR #351) covered psnr_cuda + ciede_cuda only.
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

#define PARITY_TOL 1e-4

static int fill_ref(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col) & 0xFFu);
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

static int fill_dist(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    /* Distorted = ref + 7-unit additive offset → SSIM well under 1.0. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const int v = (int)((row + col) & 0xFFu) + 7;
            y[row * pic->stride[0] + col] = (uint8_t)(v > 255 ? 255 : v);
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

static char *feed_one_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_ref(&ref);
    mu_assert("fill_ref failed", !err);
    err = fill_dist(&dist);
    mu_assert("fill_dist failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);
    return NULL;
}

static char *run_cpu_ssim(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "ssim", NULL);
    mu_assert("CPU: vmaf_use_feature(ssim) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;

    err = vmaf_feature_score_at_index(vmaf, "ssim", score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(ssim) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda_ssim(double *score)
{
    *score = NAN;
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

    err = vmaf_use_feature(vmaf, "integer_ssim_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(integer_ssim_cuda) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;

    err = vmaf_feature_score_at_index(vmaf, "ssim", score, 0u);
    mu_assert("CUDA: vmaf_feature_score_at_index(ssim) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_ssim_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("integer_ssim_cuda");
    mu_assert("integer_ssim_cuda extractor must be registered", fex != NULL);
    mu_assert("integer_ssim_cuda name matches", !strcmp(fex->name, "integer_ssim_cuda"));
    return NULL;
}

static char *test_ssim_cpu_cuda_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;

    char *msg = run_cpu_ssim(&cpu);
    if (msg)
        return msg;
    msg = run_cuda_ssim(&gpu);
    if (msg)
        return msg;
    if (isnan(gpu))
        return NULL;

    const double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nssim parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n", cpu,
                      gpu, delta, PARITY_TOL);
    }
    mu_assert("ssim CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ssim_cuda_registered);
    mu_run_test(test_ssim_cpu_cuda_parity);
    return NULL;
}
