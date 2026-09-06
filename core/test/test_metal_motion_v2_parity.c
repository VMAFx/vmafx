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
 * Metal kernel coverage round 2 — motion_v2 CPU vs. Metal parity
 * (ADR-0421 first-kernel close-out; ADR-0214 cross-backend gate).
 *
 * PR #351 added a registration audit covering all 8 Metal extractors. This
 * test takes the next step: it runs the same two-frame synthetic fixture
 * through the CPU `motion_v2` extractor and the Metal `motion_v2_metal`
 * extractor and asserts that
 *
 *     VMAF_integer_feature_motion_v2_sad_score
 *     VMAF_integer_feature_motion2_v2_score
 *     VMAF_integer_feature_motion3_v2_score
 *
 * at frame index 1 match within the places=4 (1e-4) tolerance from
 * ADR-0214. The Metal kernel is documented as bit-exact-when-fps-weight=1.0
 * in `integer_motion_v2_metal.mm` (`flush_fex_metal` comment), so the gate
 * stays at the same 1e-4 the SYCL parity twin uses.
 *
 * The motion3_v2 column was added to the Metal twin in ADR-1108 (mirroring
 * the CUDA twin landed in #909); before that the Metal extractor never
 * emitted it, so the score lookup would have failed. The test therefore
 * also asserts motion3_v2 is finite on both paths.
 *
 * Skip behaviour: when `vmaf_metal_state_init` returns -ENODEV (Linux,
 * Windows, Intel Mac), the test sets `mu_skipped = 1`, emits
 * "[skip: no Metal device]" to stderr, and exits 77 (meson skip).
 * On Apple Silicon with a Metal device, it executes and exits 0 on pass.
 *
 * Cross-references:
 *   - core/test/test_cuda_motion_v2_parity.c (motion3_v2 parity, CUDA twin)
 *   - core/test/test_sycl_motion3_parity.c   (same fixture pattern, SYCL twin)
 *   - core/test/test_metal_smoke.c           (registration + lifecycle audit)
 *   - core/src/feature/metal/integer_motion_v2_metal.mm
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"

/* Fixture geometry — large enough for the Metal threadgroup partial-sum
 * grid (32x8 = 256 threads per group), small enough for a fast CI run. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u

/* ADR-0214 cross-backend tolerance (places=4 -> 1e-4). */
#define PARITY_TOL 1e-4

/* motion_v2 emits the SAD score every frame, and the motion2_v2 +
 * motion3_v2 scores host-side in flush() for every collected frame
 * (the motion3_v2 post-process was added to the Metal twin in ADR-1108).
 * All three must match the CPU reference at places=4. */
static const char *const MOTION_V2_FEATURES[] = {
    "VMAF_integer_feature_motion_v2_sad_score",
    "VMAF_integer_feature_motion2_v2_score",
    "VMAF_integer_feature_motion3_v2_score",
};
#define NUM_MOTION_V2_FEATURES 3u

static int fill_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
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

/* ------------------------------------------------------------------ */
/* CPU path — run `motion_v2` for NUM_FRAMES frames, return the SAD,
 * motion2_v2 and motion3_v2 scores at frame index 1.                  */
/* ------------------------------------------------------------------ */
static char *run_cpu_motion_v2(double scores_out[NUM_MOTION_V2_FEATURES])
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "motion_v2", NULL);
    mu_assert("CPU: vmaf_use_feature(motion_v2) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
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

/* ------------------------------------------------------------------ */
/* Metal path — run `motion_v2_metal` for NUM_FRAMES frames, return the
 * SAD, motion2_v2 and motion3_v2 scores at frame index 1. A NaN in
 * scores_out[0] signals "skip" (no Metal device).                     */
/* ------------------------------------------------------------------ */
static char *run_metal_motion_v2(double scores_out[NUM_MOTION_V2_FEATURES])
{
    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++)
        scores_out[k] = NAN;
    int err = 0;

    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        /* No Apple-Family-7+ Metal device — skip cleanly. */
        (void)fprintf(stderr, "[skip: no Metal device] ");
        mu_skipped = 1;
        return NULL;
    }

    (void)fprintf(stdout, "[metal device active: motion_v2 parity run on device]\n");
    (void)fflush(stdout);

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("Metal: vmaf_init failed", !err);

    err = vmaf_metal_import_state(vmaf, mstate);
    mu_assert("Metal: vmaf_metal_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "motion_v2_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(motion_v2_metal) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_fixture(&ref, i);
        mu_assert("Metal: fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i);
        mu_assert("Metal: fill_fixture(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("Metal: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, MOTION_V2_FEATURES[k], &scores_out[k], 1u);
        mu_assert("Metal: vmaf_feature_score_at_index(motion_v2, idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);

    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_motion_v2_metal_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2_metal");
    mu_assert("motion_v2_metal extractor must be registered", fex != NULL);
    mu_assert("motion_v2_metal name matches", !strcmp(fex->name, "motion_v2_metal"));
    return NULL;
}

static char *test_motion_v2_cpu_metal_parity(void)
{
    double cpu[NUM_MOTION_V2_FEATURES] = {0.0, 0.0, 0.0};
    double metal[NUM_MOTION_V2_FEATURES] = {NAN, NAN, NAN};

    char *msg = run_cpu_motion_v2(cpu);
    if (msg)
        return msg;
    msg = run_metal_motion_v2(metal);
    if (msg)
        return msg;

    if (isnan(metal[0]))
        return NULL; /* No Metal device — clean skip. */

    /* motion3_v2 (index 2) must be a real, finite score on the Metal
     * path — guards against the pre-ADR-1108 gap where the Metal twin
     * never emitted it (vmaf_feature_score_at_index would have failed). */
    mu_assert("Metal motion3_v2 must be finite (ADR-1108)", isfinite(metal[2]));
    mu_assert("CPU motion3_v2 must be finite", isfinite(cpu[2]));

    for (unsigned k = 0; k < NUM_MOTION_V2_FEATURES; k++) {
        const double delta = fabs(cpu[k] - metal[k]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nmotion_v2 parity FAIL (%s): cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                          MOTION_V2_FEATURES[k], cpu[k], metal[k], delta, PARITY_TOL);
        }
        mu_assert("motion_v2 CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_v2_metal_registered);
    mu_run_test(test_motion_v2_cpu_metal_parity);
    return NULL;
}
