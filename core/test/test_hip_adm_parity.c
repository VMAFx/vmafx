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
 * ADR-0539 — integer ADM CPU vs. HIP parity test.
 *
 * The integer ADM HIP kernels were ported off the CUDA-twin's
 * `cuda_helper.cuh::warp_reduce` (warpSize=32 `__shfl_down_sync`) to a
 * per-thread `atomicAdd` on the 64-bit unsigned accumulator (associative
 * + commutative, so bit-exact w.r.t. the CUDA arithmetic).  This test
 * pins the parity claim: every emitted integer_adm feature on a synthetic
 * fixture must match the CPU integer_adm extractor within places=4
 * (1e-4) per ADR-0214 cross-backend gate.
 *
 * Skip behaviour: the HIP path runs cleanly only when libvmaf was built
 * with BOTH `enable_hip=true` (HIP runtime, HSA agent visible) AND
 * `enable_hipcc=true` (device kernels compiled into the .so via the
 * HSACO embed pipeline).  The test skips in either of these two cases:
 *   1. `vmaf_hip_state_init()` fails — no HIP/ROCm driver or no device
 *      visible.  Emits "[skip: no HIP device]" and passes.
 *   2. `vmaf_read_pictures()` returns `-ENOSYS` on the first frame —
 *      libvmaf was configured with `enable_hipcc=false`, so the
 *      `adm_hip` extractor's submit/init hits the
 *      `#ifndef HAVE_HIPCC` scaffold path (see
 *      `core/src/feature/hip/integer_adm_hip.c` lines 1014-1289).
 *      Emits "[skip: HIP kernels not built (enable_hipcc=false)]"
 *      and passes.
 * Same two-axis pattern as test_hip_motion3_parity.c (ADR-0949).
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* Fixture geometry — wide enough for the ADM DWT2 pyramid (each scale
 * halves the dimensions, so >= 32x32 keeps scale 3 from collapsing). */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u

/* Tolerance matching ADR-0214 cross-backend gate (places=4 -> 1e-4). */
#define PARITY_TOL 1e-4

/* List of integer_adm features asserted for parity.  Mirrors what the
 * CPU `integer_adm` extractor and the HIP `adm_hip` extractor both emit. */
/* Restrict to the always-emitted per-scale ratios.  `integer_adm` and
 * the aggregated `integer_adm2` / `integer_adm3` keys live under the
 * `debug=true` option (see integer_adm.c) and would force the test to
 * wire up an option dict; the four scale ratios alone fully exercise
 * the dwt2/csf/csf_den/cm kernels under test. */
static const char *const kAdmFeatures[] = {
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
};
#define NUM_ADM_FEATURES (sizeof(kAdmFeatures) / sizeof(kAdmFeatures[0]))

/* Synthetic luma: simple ramp; chroma uniform. */
static int fill_ref(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row * 3u + col) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++)
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
    }
    return 0;
}

/* Distorted: ref + low-amplitude per-pixel jitter so ADM has actual
 * decoupling work to do (an identical pair would short-circuit). */
static int fill_dis(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            int v = (int)((row * 3u + col) & 0xFFu);
            v += (int)((row ^ col) & 0x07u) - 3; /* +/- 3 per-pixel jitter */
            if (v < 0)
                v = 0;
            if (v > 255)
                v = 255;
            y[row * pic->stride[0] + col] = (uint8_t)v;
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++)
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
    }
    return 0;
}

/* Per-frame submit helper.  Returns:
 *   * NULL on success;
 *   * a non-NULL message string on a hard failure (caller bubbles up);
 *   * NULL with *enosys_skip=1 when the HIP extractor's submit/init
 *     hits the `#ifndef HAVE_HIPCC` scaffold path and returns -ENOSYS.
 * Extracted from run_extractor to keep that function under the fork's
 * `readability-function-size.LineThreshold=60` budget. */
