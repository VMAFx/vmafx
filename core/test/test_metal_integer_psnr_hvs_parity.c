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
 * Metal kernel parity — psnr_hvs CPU vs. Metal (ADR-0214 cross-backend
 * gate; ADR-0421 first-kernel Metal scaffolding).
 *
 * The PSNR-HVS extractor is registered under the name `psnr_hvs` on the
 * CPU (libvmaf/src/feature/third_party/xiph/psnr_hvs.c) and emits
 * psnr_hvs_y / psnr_hvs_cb / psnr_hvs_cr / psnr_hvs. The Metal twin
 * `integer_psnr_hvs_metal`
 * (core/src/feature/metal/integer_psnr_hvs_metal.mm +
 *  core/src/feature/metal/integer_psnr_hvs.metal) mirrors the CUDA
 * reference (cuda/integer_psnr_hvs_cuda.c + psnr_hvs_score.cu): one
 * threadgroup per output 8x8 block (step=7), 64 threads, integer
 * od_bin_fdct8x8 + CSF masking, with thread 0 running the float
 * reductions in the CPU's exact i,j summation order.
 *
 * This test runs the CPU `psnr_hvs` extractor against
 * `integer_psnr_hvs_metal` over a single-frame YUV420P fixture and
 * asserts the combined `psnr_hvs` score matches within places=4 (1e-4).
 * The integer DCT is exact on the GPU; the per-block masking and final
 * masked-error accumulation are done in float (MSL has no double, while
 * the CPU's `ret` is also a float register before the final dB
 * conversion), so the float32 residual is the cross-backend divergence
 * the 1e-4 ADR-0214 gate bounds. Both CPU and Metal default
 * enable_chroma=true, so the combined 0.8*Y + 0.1*(Cb+Cr) path is
 * exercised.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/test/test_metal_integer_ssim_parity.c (model)
 *   - core/test/test_cuda_psnr_hvs_parity.c (CUDA sibling)
 *   - core/src/feature/metal/integer_psnr_hvs_metal.mm
 *   - core/src/feature/third_party/xiph/psnr_hvs.c
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
    /* Non-flat chroma so the Cb/Cr planes contribute a meaningful
     * combined-score term (the combined psnr_hvs weights chroma at
     * 0.1 each). */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const int base = (int)((row * 3u + col * 5u) & 0xFFu);
                const int d = (variant != 0u) ? (int)(((row + col * 2u) & 0x7u)) - 3 : 0;
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

static char *run_cpu_psnr_hvs(double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "psnr_hvs", NULL);
    mu_assert("CPU: vmaf_use_feature(psnr_hvs) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("CPU: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("CPU: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("CPU: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "psnr_hvs", out_score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(psnr_hvs, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_psnr_hvs(double *out_score, int *skipped)
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

    err = vmaf_use_feature(vmaf, "integer_psnr_hvs_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(integer_psnr_hvs_metal) failed", !err);

    VmafPicture ref, dist;
    err = fill_fixture(&ref, 0u);
    mu_assert("Metal: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("Metal: fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("Metal: vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "psnr_hvs", out_score, 0u);
    mu_assert("Metal: vmaf_feature_score_at_index(psnr_hvs, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);

    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_psnr_hvs_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;
    int skipped = 0;

    char *msg = run_cpu_psnr_hvs(&cpu_score);
    if (msg)
        return msg;
    msg = run_metal_psnr_hvs(&metal_score, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\npsnr_hvs parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("psnr_hvs CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_psnr_hvs_cpu_metal_parity);
    return NULL;
}
