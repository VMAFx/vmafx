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
 * Metal kernel parity — ssimulacra2 CPU vs. Metal (ADR-0214 cross-backend
 * gate).
 *
 * The ssimulacra2 extractor is registered under the name `ssimulacra2` on
 * the CPU (core/src/feature/ssimulacra2.c) and emits the feature key
 * `ssimulacra2`. The Metal twin `ssimulacra2_metal`
 * (core/src/feature/metal/ssimulacra2_metal.mm +
 *  core/src/feature/metal/ssimulacra2.metal) mirrors the CUDA twin's
 * host/GPU split: the host runs YUV->linear-RGB, linear-RGB->XYB,
 * the per-pixel SSIM + EdgeDiff combine (double precision), the 2x2
 * downsample and the 108-weight polynomial pool, while the GPU runs only
 * the elementwise 3-plane multiply and the separable FastGaussian IIR
 * blur. Keeping the float-cbrt-sensitive XYB transform and the
 * double-precision combine on the host is what makes places=4 reachable
 * (cf. ssimulacra2_cuda.c / ADR-0201).
 *
 * This test runs the CPU `ssimulacra2` extractor against
 * `ssimulacra2_metal` over a 3-frame 256x144 YUV420P 8-bpc fixture and
 * asserts the `ssimulacra2` score at frame index 1 matches within
 * places=4 (1e-4). The 256x144 surface is large enough for the 6-scale
 * downsampling pyramid (smallest scale ~8x4). The residual cross-backend
 * divergence is the float32 IIR blur (the no-contract MSL barrier in
 * ssimulacra2.metal bounds the FMA drift) vs the CPU's -ffp-contract=off
 * scalar IIR; the 1e-4 ADR-0214 gate bounds it.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/test/test_cuda_ssimulacra2_parity.c (CUDA twin)
 *   - core/test/test_metal_integer_ssim_parity.c (Metal sibling / model)
 *   - core/src/feature/metal/ssimulacra2_metal.mm
 *   - core/src/feature/ssimulacra2.c
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"

#define FIXTURE_W 256u
#define FIXTURE_H 144u
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
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 5u) & 0xFFu);
        }
    }
    /* ssimulacra2 reads chroma planes through the YUV->XYB conversion;
     * give them deterministic non-128 values so the score is non-trivial. */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] =
                    (uint8_t)((row * 2u + col + p * 19u + frame_idx) & 0xFFu);
            }
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
            const unsigned base = (row + col + frame_idx * 5u) & 0xFFu;
            const unsigned noise = ((row * 2u + col + frame_idx * 3u) % 13u);
            y[row * pic->stride[0] + col] = (uint8_t)((base + noise) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const unsigned base = (row * 2u + col + p * 19u + frame_idx) & 0xFFu;
                const unsigned noise = ((row + col * 3u + frame_idx) % 7u);
                plane[row * pic->stride[p] + col] = (uint8_t)((base + noise) & 0xFFu);
            }
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

    err = vmaf_use_feature(vmaf, "ssimulacra2", NULL);
    mu_assert("CPU: vmaf_use_feature(ssimulacra2) failed", !err);

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

    err = vmaf_feature_score_at_index(vmaf, "ssimulacra2", out_score, 1u);
    mu_assert("CPU: vmaf_feature_score_at_index(ssimulacra2, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal(double *out_score, int *skipped)
{
    *skipped = 0;
    *out_score = NAN;
    int err = 0;

    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        *skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("Metal: vmaf_init failed", !err);

    err = vmaf_metal_import_state(vmaf, mstate);
    mu_assert("Metal: vmaf_metal_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "ssimulacra2_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(ssimulacra2_metal) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("Metal: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("Metal: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("Metal: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "ssimulacra2", out_score, 1u);
    mu_assert("Metal: vmaf_feature_score_at_index(ssimulacra2, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);

    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_ssimulacra2_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;
    int skipped = 0;

    char *msg = run_cpu(&cpu_score);
    if (msg)
        return msg;
    msg = run_metal(&metal_score, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    mu_assert("CPU ssimulacra2 score is non-finite", isfinite(cpu_score));
    mu_assert("Metal ssimulacra2 score is non-finite", isfinite(metal_score));

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nssimulacra2 parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("ssimulacra2 CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ssimulacra2_cpu_metal_parity);
    return NULL;
}
