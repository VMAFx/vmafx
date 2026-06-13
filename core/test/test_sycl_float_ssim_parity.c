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
 * SYCL float_ssim CPU vs. SYCL parity test (Research-0985).
 *
 * `float_ssim_sycl` (vmaf_fex_float_ssim_sycl in integer_ssim_sycl.cpp)
 * implements the Wang et al. (2004) combined SSIM formula using a
 * separable 11-tap Gaussian convolution + per-WG float partial sums.
 * The companion `integer_ssim_sycl` (same TU) is covered by
 * test_sycl_ssim_parity.c; this test covers the floating-point path.
 *
 * Precision contract: the CPU path (float_ssim.c via iqa_ssim) uses the
 * L×C×S decomposition with a sqrt(var_ref * var_cmp) contrast term, while
 * the SYCL path uses the combined formula 2*covar / (var_ref + var_cmp).
 * These are mathematically distinct — the places=3 (5e-04) tolerance here
 * accommodates both the formula difference and Arc A380 class fp32
 * accumulation drift. On fp64-capable hardware (RTX 4090) the same kernel
 * passes at places=4 (5e-05).
 *
 * See Research-0985 §3 for the full divergence analysis.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime or
 * no device visible) the test emits "[skip: no SYCL device]" and passes,
 * mirroring test_sycl_motion3_parity.c.
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

/* 320×180 is above the 11×11 Gaussian footprint minimum and within the
 * scale=1 auto-detect threshold (min(w,h)/256 = 180/256 ≈ 0.7 → scale=1).
 * The kernel requires scale=1; auto-detect with this fixture avoids the
 * need to pass options. */
#define FIXTURE_W 320u
#define FIXTURE_H 180u
#define FIXTURE_BPC 8u

/*
 * Tolerance is places=3 (5e-04):
 * - Accommodates the formula difference between CPU L×C×S and GPU combined form
 * - Accommodates Arc A380 class fp32 accumulation drift (~2.7e-04 observed)
 * - RTX 4090 and fp64-capable hardware will pass at places=4 (5e-05)
 * See Research-0985 §3.4 for rationale.
 */
#define PARITY_TOL 5e-04

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Sinusoidal-ish pattern via XOR — gives non-trivial
             * variance across the Gaussian footprint. */
            y[row * pic->stride[0] + col] = (uint8_t)(((row ^ col) + salt * 17u) & 0xFFu);
        }
    }
    /* Chroma planes: flat grey (float_ssim uses luma only). */
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

static char *run_cpu_float_ssim(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_ssim", NULL);
    mu_assert("CPU: vmaf_use_feature(float_ssim) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "float_ssim", score, 0u);
    mu_assert("CPU: float_ssim score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl_float_ssim(double *score, int *device_present)
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
    err = vmaf_use_feature(vmaf, "float_ssim_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(float_ssim_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "float_ssim", score, 0u);
    mu_assert("SYCL: float_ssim score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_float_ssim_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim_sycl");
    mu_assert("float_ssim_sycl extractor must be registered", fex != NULL);
    mu_assert("float_ssim_sycl name matches", !strcmp(fex->name, "float_ssim_sycl"));
    return NULL;
}

static char *test_float_ssim_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;
    int device_present = 0;
    char *msg = run_cpu_float_ssim(&cpu_score);
    if (msg)
        return msg;
    msg = run_sycl_float_ssim(&sycl_score, &device_present);
    if (msg)
        return msg;
    if (!device_present)
        return NULL;
    double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ssim parity FAIL: cpu=%.8f sycl=%.8f "
                      "delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    /* places=3 (5e-04): accommodates both the CPU/GPU formula difference
     * (combined vs L×C×S) and Arc A380 fp32 accumulation drift.
     * See Research-0985 §3.4 for full rationale. */
    mu_assert("float_ssim CPU vs. SYCL delta exceeds places=3 tolerance (5e-04)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ssim_sycl_registered);
    mu_run_test(test_float_ssim_cpu_sycl_parity);
    return NULL;
}
