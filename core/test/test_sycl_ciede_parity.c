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
 * SYCL kernel coverage round 2 — CIEDE2000 CPU vs. SYCL parity test
 * (ADR-0884).
 *
 * The CIEDE2000 colour-difference metric is computed by ciede.c (CPU
 * scalar / SIMD) and by integer_ciede_sycl.cpp (SYCL kernel). Before
 * this test there was NO cross-backend parity gate for ciede_sycl on
 * the Intel-Arc lane (the CUDA + HIP equivalents are covered by
 * PR #351's test_cuda_ciede_parity / ADR-0868).
 *
 * The Vulkan CIEDE2000 path has a documented NVIDIA-only f32/f64
 * precision gap (T-VK-CIEDE-F32-F64 / ADR-0273), so it's important to
 * pin the SYCL path independently — SYCL runs in double-precision on
 * Intel iGPU + dGPU and should match the CPU reference at places=4.
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

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
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
            y[row * pic->stride[0] + col] = (uint8_t)((row * 5u + col + salt * 11u) & 0xFFu);
        }
    }
    /* Chroma planes carry the colour delta that CIEDE2000 measures —
     * vary them with row/col + salt so ΔE != 0 on every CTU. */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] =
                    (uint8_t)((row + col * (1u + p) + salt * 7u) & 0xFFu);
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

static char *run_cpu_ciede(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "ciede", NULL);
    mu_assert("CPU: vmaf_use_feature(ciede) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "ciede2000", score, 0u);
    mu_assert("CPU: ciede2000 score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl_ciede(double *score)
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
    err = vmaf_use_feature(vmaf, "ciede_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(ciede_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "ciede2000", score, 0u);
    mu_assert("SYCL: ciede2000 score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_ciede_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ciede_sycl");
    mu_assert("ciede_sycl extractor must be registered", fex != NULL);
    mu_assert("ciede_sycl name matches", !strcmp(fex->name, "ciede_sycl"));
    return NULL;
}

static char *test_ciede_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;
    char *msg = run_cpu_ciede(&cpu_score);
    if (msg)
        return msg;
    msg = run_sycl_ciede(&sycl_score);
    if (msg)
        return msg;
    if (isnan(sycl_score))
        return NULL;
    double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nciede2000 parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("ciede2000 CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ciede_sycl_registered);
    mu_run_test(test_ciede_cpu_sycl_parity);
    return NULL;
}
