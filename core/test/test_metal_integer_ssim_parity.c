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
 * Metal kernel parity — integer_ssim CPU vs. Metal (ADR-0214 cross-backend
 * gate; ADR-0421 first-kernel Metal scaffolding).
 *
 * The integer (fixed-point) SSIM extractor is registered under the name
 * `ssim` on the CPU (libvmaf/src/feature/integer_ssim.c) and emits the
 * feature key `ssim`. The Metal twin `integer_ssim_metal`
 * (core/src/feature/metal/integer_ssim_metal.mm +
 *  core/src/feature/metal/integer_ssim.metal) mirrors the float_ssim_metal
 * two-pass separable-Gaussian dispatch but swaps the float arithmetic for
 * the CPU integer reference's fixed-point 9-tap Gaussian
 * ({2,9,28,55,68,55,28,9,2}, KERNEL_WEIGHT=256) with edge-truncated
 * convolution.
 *
 * This test runs the CPU `ssim` extractor against `integer_ssim_metal` over
 * a single-frame YUV420P fixture and asserts the `ssim` score matches within
 * places=4 (1e-4). The integer moments are computed exactly on the GPU
 * (int64 accumulators); the per-pixel SSIM combine is done in float (MSL has
 * no double, while the CPU promotes to double) — the float32 residual is the
 * cross-backend divergence the 1e-4 ADR-0214 gate bounds.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/test/test_metal_float_ssim_parity.c (float twin)
 *   - core/test/test_metal_integer_motion_parity.c (integer sibling)
 *   - core/src/feature/metal/integer_ssim_metal.mm
 *   - core/src/feature/integer_ssim.c
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
#define PARITY_TOL 1e-4

static int fill_fixture(VmafPicture *pic, unsigned variant)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const int v = (int)((row + col) & 0xFFu);
            const int d = (variant != 0u) ? (int)(((row * 9u + col) & 0x7u)) - 4 : 0;
            int clamped = v + d;
            if (clamped < 0)
                clamped = 0;
            if (clamped > 255)
                clamped = 255;
            y[row * pic->stride[0] + col] = (uint8_t)clamped;
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

static char *run_cpu_integer_ssim(double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "ssim", NULL);
    mu_assert("CPU: vmaf_use_feature(ssim) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("CPU: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("CPU: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("CPU: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "ssim", out_score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(ssim, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_integer_ssim(double *out_score, int *skipped)
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

    err = vmaf_use_feature(vmaf, "integer_ssim_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(integer_ssim_metal) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("Metal: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("Metal: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("Metal: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "ssim", out_score, 0u);
    mu_assert("Metal: vmaf_feature_score_at_index(ssim, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);

    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_integer_ssim_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;
    int skipped = 0;

    char *msg = run_cpu_integer_ssim(&cpu_score);
    if (msg)
        return msg;
    msg = run_metal_integer_ssim(&metal_score, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\ninteger_ssim parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("integer_ssim CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_ssim_cpu_metal_parity);
    return NULL;
}
