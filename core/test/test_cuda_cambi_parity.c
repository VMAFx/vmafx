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
 * Round-2 GPU-kernel coverage gap-fill — cambi CPU vs. CUDA parity.
 *
 * CAMBI (Contrast-Aware Multiscale Banding Index) is a JND-tuned banding
 * detector with non-trivial multi-scale filtering. It is implemented by
 * cambi.c (CPU scalar) and integer_cambi_cuda.c + integer_cambi/cambi_score.cu
 * (CUDA kernel). Both backends require ≥ 216×216 input — see
 * CAMBI_MIN_WIDTH_HEIGHT in cambi.c / integer_cambi_cuda.c.
 *
 * A round-trip parity gate matters because CAMBI feeds the libvmaf-2.x.x
 * "vmaf_v0.6.1neg" + "vmaf_4k_v0.6.1" model lineages — silent CUDA drift
 * would bias every CHUG re-extract that uses one of those models.
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

/* CAMBI requires ≥ 216 on at least one dimension. 256x256 is comfortably
 * above the threshold and still cheap to run in CI. */
#define FIXTURE_W 256u
#define FIXTURE_H 256u
#define FIXTURE_BPC 8u

#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    /* Build a luma pattern with step-function banding-prone gradients so
     * CAMBI emits a non-trivial score in both backends. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* 8-step quantised gradient — classic banding artefact pattern. */
            const unsigned base = (col / 32u) * 32u + salt * 4u;
            y[row * pic->stride[0] + col] = (uint8_t)(base & 0xFFu);
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

static char *feed_one_frame(VmafContext *vmaf, unsigned ref_salt, unsigned dist_salt)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, ref_salt);
    mu_assert("fill_pic(ref) failed", !err);
    err = fill_pic(&dist, dist_salt);
    mu_assert("fill_pic(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);
    return NULL;
}

static char *run_cpu_cambi(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "cambi", NULL);
    mu_assert("CPU: vmaf_use_feature(cambi) failed", !err);

    char *msg = feed_one_frame(vmaf, 0u, 1u);
    if (msg)
        return msg;

    err = vmaf_feature_score_at_index(vmaf, "Cambi_feature_cambi_score", score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(cambi) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda_cambi(double *score)
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

    err = vmaf_use_feature(vmaf, "cambi_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(cambi_cuda) failed", !err);

    char *msg = feed_one_frame(vmaf, 0u, 1u);
    if (msg)
        return msg;

    err = vmaf_feature_score_at_index(vmaf, "Cambi_feature_cambi_score", score, 0u);
    mu_assert("CUDA: vmaf_feature_score_at_index(cambi) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_cambi_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("cambi_cuda");
    mu_assert("cambi_cuda extractor must be registered", fex != NULL);
    mu_assert("cambi_cuda name matches", !strcmp(fex->name, "cambi_cuda"));
    return NULL;
}

static char *test_cambi_cpu_cuda_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;

    char *msg = run_cpu_cambi(&cpu);
    if (msg)
        return msg;
    msg = run_cuda_cambi(&gpu);
    if (msg)
        return msg;
    if (isnan(gpu))
        return NULL;

    const double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\ncambi parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n", cpu,
                      gpu, delta, PARITY_TOL);
    }
    mu_assert("cambi CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cambi_cuda_registered);
    mu_run_test(test_cambi_cpu_cuda_parity);
    return NULL;
}
