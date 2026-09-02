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
 * Round-2 GPU-kernel coverage gap-fill — adm2 / adm3 CPU vs. CUDA parity test.
 *
 * The ADM (Additive Detail Metric) is computed by integer_adm.c (CPU scalar)
 * and integer_adm_cuda.c + integer_adm/*.cu (CUDA kernel set: adm_dwt2,
 * adm_decouple, adm_csf, adm_csf_den, adm_cm). It is a load-bearing component
 * of the libvmaf-2.x.x default model — a regression in the DWT2 stage or the
 * CSF normaliser would silently bias the VMAF score across CHUG re-extracts.
 *
 * No cross-backend assertion existed before this test for the ADM kernel set;
 * round-1 (PR #351) covered psnr_cuda + ciede_cuda only.
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no CUDA driver / no device)
 * the test emits "[skip: no CUDA device]" and passes.
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

/* Fixture geometry — large enough for the 4-scale ADM DWT pyramid. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u

/* ADR-0214 cross-backend gate (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

/* The two top-level ADM features both backends must emit. */
static const char *const ADM_FEATURES[] = {
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_adm3_score",
};
#define NUM_ADM_FEATURES 2u

static int fill_ref(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row * 3u + col * 2u) & 0xFFu);
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
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Same base ramp as ref + a small additive perturbation so ADM
             * produces non-trivial scores without saturating to 1.0. */
            const int v = (int)((row * 3u + col * 2u) & 0xFFu) + 9;
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
    return NULL;
}

static char *run_cpu_adm(double scores_out[NUM_ADM_FEATURES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "adm", NULL);
    mu_assert("CPU: vmaf_use_feature(adm) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, ADM_FEATURES[k], &scores_out[k], 0u);
        mu_assert("CPU: vmaf_feature_score_at_index(adm) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda_adm(double scores_out[NUM_ADM_FEATURES])
{
    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++)
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

    err = vmaf_use_feature(vmaf, "adm_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(adm_cuda) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, ADM_FEATURES[k], &scores_out[k], 0u);
        mu_assert("CUDA: vmaf_feature_score_at_index(adm) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_adm_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm_cuda");
    mu_assert("adm_cuda extractor must be registered", fex != NULL);
    mu_assert("adm_cuda name matches", !strcmp(fex->name, "adm_cuda"));
    return NULL;
}

static char *test_adm_cpu_cuda_parity(void)
{
    double cpu[NUM_ADM_FEATURES] = {0.0, 0.0};
    double gpu[NUM_ADM_FEATURES] = {NAN, NAN};

    char *msg = run_cpu_adm(cpu);
    if (msg)
        return msg;
    msg = run_cuda_adm(gpu);
    if (msg)
        return msg;
    if (isnan(gpu[0]))
        return NULL;

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nadm parity FAIL (%s): cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                          ADM_FEATURES[k], cpu[k], gpu[k], delta, PARITY_TOL);
        }
        mu_assert("adm CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_cuda_registered);
    mu_run_test(test_adm_cpu_cuda_parity);
    return NULL;
}
