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
 * SYCL kernel coverage round 3 — PSNR-HVS CPU vs. SYCL parity test
 * (ADR-0946).
 *
 * The PSNR-HVS extractor is implemented by
 * core/src/feature/third_party/xiph/psnr_hvs.c (CPU scalar, derived
 * from Xiph daala-tools) and by
 * integer_psnr_hvs_sycl.cpp::vmaf_fex_psnr_hvs_sycl (SYCL DCT8x8 +
 * CSF mask + per-block MSE reduction). PSNR-HVS is one of the largest
 * remaining SYCL parity gaps — the kernel runs a per-8x8-block DCT
 * with a 64-entry CSF lookup and a sub-group reduction, all of which
 * are independent of the integer-family extractors covered in
 * rounds 1+2.
 *
 * Drift in the DCT precision, CSF lookup table, or sub-group mask
 * would silently corrupt every psnr_hvs column on Intel-Arc CHUG
 * re-extracts and any psnr_hvs-using libvmaf model.
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

/* Fixture must be ≥ 8x8 for the DCT block size and ideally a
 * multiple of 8 in both dimensions; 256x144 satisfies both
 * (256 % 8 == 0, 144 % 8 == 0). */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* XOR pattern + salt — yields non-trivial DCT coefficients
             * across the 8x8 blocks. */
            y[row * pic->stride[0] + col] = (uint8_t)(((row ^ col) + salt * 19u) & 0xFFu);
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
    err = vmaf_use_feature(vmaf, "psnr_hvs", NULL);
    mu_assert("CPU: vmaf_use_feature(psnr_hvs) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_hvs", score, 0u);
    mu_assert("CPU: psnr_hvs score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl(double *score)
{
    *score = NAN;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);
    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "psnr_hvs_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(psnr_hvs_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_hvs", score, 0u);
    mu_assert("SYCL: psnr_hvs score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_psnr_hvs_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs_sycl");
    mu_assert("psnr_hvs_sycl extractor must be registered", fex != NULL);
    mu_assert("psnr_hvs_sycl name matches", !strcmp(fex->name, "psnr_hvs_sycl"));
    return NULL;
}

static char *test_psnr_hvs_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;
    char *msg = run_cpu(&cpu_score);
    if (msg)
        return msg;
    msg = run_sycl(&sycl_score);
    if (msg)
        return msg;
    if (isnan(sycl_score))
        return NULL;
    double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\npsnr_hvs parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("psnr_hvs CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_psnr_hvs_sycl_registered);
    mu_run_test(test_psnr_hvs_cpu_sycl_parity);
    return NULL;
}
