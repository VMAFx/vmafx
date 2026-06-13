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
 * GPU-kernel coverage gap-fill — VIF CPU vs. HIP parity test.
 *
 * The integer VIF (Visual Information Fidelity) scale0 feature is
 * computed by integer_vif.c (CPU) and by integer_vif_hip.c +
 * integer_vif/vif_statistics.hip (HIP kernel). The HIP path has a
 * smoke test (test_hip_smoke.c) but no cross-backend parity gate; this
 * test closes that gap so a regression in the separable Gaussian filter
 * or the M1/M2/M3/M4 statistical accumulator on AMD GPUs is caught at
 * CI time rather than at downstream model-prediction time.
 *
 * Asserts only scale0 because that is the score most sensitive to filter
 * accuracy and avoids the wider tolerance budgets that the lower-resolution
 * scales accumulate.
 *
 * Skip behaviour: if vmaf_hip_state_init() fails (no HIP runtime or
 * no device visible) the test emits "[skip: no HIP device]" and passes.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
/* ADR-0568: mirror2_i boundary fix brings integer_vif_hip to places=6 on real
 * content (max |HIP−CPU| ≈ 1e-6 on the Netflix src01 576×324 pair, all 48
 * frames).  Tighten from places=3 to places=4 per the ADR-0214 gate.
 * The synthetic gradient fixture here is simpler than natural video, so
 * places=6 is achievable; places=4 (1e-4) is the hard gate floor per ADR-0214
 * and ADR-0566. */
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Smooth gradient so the VIF natural-scene statistics see
             * a wide variance range. salt offsets the dist plane. */
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + salt * 17u) & 0xFFu);
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

static char *run_cpu_vif(double *scale0)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "vif", NULL);
    mu_assert("CPU: vmaf_use_feature(vif) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_vif_scale0_score", scale0, 0u);
    mu_assert("CPU: vif_scale0 missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_hip_vif(double *scale0)
{
    *scale0 = NAN;
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);
    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "vif_hip", NULL);
    mu_assert("HIP: vmaf_use_feature(vif_hip) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("HIP: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_vif_scale0_score", scale0, 0u);
    mu_assert("HIP: vif_scale0 missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_vif_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("vif_hip");
    mu_assert("vif_hip extractor must be registered", fex != NULL);
    mu_assert("vif_hip name matches", !strcmp(fex->name, "vif_hip"));
    return NULL;
}

static char *test_vif_cpu_hip_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;
    char *msg = run_cpu_vif(&cpu);
    if (msg)
        return msg;
    msg = run_hip_vif(&gpu);
    if (msg)
        return msg;
    if (isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nvif_scale0 parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("vif_scale0 CPU vs. HIP delta exceeds places=4 tolerance (1e-4; ADR-0214/ADR-0568)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_vif_hip_registered);
    mu_run_test(test_vif_cpu_hip_parity);
    return NULL;
}
