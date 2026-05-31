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
 * Metal kernel coverage round 3 — float_ms_ssim CPU vs. Metal parity
 * (T8-2a; ADR-0589 metal-ssim-lcs-db-parity; ADR-0214 cross-backend gate).
 *
 * `float_ms_ssim_metal` emits the aggregate `float_ms_ssim` score: a 5-scale
 * MS-SSIM pyramid combining luminance, contrast, and structure across the
 * Wang et al. weight vector. This test runs the CPU `float_ms_ssim`
 * extractor against `float_ms_ssim_metal` on a single-frame YUV420P
 * fixture and asserts that the public `float_ms_ssim` score matches within
 * the 1e-3 SSIM-specific bound from ADR-0589.
 *
 * Fixture dims: the 5-scale 11-tap MS-SSIM pyramid requires every input
 * dimension to satisfy `min(w, h) >= GAUSSIAN_LEN << (SCALES - 1) = 11 << 4
 * = 176` — anything smaller is rejected at init with -EINVAL (see
 * `core/src/feature/float_ms_ssim.c:131-138`). The previous 256x144
 * fixture tripped that gate on every macOS runner, so the CPU twin failed
 * before the Metal path could even initialise. Use 256x192 (192 = 176
 * rounded up to a multiple of 16 for clean pyramid downsamples).
 *
 * Tolerance rationale: SSIM-family metrics are normalised to [0, 1] and
 * the Metal pyramid uses workgroup-partial-sum reductions across 5 scales,
 * each with separable convolution + downsample passes. ADR-0589 §"Test
 * plan" cites 1e-3 as the working parity target for the float_ssim /
 * float_ms_ssim family before bit-exact L/C/S separation lands; this test
 * inherits the same bound rather than the tighter 1e-4 from ADR-0214.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/src/feature/metal/float_ms_ssim_metal.mm
 *   - core/src/feature/float_ms_ssim.c
 *   - core/test/test_metal_float_ssim_parity.c (sibling, single-scale SSIM)
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
#define FIXTURE_H 192u
#define FIXTURE_BPC 8u

/* MS-SSIM inherits the SSIM-family 1e-3 bound from ADR-0589. */
#define PARITY_TOL 1e-3

static int fill_fixture(VmafPicture *pic, unsigned variant)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
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

static char *run_cpu_float_ms_ssim(double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_ms_ssim", NULL);
    mu_assert("CPU: vmaf_use_feature(float_ms_ssim) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("CPU: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("CPU: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("CPU: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", out_score, 0u);
    mu_assert("CPU: float_ms_ssim read failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_float_ms_ssim(double *out_score)
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
    err = vmaf_use_feature(vmaf, "float_ms_ssim_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(float_ms_ssim_metal) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("Metal: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("Metal: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("Metal: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", out_score, 0u);
    mu_assert("Metal: float_ms_ssim read failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);
    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_float_ms_ssim_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;

    char *msg = run_cpu_float_ms_ssim(&cpu_score);
    if (msg)
        return msg;
    msg = run_metal_float_ms_ssim(&metal_score);
    if (msg)
        return msg;
    if (isnan(metal_score))
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim CPU vs. Metal exceeds 1e-3 tolerance (ADR-0589)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ms_ssim_cpu_metal_parity);
    return NULL;
}
