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
 * SYCL kernel coverage round 2 — motion_v2 CPU vs. SYCL parity test
 * (ADR-0884), extended for the motion3_v2 host-side post-process
 * (ADR-1108).
 *
 * Motion-v2 (Netflix's SAD-based motion energy refinement, ADR-0349
 * lineage) is computed by integer_motion_v2.c (CPU scalar) and by
 * integer_motion_v2_sycl.cpp (SYCL kernel). It is a separate kernel
 * from the classic motion / motion2 / motion3 stack (already covered
 * by test_sycl_motion3_parity.c) — motion_v2 emits its own
 * sad/motion2_v2/motion3_v2 score triple via a different reduction
 * topology.
 *
 * motion_v2 emits the SAD score every frame, and the motion2_v2 +
 * motion3_v2 scores host-side in flush() for every collected frame
 * (the motion3_v2 post-process was added to the SYCL twin in ADR-1108,
 * mirroring the CUDA twin landed in #909). All three must match the CPU
 * reference at frame index 1 (the first frame where the temporal blend
 * has a previous-frame reference) at places=4.
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

/* Use a slightly larger fixture than the static-image parity tests
 * so the motion SAD has enough block-aligned area for the SYCL
 * tile-aligned reduction to converge with the CPU scalar path. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u
#define PARITY_TOL 1e-4

/* The full motion_v2 feature triple — SAD, motion2_v2, and the
 * host-side motion3_v2 post-process (ADR-1108). All three are checked
 * for CPU-vs-SYCL parity at frame index 1. */
static const char *const MOTION_V2_FEATURES[] = {
    "VMAF_integer_feature_motion_v2_sad_score",
    "VMAF_integer_feature_motion2_v2_score",
    "VMAF_integer_feature_motion3_v2_score",
};
#define NUM_MOTION_V2_FEATURES 3u

static int fill_pic(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Frame-dependent offset → non-zero motion. */
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 13u) & 0xFFu);
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

static char *run_cpu_motion_v2(double scores_out[NUM_MOTION_V2_FEATURES])
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
        err = fill_pic(&ref, i);
        mu_assert("CPU: fill_pic(ref) failed", !err);
        err = fill_pic(&dist, i);
        mu_assert("CPU: fill_pic(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, MOTION_V2_FEATURES[k], &scores_out[k], 1u);
        mu_assert("CPU: motion_v2 feature score at idx=1 missing", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl_motion_v2(double scores_out[NUM_MOTION_V2_FEATURES])
{
    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++)
        scores_out[k] = NAN;
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
    err = vmaf_use_feature(vmaf, "motion_v2_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(motion_v2_sycl) failed", !err);
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_pic(&ref, i);
        mu_assert("SYCL: fill_pic(ref) failed", !err);
        err = fill_pic(&dist, i);
        mu_assert("SYCL: fill_pic(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("SYCL: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, MOTION_V2_FEATURES[k], &scores_out[k], 1u);
        mu_assert("SYCL: motion_v2 feature score at idx=1 missing", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_motion_v2_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2_sycl");
    mu_assert("motion_v2_sycl extractor must be registered", fex != NULL);
    mu_assert("motion_v2_sycl name matches", !strcmp(fex->name, "motion_v2_sycl"));
    return NULL;
}

static char *test_motion_v2_cpu_sycl_parity(void)
{
    double cpu[NUM_MOTION_V2_FEATURES] = {0.0, 0.0, 0.0};
    double gpu[NUM_MOTION_V2_FEATURES] = {NAN, NAN, NAN};

    char *msg = run_cpu_motion_v2(cpu);
    if (msg)
        return msg;
    msg = run_sycl_motion_v2(gpu);
    if (msg)
        return msg;
    if (isnan(gpu[0]))
        return NULL;

    /* motion3_v2 (index 2) must be a real, finite score on the SYCL
     * path — guards against the pre-ADR-1108 gap where the SYCL twin
     * never emitted it (vmaf_feature_score_at_index would have failed). */
    mu_assert("SYCL motion3_v2 must be finite (ADR-1108)", isfinite(gpu[2]));
    mu_assert("CPU motion3_v2 must be finite", isfinite(cpu[2]));

    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nmotion_v2 parity FAIL (%s): cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                          MOTION_V2_FEATURES[k], cpu[k], gpu[k], delta, PARITY_TOL);
        }
        mu_assert("motion_v2 CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_v2_sycl_registered);
    mu_run_test(test_motion_v2_cpu_sycl_parity);
    return NULL;
}
