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
 * GPU-kernel coverage gap-fill — ciede2000 CPU vs. CUDA parity test.
 *
 * The CIEDE2000 colour-difference metric is computed by ciede.c (CPU
 * scalar) and by integer_ciede_cuda.c + integer_ciede/ciede_score.cu
 * (CUDA kernel). No cross-backend assertion existed before this test;
 * a regression in the CIE-Lab conversion or the chroma-rotation kernel
 * would silently pollute the ciede2000 column used by the libvmaf-2.x.x
 * default model and by every CHUG re-extract.
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no CUDA driver or
 * no device visible) the test emits "[skip: no CUDA device]" and passes.
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
            y[row * pic->stride[0] + col] = (uint8_t)((row * 5u + col + salt * 11u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] =
                    (uint8_t)((row + col * (1u + p) + salt * 7u) & 0xFFu);
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

static char *run_cpu_ciede(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "ciede", NULL);
    mu_assert("CPU: vmaf_use_feature(ciede) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "ciede2000", score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(ciede2000) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda_ciede(double *score)
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
    err = vmaf_use_feature(vmaf, "ciede_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(ciede_cuda) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CUDA: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "ciede2000", score, 0u);
    mu_assert("CUDA: vmaf_feature_score_at_index(ciede2000) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_ciede_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ciede_cuda");
    mu_assert("ciede_cuda extractor must be registered", fex != NULL);
    mu_assert("ciede_cuda name matches", !strcmp(fex->name, "ciede_cuda"));
    return NULL;
}

static char *test_ciede_cpu_cuda_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;
    char *msg = run_cpu_ciede(&cpu);
    if (msg)
        return msg;
    msg = run_cuda_ciede(&gpu);
    if (msg)
        return msg;
    if (isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nciede2000 parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("ciede2000 CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ciede_cuda_registered);
    mu_run_test(test_ciede_cpu_cuda_parity);
    return NULL;
}
