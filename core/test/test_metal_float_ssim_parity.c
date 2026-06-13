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
 * Metal kernel coverage round 2 — float_ssim CPU vs. Metal parity
 * (ADR-0589 metal-ssim-lcs-db-parity; ADR-0214 cross-backend gate).
 *
 * PR #351 only asserted registration. This test exercises the real
 * float_ssim Metal kernel and compares its `float_ssim` score against the
 * CPU `float_ssim` extractor on a 256x144 8-bpc YUV420P synthetic fixture.
 *
 * Tolerance: 1e-3 (looser than the 1e-4 ADR-0214 gate). SSIM is a
 * normalised similarity index in [0, 1], so absolute residuals are
 * intrinsically small. The looser bound accommodates the documented
 * workgroup-partial-sum order-of-summation differences between the CPU
 * separable convolution and the Metal vertical+horizontal MTLCompute
 * passes (see `float_ssim_metal.mm` :: extract block — the partial-sum
 * reduction is order-dependent in float). ADR-0589 §"Test plan" cites
 * 1e-3 as the working SSIM parity target before bit-exact L/C/S split
 * lands.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip.
 *
 * Cross-references:
 *   - core/src/feature/metal/float_ssim_metal.mm
 *   - core/src/feature/float_ssim.c
 *   - docs/adr/0589-metal-ssim-lcs-db-parity.md
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

/* SSIM is dimensionless [0, 1]; 1e-3 absolute matches ADR-0589 test plan. */
#define PARITY_TOL 1e-3

static int fill_fixture(VmafPicture *pic, unsigned variant)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Variant 0 = clean ramp; variant 1 = ramp + small perturbation. */
            const int v = (int)((row + col) & 0xFFu);
            const int d = (variant != 0u) ? (((row * 5u + col) & 0x7) - 3) : 0;
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

static char *run_cpu_float_ssim(double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_ssim", NULL);
    mu_assert("CPU: vmaf_use_feature(float_ssim) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("CPU: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("CPU: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("CPU: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ssim", out_score, 0u);
    mu_assert("CPU: float_ssim read failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_float_ssim(double *out_score)
{
    *out_score = NAN;
    int err = 0;

    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("Metal: vmaf_init failed", !err);
    err = vmaf_metal_import_state(vmaf, mstate);
    mu_assert("Metal: vmaf_metal_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "float_ssim_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(float_ssim_metal) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("Metal: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("Metal: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("Metal: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ssim", out_score, 0u);
    mu_assert("Metal: float_ssim read failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);
    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_float_ssim_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;

    char *msg = run_cpu_float_ssim(&cpu_score);
    if (msg)
        return msg;
    msg = run_metal_float_ssim(&metal_score);
    if (msg)
        return msg;
    if (isnan(metal_score))
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nfloat_ssim parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("float_ssim CPU vs. Metal exceeds 1e-3 tolerance (ADR-0589)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ssim_cpu_metal_parity);
    return NULL;
}
