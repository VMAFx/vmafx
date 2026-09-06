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
 * ADR-0947 — float_psnr CPU vs. CUDA parity test (round 3).
 *
 * The float-path PSNR extractor is implemented independently in
 * core/src/feature/float_psnr.c (CPU) and
 * core/src/feature/cuda/float_psnr_cuda.c (CUDA).  Both emit the
 * scalar `float_psnr` feature (luma, full-precision float reduction).
 * Before this test no cross-backend assertion gated drift between the
 * two; a SIMD pivot on the CPU side or a kernel-grid change on the
 * CUDA side could silently shift the score without surfacing in any
 * golden gate.
 *
 * This test allocates a 256x144 YUV420P 8-bpc synthetic fixture with
 * deterministic ramp patterns that differ between ref and dist (so the
 * PSNR is non-trivial and finite), feeds 3 frames through each
 * backend, and asserts that the `float_psnr` score at frame index 1
 * matches to within 1e-4 (places=4, ADR-0214 cross-backend gate).
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no driver / no
 * device) the test emits "[skip: no CUDA device]" and passes.
 * Mirrors test_cuda_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

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
#define NUM_FRAMES 3u

/* ADR-0214 cross-backend tolerance (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

static int fill_ref(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 5u) & 0xFFu);
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

/* Distorted picture: ref + per-pixel deterministic offset so noise is
 * non-zero and the PSNR is finite (~30-40 dB region). */
static int fill_dist(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (row + col + frame_idx * 5u) & 0xFFu;
            const unsigned noise = ((row * 3u + col * 2u + frame_idx) % 11u);
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

    err = vmaf_use_feature(vmaf, "float_psnr", NULL);
    mu_assert("CPU: vmaf_use_feature(float_psnr) failed", !err);

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

    err = vmaf_feature_score_at_index(vmaf, "float_psnr", out_score, 1u);
    mu_assert("CPU: vmaf_feature_score_at_index(float_psnr, idx=1) failed", !err);

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

    err = vmaf_use_feature(vmaf, "float_psnr_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(float_psnr_cuda) failed", !err);

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

    err = vmaf_feature_score_at_index(vmaf, "float_psnr", out_score, 1u);
    mu_assert("CUDA: vmaf_feature_score_at_index(float_psnr, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_float_psnr_cpu_cuda_parity(void)
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
        return NULL; /* skip path */

    /* Sanity: PSNR for our fixture should land in 30-50 dB region. */
    mu_assert("CPU float_psnr score is non-finite", isfinite(cpu_score));
    mu_assert("CUDA float_psnr score is non-finite", isfinite(cuda_score));

    double delta = fabs(cpu_score - cuda_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nfloat_psnr parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, cuda_score, delta, PARITY_TOL);
    }
    mu_assert("float_psnr CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_psnr_cpu_cuda_parity);
    return NULL;
}
