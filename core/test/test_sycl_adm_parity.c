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
 * SYCL kernel coverage round 2 — ADM CPU vs. SYCL parity test (ADR-0884).
 *
 * ADM (Additive Detail Measure, scale 0..3) is computed by integer_adm.c
 * (CPU scalar / SIMD) and by integer_adm_sycl.cpp (SYCL kernel, 1738
 * lines — the largest and most complex SYCL kernel in the fork). Before
 * this test there was NO cross-backend parity gate for adm_sycl. ADM
 * is the dominant feature in every shipping VMAF model
 * (libvmaf-2.x default + 4k + phone-screen). A regression in the SYCL
 * DLM (Detail-Loss Metric) sub-band convolution or the CSF (Contrast
 * Sensitivity Function) weighting would silently shift the headline
 * VMAF score on every Intel-Arc CHUG re-extract.
 *
 * The test asserts VMAF_integer_feature_adm2_score (the headline
 * combined-scale ADM2 score) matches between CPU and SYCL within
 * ADR-0214 places=4 (1e-4) tolerance.
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

/* Fixture geometry — large enough to clear ADM's 5-tap filter and
 * 4-scale dyadic pyramid (min 32x32 after scale-3 decimation), small
 * enough for fast CI. */
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
            /* Diagonal gradient + salt offset — produces non-trivial
             * detail at every ADM scale so DLM/CSF paths are exercised. */
            y[row * pic->stride[0] + col] = (uint8_t)(((row * 3u + col * 5u + salt * 17u)) & 0xFFu);
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

static char *run_cpu_adm(double *adm2)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "adm", NULL);
    mu_assert("CPU: vmaf_use_feature(adm) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_adm2_score", adm2, 0u);
    mu_assert("CPU: VMAF_integer_feature_adm2_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl_adm(double *adm2)
{
    *adm2 = NAN;
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
    err = vmaf_use_feature(vmaf, "adm_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(adm_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_adm2_score", adm2, 0u);
    mu_assert("SYCL: VMAF_integer_feature_adm2_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_adm_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm_sycl");
    mu_assert("adm_sycl extractor must be registered", fex != NULL);
    mu_assert("adm_sycl name matches", !strcmp(fex->name, "adm_sycl"));
    return NULL;
}

static char *test_adm_cpu_sycl_parity(void)
{
    double cpu_adm2 = 0.0;
    double sycl_adm2 = NAN;
    char *msg = run_cpu_adm(&cpu_adm2);
    if (msg)
        return msg;
    msg = run_sycl_adm(&sycl_adm2);
    if (msg)
        return msg;
    if (isnan(sycl_adm2))
        return NULL;
    double delta = fabs(cpu_adm2 - sycl_adm2);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nadm2 parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_adm2, sycl_adm2, delta, PARITY_TOL);
    }
    mu_assert("adm2 CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_sycl_registered);
    mu_run_test(test_adm_cpu_sycl_parity);
    return NULL;
}
