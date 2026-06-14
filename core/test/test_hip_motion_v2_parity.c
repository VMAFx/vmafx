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
 * ADR-1108 cross-backend follow-up — motion_v2 CPU vs. HIP parity test.
 *
 * motion_v2 is the SAD-weighted variant of the motion feature (used by the
 * libvmaf-2.x.x "vmaf_v0.6.1neg" lineage models). It is implemented by
 * integer_motion_v2.c (CPU scalar) and integer_motion_v2_hip.c +
 * integer_motion_v2/motion_v2_score.hip (HIP kernel). The HIP path runs an
 * SAD reduction over the 5-tap Gaussian convolution result, then emits the
 * motion2_v2 (frame-pair min) and motion3_v2 (per-frame blend + clip +
 * optional moving-average) post-process columns host-side in flush().
 *
 * The CUDA twin grew the motion3_v2 emission in PR #909 (ADR-1108); this
 * test is the HIP sibling. It exercises:
 *   1. the SAD reduction (motion_v2_sad_score),
 *   2. the frame-pair min (motion2_v2_score), and
 *   3. the blend/clip/stamp-seed/moving-average post-process
 *      (motion3_v2_score) — all at default options, where the HIP path is
 *      bit-exact against the CPU reference.
 *
 * The companion test_hip_motion3_parity covers the *motion3* (v1)
 * post-process; it does NOT exercise motion_v2's SAD reduction or the
 * motion3_v2 column.
 *
 * Skip behaviour: the HIP path runs cleanly only when libvmaf was built
 * with BOTH `enable_hip=true` (HIP runtime, HSA agent visible) AND
 * `enable_hipcc=true` (device kernels compiled into the .so via the HSACO
 * embed pipeline). The test skips in either of these two cases:
 *   1. `vmaf_hip_state_init()` fails — no HIP/ROCm driver or no device
 *      visible. Emits "[skip: no HIP device]" and passes.
 *   2. `vmaf_read_pictures()` returns `-ENOSYS` on the first frame —
 *      libvmaf was configured with `enable_hipcc=false`, so the
 *      `motion_v2_hip` extractor's submit/init hits the `#ifndef
 *      HAVE_HIPCC` scaffold path. Emits
 *      "[skip: HIP kernels not built (enable_hipcc=false)]" and passes.
 * Mirrors the runtime-vs-toolchain split in test_hip_motion3_parity.c.
 *
 * Reproducer (manual):
 *   tools/vmaf --reference testdata/yuv/ref_256x144_2f.yuv \
 *              --distorted testdata/yuv/ref_256x144_2f.yuv \
 *              --width 256 --height 144 --pixel_format 420 --bitdepth 8 \
 *              --feature motion_v2 \
 *              --feature motion_v2_hip
 *   # diff the motion_v2_sad / motion2_v2 / motion3_v2 columns;
 *   # expected per-feature delta < 1e-4.
 */

#include <errno.h>
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
#define NUM_FRAMES 2u

/* Tolerance matching ADR-0214 cross-backend gate (places=4 -> 1e-4). */
#define PARITY_TOL 1e-4

/* motion_v2 emits the SAD score every frame, and the motion2_v2 +
 * motion3_v2 post-process columns for every index (mid-stream from
 * index 1). All three are asserted at frame index 1. */
static const char *const MOTION_V2_FEATURES[] = {
    "VMAF_integer_feature_motion_v2_sad_score",
    "VMAF_integer_feature_motion2_v2_score",
    "VMAF_integer_feature_motion3_v2_score",
};
#define NUM_MOTION_V2_FEATURES 3u

/* Fill a YUV420P 8-bpc picture with a deterministic pattern so that
 * frame 0 and frame 1 differ, causing a non-zero motion score. */
static int fill_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 17u) & 0xFFu);
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

/* CPU path — feed NUM_FRAMES frames + EOS through "motion_v2". */
static char *run_cpu(double scores_out[NUM_MOTION_V2_FEATURES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "motion_v2", NULL);
    mu_assert("CPU: vmaf_use_feature(motion_v2) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_fixture(&ref, i);
        mu_assert("CPU: fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i);
        mu_assert("CPU: fill_fixture(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, MOTION_V2_FEATURES[k], &scores_out[k], 1u);
        mu_assert("CPU: vmaf_feature_score_at_index(motion_v2, idx=1) failed", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

/* Per-frame submit helper.  Returns:
 *   * NULL on success;
 *   * a non-NULL message string on a hard failure (caller bubbles up);
 *   * NULL with *enosys_skip=1 when the HIP extractor's submit/init hits
 *     the `#ifndef HAVE_HIPCC` scaffold path and returns -ENOSYS.
 * Extracted to keep run_hip() under the fork's
 * readability-function-size.LineThreshold=60 budget. */