static char *adm_submit_one_frame(VmafContext *vmaf, unsigned i, int *enosys_skip)
{
    *enosys_skip = 0;
    VmafPicture ref, dist;
    int err = fill_ref(&ref);
    mu_assert("fill_ref failed", !err);
    err = fill_dis(&dist);
    mu_assert("fill_dis failed", !err);

    err = vmaf_read_pictures(vmaf, &ref, &dist, i);
    /* The HIP `adm_hip` extractor's init/submit return -ENOSYS when
     * libvmaf was built with `enable_hipcc=false` — the HSACO device
     * kernels are not embedded in the .so, so dispatch hits the
     * `#ifndef HAVE_HIPCC` scaffold path in `integer_adm_hip.c`.
     * vmaf_read_pictures unrefs the caller pictures internally on every
     * code path; the caller tears down the partially-initialised
     * VmafContext on a skip. */
    if (err == -ENOSYS) {
        *enosys_skip = 1;
        return NULL;
    }
    mu_assert("vmaf_read_pictures failed", !err);
    return NULL;
}

/* Run one extractor over the fixture; fill scores[] for kAdmFeatures. */
static char *run_extractor(const char *feature_name, int use_hip, double scores[NUM_ADM_FEATURES],
                           int *skipped)
{
    *skipped = 0;
    VmafHipState *hip_state = NULL;
    if (use_hip) {
        VmafHipConfiguration hip_cfg = {.device_index = -1};
        int rc = vmaf_hip_state_init(&hip_state, hip_cfg);
        if (rc != 0 || hip_state == NULL) {
            (void)fprintf(stderr, "[skip: no HIP device] ");
            *skipped = 1;
            return NULL;
        }
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", !err);

    if (use_hip) {
        err = vmaf_hip_import_state(vmaf, hip_state);
        mu_assert("vmaf_hip_import_state failed", !err);
    }

    err = vmaf_use_feature(vmaf, feature_name, NULL);
    mu_assert("vmaf_use_feature failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        int enosys_skip = 0;
        char *msg = adm_submit_one_frame(vmaf, i, &enosys_skip);
        if (msg)
            return msg;
        if (enosys_skip) {
            (void)fprintf(stderr, "[skip: HIP kernels not built (enable_hipcc=false)] ");
            *skipped = 1;
            (void)vmaf_close(vmaf);
            if (hip_state)
                vmaf_hip_state_free(&hip_state);
            return NULL;
        }
    }

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);

    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        err = vmaf_feature_score_at_index(vmaf, kAdmFeatures[f], &scores[f], 0u);
        if (err) {
            (void)fprintf(stderr, "feature %s missing (err=%d)\n", kAdmFeatures[f], err);
        }
        mu_assert("vmaf_feature_score_at_index failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed", !err);

    if (hip_state)
        vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_integer_adm_cpu_hip_parity(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double hip_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    /* CPU baseline: the upstream `adm` extractor (integer_adm.c) emits
     * the four scale ratios under their `integer_adm_scale*` keys. */
    char *msg = run_extractor("adm", /*use_hip=*/0, cpu_scores, &skipped);
    if (msg)
        return msg;

    /* HIP comparand: the fork's `adm_hip` extractor (integer_adm_hip.c
     * line 1391) — NOT `"adm"`, which would silently re-run the CPU
     * extractor a second time and pass trivially.  See ADR-0950. */
    msg = run_extractor("adm_hip", /*use_hip=*/1, hip_scores, &skipped);
    if (msg)
        return msg;

    if (skipped)
        return NULL; /* No HIP device or no HIPCC kernels — skip cleanly. */

    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        double d = fabs(cpu_scores[f] - hip_scores[f]);
        if (d > PARITY_TOL) {
            (void)fprintf(stderr, "\nADM parity FAIL: %s cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                          kAdmFeatures[f], cpu_scores[f], hip_scores[f], d, PARITY_TOL);
        }
        mu_assert("integer_adm CPU vs HIP delta exceeds places=4 tolerance", d <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_adm_cpu_hip_parity);
    return NULL;
}
