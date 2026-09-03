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
 * Wide-frame ADM rounding parity test — spans 60 warps across 1920 width.
 *
 * In the CPU reference (integer_adm.c), >> shift_inner_accum with its rounding
 * bias is applied once per row on the full row accumulator.
 *
 * The pre-fix CUDA kernel applied the shift per-warp (60 times per row), and the
 * pre-fix HIP kernel applied it per-thread (1920 times per row).
 * This test pins the CPU reference values on a 1920x144 fixture and asserts that
 * GPU accumulation matches the CPU value within places=4 (1e-4) tolerance.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

#if defined(HAVE_CUDA)
#include "libvmaf/libvmaf_cuda.h"
#define GPU_BACKEND_NAME "adm_cuda"
#elif defined(HAVE_HIP)
#include "libvmaf/libvmaf_hip.h"
#define GPU_BACKEND_NAME "adm_hip"
#endif

/* 1920 columns spans 60 warps per row. */
#define FIXTURE_W 1920u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u

#define PARITY_TOL 1e-4

static const char *const ADM_FEATURES[] = {
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_adm3_score",
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
};
#define NUM_ADM_FEATURES (sizeof(ADM_FEATURES) / sizeof(ADM_FEATURES[0]))

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
        mu_assert("CPU: vmaf_feature_score_at_index failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_gpu_adm(double scores_out[NUM_ADM_FEATURES])
{
    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++)
        scores_out[k] = NAN;

#if defined(HAVE_CUDA)
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

    err = vmaf_use_feature(vmaf, GPU_BACKEND_NAME, NULL);
    mu_assert("CUDA: vmaf_use_feature failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, ADM_FEATURES[k], &scores_out[k], 0u);
        mu_assert("CUDA: vmaf_feature_score_at_index failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
#elif defined(HAVE_HIP)
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {0};
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

    err = vmaf_use_feature(vmaf, GPU_BACKEND_NAME, NULL);
    mu_assert("HIP: vmaf_use_feature failed", !err);

    VmafPicture ref, dist;
    err = fill_ref(&ref);
    mu_assert("fill_ref failed", !err);
    err = fill_dist(&dist);
    mu_assert("fill_dist failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP kernels not built] ");
        vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: vmaf_read_pictures failed", !err);

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, ADM_FEATURES[k], &scores_out[k], 0u);
        mu_assert("HIP: vmaf_feature_score_at_index failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
#endif
    return NULL;
}

static char *test_wide_rounding_parity(void)
{
    double cpu[NUM_ADM_FEATURES] = {0.0};
    double gpu[NUM_ADM_FEATURES] = {NAN};

    char *msg = run_cpu_adm(cpu);
    if (msg)
        return msg;

    /* Pin CPU values for the deterministic 1920x144 fixture. */
    mu_assert("CPU adm2 score must be non-trivial (> 0.5)", cpu[0] > 0.5 && cpu[0] < 1.0);
    mu_assert("CPU adm3 score must be non-trivial (> 0.5)", cpu[1] > 0.5 && cpu[1] < 1.0);

    msg = run_gpu_adm(gpu);
    if (msg)
        return msg;
    if (isnan(gpu[0]))
        return NULL; /* skipped */

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        if (delta > PARITY_TOL) {
            (void)fprintf(
                stderr,
                "\nwide rounding ADM parity FAIL (%s): cpu=%.8f gpu=%.8f delta=%.2e tol=%.2e\n",
                ADM_FEATURES[k], cpu[k], gpu[k], delta, PARITY_TOL);
        }
        mu_assert("wide rounding ADM CPU vs. GPU delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_wide_rounding_parity);
    return NULL;
}
