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
 * SYCL kernel coverage round 4 — SSIMULACRA2 CPU vs. SYCL parity test
 * (ADR-0957).
 *
 * The SSIMULACRA2 extractor is implemented by ssimulacra2.c (CPU
 * scalar via vmaf_fex_ssimulacra2) and by
 * ssimulacra2_sycl.cpp::vmaf_fex_ssimulacra2_sycl (hybrid host + SYCL
 * GPU port of the ADR-0201 Vulkan pipeline: host YUV→linear-RGB +
 * 2x2 box downsample + linear-RGB→XYB + per-pixel SSIM/EdgeDiff
 * combine; GPU 3-plane elementwise multiply + separable Charalampidis
 * 2016 3-pole IIR blur — ADR-0206).
 *
 * Round 3 (ADR-0946) deferred this kernel because the SYCL twin
 * relies on the `yuv_matrix` option-table entry that was not yet
 * templated in the round-2 / round-3 scaffold. Defaults match
 * between CPU and SYCL (`bt709_limited`) so a `NULL` options dict
 * exercises the same numerical path on both sides.
 *
 * Tolerance: 5e-3 — matches the ADR-0214 `FEATURE_TOLERANCE`
 * entry for ssimulacra2 (looser than the places=4 baseline because
 * the multi-stage XYB + IIR + SSIM-combine + log float pipeline
 * accumulates per-stage rounding; ADR-0192 §"Per-feature precision
 * contracts" anticipated places=2 / measure-first).
 *
 * A stride, IIR-coefficient, or XYB-LUT drift in the SYCL kernel
 * would silently shift every SSIMULACRA2 column on Intel-Arc CHUG
 * re-extracts.
 *
 * Headline score asserted: "ssimulacra2" at frame index 0.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime
 * or no device visible) the test emits "[skip: no SYCL device]" and
 * passes, mirroring test_sycl_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* 256x144 matches the round-2 / round-3 fixture footprint and stays
 * comfortably above the 8x8 ssimulacra2 minimum. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
/* ADR-0214 `FEATURE_TOLERANCE['ssimulacra2'] = 5e-3` — the multi-
 * stage XYB + IIR + SSIM-combine pipeline accumulates per-stage
 * float rounding past the places=4 baseline. */
#define PARITY_TOL 5e-3

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    /* All three planes carry signal — SSIMULACRA2 consumes YUV →
     * linear-RGB → XYB, so chroma matters for the headline score. */
    for (unsigned p = 0; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                /* XOR + salt + plane-dependent stride keeps every
                 * channel non-trivial. */
                plane[row * pic->stride[p] + col] =
                    (uint8_t)(((row ^ col) + salt * 19u + p * 23u) & 0xFFu);
            }
        }
    }
    return 0;
}

static int feed_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 0u);
    if (err)
        return err;
    err = fill_pic(&dist, 1u);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

static char *run_cpu(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "ssimulacra2", NULL);
    mu_assert("CPU: vmaf_use_feature(ssimulacra2) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "ssimulacra2", score, 0u);
    mu_assert("CPU: ssimulacra2 score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl(double *score, int *device_present)
{
    *score = NAN;
    *device_present = 0;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    *device_present = 1;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);
    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "ssimulacra2_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(ssimulacra2_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "ssimulacra2", score, 0u);
    mu_assert("SYCL: ssimulacra2 score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_ssimulacra2_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssimulacra2_sycl");
    mu_assert("ssimulacra2_sycl extractor must be registered", fex != NULL);
    mu_assert("ssimulacra2_sycl name matches", !strcmp(fex->name, "ssimulacra2_sycl"));
    return NULL;
}

static char *test_ssimulacra2_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;
    int device_present = 0;
    char *msg = run_cpu(&cpu_score);
    if (msg)
        return msg;
    msg = run_sycl(&sycl_score, &device_present);
    if (msg)
        return msg;
    if (!device_present)
        return NULL;
    double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nssimulacra2 parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("ssimulacra2 CPU vs. SYCL delta exceeds ADR-0214 ssimulacra2 tolerance (5e-3)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ssimulacra2_sycl_registered);
    mu_run_test(test_ssimulacra2_cpu_sycl_parity);
    return NULL;
}
