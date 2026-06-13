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
 * SYCL kernel coverage round 2 — MS-SSIM CPU vs. SYCL parity test
 * (ADR-0884).
 *
 * MS-SSIM (5-scale Multi-Scale SSIM with exponent weighting) is
 * computed by float_ms_ssim.c (CPU scalar) and by
 * integer_ms_ssim_sycl.cpp::vmaf_fex_float_ms_ssim_sycl (SYCL kernel
 * — a stack of 5 dyadic Gaussian + downsample passes culminating in
 * an exponent-weighted product). Before this test there was NO
 * cross-backend parity gate for float_ms_ssim_sycl.
 *
 * The 5-scale exponent stack is the most numerically delicate of the
 * SYCL SSIM family — a single off-by-one in the pyramid-stride
 * calculation produces a ~percent-level shift at scale 4 that
 * vanishes at scale 0, so a single-point check would catch it where
 * cross-backend ULP-by-ULP gates would not.
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

/* Fixture must accommodate the 5-scale dyadic pyramid combined with
 * the 11-tap Gaussian: min dimension is `MS_SSIM_GAUSSIAN_LEN << (MS_SSIM_SCALES - 1)`
 * = 11 << 4 = 176. We use 256x192 — both dimensions ≥ 176 and the
 * 256/192 ratio still resembles a typical 16:9 content shape. */
#define FIXTURE_W 256u
#define FIXTURE_H 192u
#define FIXTURE_BPC 8u
/* MS-SSIM has more variance across the 5-scale exponent stack than
 * single-scale SSIM; ADR-0214 places=4 (1e-4) still applies. */
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Distinct gradient pattern from other parity tests so
             * the test exercises a different ms_ssim score region. */
            y[row * pic->stride[0] + col] = (uint8_t)((row * 2u + col * 3u + salt * 19u) & 0xFFu);
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

static char *run_cpu_ms_ssim(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_ms_ssim", NULL);
    mu_assert("CPU: vmaf_use_feature(float_ms_ssim) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", score, 0u);
    mu_assert("CPU: float_ms_ssim score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl_ms_ssim(double *score)
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
    err = vmaf_use_feature(vmaf, "float_ms_ssim_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(float_ms_ssim_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", score, 0u);
    mu_assert("SYCL: float_ms_ssim score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_ms_ssim_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim_sycl");
    mu_assert("float_ms_ssim_sycl extractor must be registered", fex != NULL);
    mu_assert("float_ms_ssim_sycl name matches", !strcmp(fex->name, "float_ms_ssim_sycl"));
    return NULL;
}

static char *test_ms_ssim_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;
    char *msg = run_cpu_ms_ssim(&cpu_score);
    if (msg)
        return msg;
    msg = run_sycl_ms_ssim(&sycl_score);
    if (msg)
        return msg;
    if (isnan(sycl_score))
        return NULL;
    double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ms_ssim_sycl_registered);
    mu_run_test(test_ms_ssim_cpu_sycl_parity);
    return NULL;
}