static char *hip_submit_one_frame(VmafContext *vmaf, unsigned i, int *enosys_skip)
{
    *enosys_skip = 0;
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_fixture(&ref, i);
    mu_assert("HIP: fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, i);
    mu_assert("HIP: fill_fixture(dist) failed", !err);

    err = vmaf_read_pictures(vmaf, &ref, &dist, i);
    /* The HIP `motion_v2_hip` extractor's init/submit return -ENOSYS when
     * libvmaf was built with `enable_hipcc=false` — the HSACO device
     * kernels are not embedded in the .so, so dispatch hits the `#ifndef
     * HAVE_HIPCC` scaffold path. vmaf_read_pictures unrefs the caller
     * pictures internally on every code path; the caller tears down the
     * partially-initialised VmafContext on a skip. */
    if (err == -ENOSYS) {
        *enosys_skip = 1;
        return NULL;
    }
    mu_assert("HIP: vmaf_read_pictures failed", !err);
    return NULL;
}

/* HIP path — feed NUM_FRAMES frames + EOS through "motion_v2_hip".
 * Leaves scores_out[*] = NaN as the skip sentinel when no HIP device is
 * present or the HSACO kernels were not built. */
static char *run_hip(double scores_out[NUM_MOTION_V2_FEATURES])
{
    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++)
        scores_out[k] = NAN;

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

    err = vmaf_use_feature(vmaf, "motion_v2_hip", NULL);
    mu_assert("HIP: vmaf_use_feature(motion_v2_hip) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        int enosys_skip = 0;
        char *msg = hip_submit_one_frame(vmaf, i, &enosys_skip);
        if (msg)
            return msg;
        if (enosys_skip) {
            (void)fprintf(stderr, "[skip: HIP kernels not built (enable_hipcc=false)] ");
            (void)vmaf_close(vmaf);
            vmaf_hip_state_free(&hip_state);
            return NULL;
        }
    }

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, MOTION_V2_FEATURES[k], &scores_out[k], 1u);
        mu_assert("HIP: vmaf_feature_score_at_index(motion_v2, idx=1) failed", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);

    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_motion_v2_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2_hip");
    mu_assert("motion_v2_hip extractor must be registered", fex != NULL);
    mu_assert("motion_v2_hip name matches", !strcmp(fex->name, "motion_v2_hip"));

    /* motion3_v2 must be advertised in provided_features[] so the feature
     * collector materialises the column (ADR-1108). */
    int found_motion3 = 0;
    for (const char *const *pf = fex->provided_features; pf && *pf; pf++) {
        if (!strcmp(*pf, "VMAF_integer_feature_motion3_v2_score"))
            found_motion3 = 1;
    }
    mu_assert("motion_v2_hip must provide motion3_v2_score", found_motion3 == 1);
    return NULL;
}

static char *test_motion_v2_cpu_hip_parity(void)
{
    double cpu[NUM_MOTION_V2_FEATURES] = {0.0, 0.0, 0.0};
    double gpu[NUM_MOTION_V2_FEATURES] = {NAN, NAN, NAN};

    char *msg = run_cpu(cpu);
    if (msg)
        return msg;
    msg = run_hip(gpu);
    if (msg)
        return msg;

    /* No HIP device / kernels-not-built — gpu[*] left NaN, skip cleanly. */
    if (isnan(gpu[0]))
        return NULL;

    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        mu_assert("motion_v2 HIP score must be finite", isfinite(gpu[k]));
        const double delta = fabs(cpu[k] - gpu[k]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nmotion_v2 parity FAIL (%s): cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                          MOTION_V2_FEATURES[k], cpu[k], gpu[k], delta, PARITY_TOL);
        }
        mu_assert("motion_v2 CPU vs. HIP delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_v2_hip_registered);
    mu_run_test(test_motion_v2_cpu_hip_parity);
    return NULL;
}
