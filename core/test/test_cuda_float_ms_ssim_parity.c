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
 * ADR-0947 — float_ms_ssim CPU vs. CUDA parity test (round 3).
 *
 * The float-path multi-scale SSIM extractor is implemented
 * independently in core/src/feature/float_ms_ssim.c (CPU) and
 * core/src/feature/cuda/integer_ms_ssim_cuda.c (CUDA, registered as
 * `float_ms_ssim_cuda`).  Both emit the scalar `float_ms_ssim`
 * feature.  Before this test no cross-backend assertion gated drift;
 * any kernel-grid or SIMD pivot could silently shift the score.
 *
 * The 5-scale MS-SSIM pyramid requires a fixture wide enough that the
 * smallest scale (/16) still has non-trivial dimensions.  256x144
 * gives 16x9 at scale 4 — well above the 11x11 Gaussian window.
 *
 * Asserts agreement to within 1e-4 (places=4, ADR-0214) at frame
 * index 1 across 3 frames.  Skips cleanly when no CUDA device is
 * visible.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* The 5-level 11-tap MS-SSIM pyramid requires min(w,h) >= 11<<4 = 176.
 * 256x192 keeps both axes above the floor with a clean 16-px multiple. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 192u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

#define PARITY_TOL 1e-4

static int fill_ref(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 7u) & 0xFFu);
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

static int fill_dist(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (row + col + frame_idx * 7u) & 0xFFu;
            const unsigned noise = ((row * 2u + col + frame_idx * 3u) % 9u);
            y[row * pic->stride[0] + col] = (uint8_t)((base + noise) & 0xFFu);
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

static char *run_cpu(double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "float_ms_ssim", NULL);
    mu_assert("CPU: vmaf_use_feature(float_ms_ssim) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CPU: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("CPU: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", out_score, 1u);
    mu_assert("CPU: vmaf_feature_score_at_index(float_ms_ssim, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda(double *out_score)
{
    *out_score = NAN;
    int err = 0;

    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
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

    err = vmaf_use_feature(vmaf, "float_ms_ssim_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(float_ms_ssim_cuda) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CUDA: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("CUDA: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CUDA: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", out_score, 1u);
    mu_assert("CUDA: vmaf_feature_score_at_index(float_ms_ssim, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_float_ms_ssim_cpu_cuda_parity(void)
{
    double cpu_score = 0.0;
    double cuda_score = NAN;

    char *msg = run_cpu(&cpu_score);
    if (msg)
        return msg;
    msg = run_cuda(&cuda_score);
    if (msg)
        return msg;
    if (isnan(cuda_score))
        return NULL;

    mu_assert("CPU float_ms_ssim score is non-finite", isfinite(cpu_score));
    mu_assert("CUDA float_ms_ssim score is non-finite", isfinite(cuda_score));

    double delta = fabs(cpu_score - cuda_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, cuda_score, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ms_ssim_cpu_cuda_parity);
    return NULL;
}
