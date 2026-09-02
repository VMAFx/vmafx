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
 * Metal kernel parity — ciede2000 CPU vs. Metal (ADR-0214 cross-backend
 * gate; ADR-0421 Metal scaffolding).
 *
 * The CIEDE2000 extractor is registered under the name `ciede` on the CPU
 * (core/src/feature/ciede.c) and emits the feature key `ciede2000`. The
 * Metal twin `integer_ciede_metal`
 * (core/src/feature/metal/integer_ciede_metal.mm +
 *  core/src/feature/metal/integer_ciede.metal) ports the per-pixel
 * YUV→Lab→ΔE2000 math from the CUDA reference
 * (core/src/feature/cuda/integer_ciede/ciede_score.cu) and the SYCL twin
 * (core/src/feature/sycl/integer_ciede_sycl.cpp). Chroma is upscaled to
 * luma resolution host-side (mirrors ciede.c::scale_chroma_planes); the
 * per-pixel ΔE is reduced to one float partial per threadgroup, then the
 * host accumulates in double and applies `45 - 20*log10(mean_dE)`.
 *
 * This test runs the CPU `ciede` extractor against `integer_ciede_metal`
 * over a single-frame YUV420P fixture and asserts the `ciede2000` score
 * matches within places=4 (1e-4). The cross-backend divergence is the
 * float32 per-pixel transcendental residual against the CPU's double-
 * precision libm path — the same residual the CUDA / SYCL twins bound at
 * places=4 on real hardware (ADR-0187).
 *
 * Skip behaviour: -ENODEV (or NULL state) from `vmaf_metal_state_init`
 * -> clean skip on Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/test/test_metal_integer_ssim_parity.c (test model)
 *   - core/test/test_metal_float_psnr_parity.c (reduction sibling)
 *   - core/src/feature/metal/integer_ciede_metal.mm
 *   - core/src/feature/ciede.c
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
    /* Luma: deterministic ramp; dist applies a small bounded delta. */
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
    /* Chroma: non-neutral, spatially varying so the chroma-upscale path
     * (ss_hor / ss_ver) and the Lab a-/b-channel terms are exercised. */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const int base = (int)((row * 3u + col * 2u + p * 40u) & 0xFFu);
                const int d = (variant != 0u) ? (int)(((row + col * 5u) & 0x7u)) - 4 : 0;
                int clamped = base + d;
                if (clamped < 0)
                    clamped = 0;
                if (clamped > 255)
                    clamped = 255;
                plane[row * pic->stride[p] + col] = (uint8_t)clamped;
            }
        }
    }
    return 0;
}

static char *run_cpu_ciede(double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "ciede", NULL);
    mu_assert("CPU: vmaf_use_feature(ciede) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("CPU: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("CPU: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("CPU: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "ciede2000", out_score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(ciede2000, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_ciede(double *out_score, int *skipped)
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

    err = vmaf_use_feature(vmaf, "integer_ciede_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(integer_ciede_metal) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("Metal: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("Metal: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("Metal: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "ciede2000", out_score, 0u);
    mu_assert("Metal: vmaf_feature_score_at_index(ciede2000, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);

    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_ciede_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;
    int skipped = 0;

    char *msg = run_cpu_ciede(&cpu_score);
    if (msg)
        return msg;
    msg = run_metal_ciede(&metal_score, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nciede2000 parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("ciede2000 CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ciede_cpu_metal_parity);
    return NULL;
}
