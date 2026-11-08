/**
 *
 *  Copyright 2026-present VMAFx contributors
 *
 *  Licensed under the EUPL, Version 1.2 or – as soon they will be approved by
 *  the European Commission - subsequent versions of the EUPL (the "Licence");
 *  You may not use this work except in compliance with the Licence.
 *  You may obtain a copy of the Licence at:
 *
 *      https://joinup.ec.europa.eu/software/page/eupl
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the Licence is distributed on an "AS IS" basis,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the Licence for the specific language governing permissions and
 *  limitations under the Licence.
 *
 *  SPDX-License-Identifier: EUPL-1.2
 */

/* Regression gate for #3356: SYCL temporal motion paired with non-temporal
 * SYCL vif under --subsample N (N > 1).
 *
 * Reuses mft_fill_picture() from motion_five_frame_twin_parity.h and
 * vmaf_test_identical_f64() from test.h. Exits 77 when no SYCL device is
 * visible.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

#include "motion_five_frame_twin_parity.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; see ADR-1138. */

#define SUBSAMPLE_MAX_FRAMES 9u
#define SUBSAMPLE_KEY_COUNT 9u

static const char *const SUBSAMPLE_KEYS[] = {
    "VMAF_integer_feature_motion_sad_score",
    "VMAF_integer_feature_motion_score",
    "VMAF_integer_feature_motion2_score",
    "VMAF_integer_feature_motion3_score",
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
    "integer_vif",
};
_Static_assert(sizeof(SUBSAMPLE_KEYS) / sizeof(SUBSAMPLE_KEYS[0]) == SUBSAMPLE_KEY_COUNT,
               "SUBSAMPLE_KEYS must list SUBSAMPLE_KEY_COUNT keys");

static int use_debug_feature(VmafContext *vmaf, const char *name)
{
    VmafFeatureDictionary *opts = NULL;
    int err = vmaf_feature_dictionary_set(&opts, "debug", "true");
    if (err)
        return err;
    err = vmaf_use_feature(vmaf, name, opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    return err;
}

static int feed_distinct_pair(VmafContext *vmaf, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = mft_fill_picture(&ref, 8u, frame);
    if (err)
        return err;
    err = mft_fill_picture(&dist, 8u, frame + 1u);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

static int init_run_context(VmafContext **vmaf, VmafSyclState *state, unsigned n_subsample)
{
    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_subsample = n_subsample,
    };
    int err = vmaf_init(vmaf, cfg);
    if (err)
        return err;
    if (state)
        err = vmaf_sycl_import_state(*vmaf, state);
    if (!err)
        err = use_debug_feature(*vmaf, state ? "motion_sycl" : "motion");
    if (!err)
        err = use_debug_feature(*vmaf, state ? "vif_sycl" : "vif");
    return err;
}

static int collect_scored_frames(VmafContext *vmaf, unsigned frames, unsigned n_subsample,
                                 double out[SUBSAMPLE_MAX_FRAMES][SUBSAMPLE_KEY_COUNT])
{
    const unsigned step = n_subsample > 1u ? n_subsample : 1u;
    for (unsigned frame = 0; frame < frames; frame++) {
        if ((frame % step) != 0u) {
            double unexpected = 0.0;
            if (vmaf_feature_score_at_index(vmaf, "integer_vif", &unexpected, frame) == 0)
                return -1;
            continue;
        }
        for (size_t k = 0; k < SUBSAMPLE_KEY_COUNT; k++) {
            const int err =
                vmaf_feature_score_at_index(vmaf, SUBSAMPLE_KEYS[k], &out[frame][k], frame);
            if (err)
                return err;
        }
    }
    return 0;
}

static int run_motion_vif(VmafSyclState *state, unsigned frames, unsigned n_subsample,
                          double out[SUBSAMPLE_MAX_FRAMES][SUBSAMPLE_KEY_COUNT])
{
    VmafContext *vmaf = NULL;
    int err = init_run_context(&vmaf, state, n_subsample);
    for (unsigned frame = 0; !err && frame < frames; frame++)
        err = feed_distinct_pair(vmaf, frame);
    if (!err)
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (!err)
        err = collect_scored_frames(vmaf, frames, n_subsample, out);
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : close_err;
}

static unsigned compare_scored_frames(unsigned frames, unsigned n_subsample,
                                      const double cpu[SUBSAMPLE_MAX_FRAMES][SUBSAMPLE_KEY_COUNT],
                                      const double gpu[SUBSAMPLE_MAX_FRAMES][SUBSAMPLE_KEY_COUNT])
{
    unsigned mismatches = 0u;
    const unsigned step = n_subsample > 1u ? n_subsample : 1u;
    for (unsigned frame = 0; frame < frames; frame += step) {
        for (size_t k = 0; k < SUBSAMPLE_KEY_COUNT; k++) {
            if (isfinite(cpu[frame][k]) && vmaf_test_identical_f64(cpu[frame][k], gpu[frame][k]))
                continue;
            (void)fprintf(stderr,
                          "\nsubsample=%u frames=%u frame %u %s:\n"
                          "  CPU  %.17g\n"
                          "  SYCL %.17g\n",
                          n_subsample, frames, frame, SUBSAMPLE_KEYS[k], cpu[frame][k],
                          gpu[frame][k]);
            mismatches++;
        }
    }
    return mismatches;
}

static char *check_subsample_case(unsigned frames, unsigned n_subsample)
{
    VmafSyclState *state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&state, sycl_cfg) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        mu_skipped = 1;
        return NULL;
    }

    double cpu[SUBSAMPLE_MAX_FRAMES][SUBSAMPLE_KEY_COUNT] = {{0}};
    double gpu[SUBSAMPLE_MAX_FRAMES][SUBSAMPLE_KEY_COUNT] = {{0}};
    const int gpu_err = run_motion_vif(state, frames, n_subsample, gpu);
    vmaf_sycl_state_free(&state);
    mu_assert("SYCL motion+vif subsample run failed", !gpu_err);

    const int cpu_err = run_motion_vif(NULL, frames, n_subsample, cpu);
    mu_assert("CPU motion+vif subsample run failed", !cpu_err);

    const unsigned mismatches = compare_scored_frames(frames, n_subsample, cpu, gpu);
    mu_assert("SYCL motion+vif subsample scores differ from CPU", mismatches == 0u);
    return NULL;
}

static char *test_subsample_three_motion_and_vif(void)
{
    return check_subsample_case(9u, 3u);
}

static char *test_subsample_one_boundary(void)
{
    if (mu_skipped)
        return NULL;
    return check_subsample_case(7u, 1u);
}

static char *test_subsample_exceeds_frame_count_boundary(void)
{
    if (mu_skipped)
        return NULL;
    return check_subsample_case(7u, 12u);
}

char *run_tests(void)
{
    mu_run_test(test_subsample_three_motion_and_vif);
    mu_run_test(test_subsample_one_boundary);
    mu_run_test(test_subsample_exceeds_frame_count_boundary);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
