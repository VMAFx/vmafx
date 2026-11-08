/**
 *
 *  Copyright 2026 Lusoris
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

/* Regression gate for #3356: SYCL temporal combined-graph extractors paired
 * with non-temporal ones under --subsample N (N > 1).
 *
 * Temporal keys (motion, psnr) must be scored on every frame; non-temporal
 * keys (vif, adm, float_moment, cambi) only on frames where index % N == 0,
 * and must be absent on the others.
 *
 * - motion_sycl + vif_sycl is compared against the CPU twins at the same
 *   subsample factor.
 * - Every other temporal / non-temporal pair, and the all-temporal pair, is
 *   compared against the same SYCL extractors at subsample 1.
 * - The test_sycl_subsample_motion_vif_graph registration forces graph
 *   dispatch (VMAF_SYCL_DISPATCH) and sets VMAFX_TEST_EXPECT_SYCL_GRAPH=1, so
 *   each SYCL run must also have recorded the combined graph and replayed it
 *   on exactly the frames, from the recording frame on, that run every
 *   extractor.
 *
 * Reuses mft_fill_picture() from motion_five_frame_twin_parity.h and
 * vmaf_test_identical_f64() from test.h. Exits 77 when no SYCL device is
 * visible.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

#include "gpu_dispatch_env.h"
#include "sycl/common.h"

#include "motion_five_frame_twin_parity.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; see ADR-1138. */

#define SUBSAMPLE_MAX_FRAMES 9u
#define SUBSAMPLE_MAX_KEYS 9u
#define SUBSAMPLE_MAX_FEATURES 2u
#define SUBSAMPLE_FACTOR 3u

/* One score key and whether libvmaf runs its extractor on every frame
 * under --subsample (TEMPORAL or PREV_REF flag). */
typedef struct {
    const char *name;
    bool temporal;
} SubsampleKey;

/* One extractor to register: its SYCL name and, for the CPU parity case,
 * its CPU twin (NULL in the SYCL-only cases). */
typedef struct {
    const char *sycl;
    const char *cpu;
} SubsampleExtractor;

/* Extractors to register, whether to set debug=true on them, and the
 * keys to compare. */
typedef struct {
    SubsampleExtractor extractors[SUBSAMPLE_MAX_FEATURES];
    bool debug;
    size_t n_keys;
    SubsampleKey keys[SUBSAMPLE_MAX_KEYS];
} SubsampleCase;

typedef double SubsampleScores[SUBSAMPLE_MAX_FRAMES][SUBSAMPLE_MAX_KEYS];

#define KEY_MOTION2 {"VMAF_integer_feature_motion2_score", true}
#define KEY_PSNR_Y {"psnr_y", true}
#define KEY_VIF0 {"VMAF_integer_feature_vif_scale0_score", false}
#define KEY_ADM2 {"VMAF_integer_feature_adm2_score", false}
#define KEY_MOMENT {"float_moment_ref1st", false}
#define KEY_CAMBI {"Cambi_feature_cambi_score", false}

static const SubsampleCase MOTION_VIF_CPU_CASE = {
    .extractors = {{.sycl = "motion_sycl", .cpu = "motion"}, {.sycl = "vif_sycl", .cpu = "vif"}},
    .debug = true,
    .n_keys = 9u,
    .keys =
        {
            {"VMAF_integer_feature_motion_sad_score", true},
            {"VMAF_integer_feature_motion_score", true},
            KEY_MOTION2,
            {"VMAF_integer_feature_motion3_score", true},
            KEY_VIF0,
            {"VMAF_integer_feature_vif_scale1_score", false},
            {"VMAF_integer_feature_vif_scale2_score", false},
            {"VMAF_integer_feature_vif_scale3_score", false},
            {"integer_vif", false},
        },
};

/* A SYCL-only case: two extractors, one key from each. */
#define SCHEDULE_CASE(a, b, key_a, key_b)                                                          \
    {                                                                                              \
        .extractors = {{.sycl = (a)}, {.sycl = (b)}}, .n_keys = 2u, .keys = {key_a, key_b}         \
    }

static const SubsampleCase SCHEDULE_CASES[] = {
    SCHEDULE_CASE("motion_sycl", "adm_sycl", KEY_MOTION2, KEY_ADM2),
    SCHEDULE_CASE("motion_sycl", "float_moment_sycl", KEY_MOTION2, KEY_MOMENT),
    SCHEDULE_CASE("motion_sycl", "cambi_sycl", KEY_MOTION2, KEY_CAMBI),
    SCHEDULE_CASE("psnr_sycl", "vif_sycl", KEY_PSNR_Y, KEY_VIF0),
    SCHEDULE_CASE("psnr_sycl", "adm_sycl", KEY_PSNR_Y, KEY_ADM2),
    SCHEDULE_CASE("psnr_sycl", "float_moment_sycl", KEY_PSNR_Y, KEY_MOMENT),
    SCHEDULE_CASE("psnr_sycl", "cambi_sycl", KEY_PSNR_Y, KEY_CAMBI),
    SCHEDULE_CASE("motion_sycl", "psnr_sycl", KEY_MOTION2, KEY_PSNR_Y),
};
#define SCHEDULE_CASE_COUNT (sizeof(SCHEDULE_CASES) / sizeof(SCHEDULE_CASES[0]))

/* True when libvmaf writes `key` for `frame` under `n_subsample`. */
static bool key_scored_at(const SubsampleKey *key, unsigned frame, unsigned n_subsample)
{
    return key->temporal || n_subsample <= 1u || (frame % n_subsample) == 0u;
}

/* True when this registration forces graph dispatch and expects the
 * combined graph to be recorded. Read through the once-only environment
 * snapshot (ADR-0488), not getenv. */
static bool expect_graph_recorded(void)
{
    const char *const value = vmaf_gpu_dispatch_env_get("VMAFX_TEST_EXPECT_SYCL_GRAPH");
    return value != NULL && value[0] == '1';
}

/* Register `name`, with debug=true when `debug` is set. */
static int use_feature(VmafContext *vmaf, const char *name, bool debug)
{
    VmafFeatureDictionary *opts = NULL;
    if (debug) {
        const int err = vmaf_feature_dictionary_set(&opts, "debug", "true");
        if (err) {
            return err;
        }
    }
    const int err = vmaf_use_feature(vmaf, name, opts);
    if (err && opts) {
        (void)vmaf_feature_dictionary_free(&opts);
    }
    return err;
}

/* Feed frame `frame` as reference and frame `frame + 1` as distorted, so
 * every full-reference key sees a non-trivial pair. */
static int feed_distinct_pair(VmafContext *vmaf, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = mft_fill_picture(&ref, 8u, frame);
    if (err) {
        return err;
    }
    err = mft_fill_picture(&dist, 8u, frame + 1u);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

/* Open a context at `n_subsample`; with `state` it runs the SYCL
 * extractors of `c`, without it the CPU twins. */
static int init_run_context(VmafContext **vmaf, VmafSyclState *state, const SubsampleCase *c,
                            unsigned n_subsample)
{
    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_subsample = n_subsample,
    };
    int err = vmaf_init(vmaf, cfg);
    if (err) {
        return err;
    }
    if (state) {
        err = vmaf_sycl_import_state(*vmaf, state);
    }
    for (size_t i = 0; !err && i < SUBSAMPLE_MAX_FEATURES; i++) {
        const SubsampleExtractor *x = &c->extractors[i];
        const char *name = state ? x->sycl : x->cpu;
        if (name != NULL) {
            err = use_feature(*vmaf, name, c->debug);
        }
    }
    return err;
}

/* Read every key of `c` for `frame`: a key libvmaf scores there must be
 * present, a key of a skipped non-temporal extractor must be absent. */
static int collect_frame(VmafContext *vmaf, const SubsampleCase *c, unsigned frame,
                         unsigned n_subsample, double row[SUBSAMPLE_MAX_KEYS])
{
    for (size_t k = 0; k < c->n_keys; k++) {
        const SubsampleKey *key = &c->keys[k];
        const bool scored = key_scored_at(key, frame, n_subsample);
        const int err = vmaf_feature_score_at_index(vmaf, key->name, &row[k], frame);
        if (scored == (err == 0)) {
            continue;
        }
        (void)fprintf(stderr, "\nsubsample=%u frame %u %s: %s\n", n_subsample, frame, key->name,
                      scored ? "missing" : "scored on a skipped frame");
        return -1;
    }
    return 0;
}

/* Collect `frames` frames of scores of `c` into `out`. */
static int collect_scored_frames(VmafContext *vmaf, const SubsampleCase *c, unsigned frames,
                                 unsigned n_subsample, SubsampleScores out)
{
    for (unsigned frame = 0; frame < frames; frame++) {
        const int err = collect_frame(vmaf, c, frame, n_subsample, out[frame]);
        if (err) {
            return err;
        }
    }
    return 0;
}

/* Number of frames in [first, frames) on which every key of `c` is scored,
 * i.e. on which every SYCL extractor of the case runs. All of them register
 * with the combined graph, so these are the only frames it may replay on. */
static unsigned all_active_frames(const SubsampleCase *c, unsigned first, unsigned frames,
                                  unsigned n_subsample)
{
    unsigned count = 0u;
    for (unsigned frame = first; frame < frames; frame++) {
        bool all_scored = true;
        for (size_t k = 0; k < c->n_keys; k++) {
            all_scored = all_scored && key_scored_at(&c->keys[k], frame, n_subsample);
        }
        count += all_scored ? 1u : 0u;
    }
    return count;
}

/* The frame index whose submit records the combined graph: the upload makes
 * frame_counter 2 on frame 1, and sycl_any_extractor_wants_graph() records
 * there (core/src/sycl/common.cpp). */
#define GRAPH_RECORD_FRAME 1u

/* Under forced graph dispatch, a SYCL run of two or more frames must have
 * recorded the combined graph and replayed it on every frame from the
 * recording frame on where every registered extractor runs, and on no other
 * frame; those submit directly. */
static int check_graph_recorded(const SubsampleCase *c, VmafSyclState *state, unsigned frames,
                                unsigned n_subsample)
{
    if (state == NULL || frames < 2u || !expect_graph_recorded()) {
        return 0;
    }
    if (vmaf_sycl_graphs_recorded(state) != 1) {
        (void)fprintf(stderr, "\ncombined graph not recorded under forced graph dispatch\n");
        return -1;
    }
    const uint64_t replays = vmaf_sycl_graph_replays(state);
    const unsigned expected = all_active_frames(c, GRAPH_RECORD_FRAME, frames, n_subsample);
    if (replays == expected) {
        return 0;
    }
    (void)fprintf(stderr,
                  "\nsubsample=%u frames=%u: combined graph replayed %llu times, expected %u "
                  "(frames from %u on that run every extractor)\n",
                  n_subsample, frames, (unsigned long long)replays, expected, GRAPH_RECORD_FRAME);
    return -1;
}

/* Run `c` over `frames` frames at `n_subsample` and collect its scores. */
static int run_case(const SubsampleCase *c, VmafSyclState *state, unsigned frames,
                    unsigned n_subsample, SubsampleScores out)
{
    VmafContext *vmaf = NULL;
    int err = init_run_context(&vmaf, state, c, n_subsample);
    for (unsigned frame = 0; !err && frame < frames; frame++) {
        err = feed_distinct_pair(vmaf, frame);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    if (!err) {
        err = collect_scored_frames(vmaf, c, frames, n_subsample, out);
    }
    if (!err) {
        err = check_graph_recorded(c, state, frames, n_subsample);
    }
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : close_err;
}

/* run_case() on a fresh SYCL state. */
static int run_sycl_case(const SubsampleCase *c, unsigned frames, unsigned n_subsample,
                         SubsampleScores out)
{
    VmafSyclState *state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&state, sycl_cfg) != 0 || state == NULL) {
        return -1;
    }
    const int err = run_case(c, state, frames, n_subsample, out);
    vmaf_sycl_state_free(&state);
    return err;
}

/* Count keys of `c` that differ between `want` and `got` on frames where
 * the subsampled run scores them. */
static unsigned compare_scored_frames(const SubsampleCase *c, unsigned frames, unsigned n_subsample,
                                      const SubsampleScores want, const SubsampleScores got,
                                      const char *want_label)
{
    unsigned mismatches = 0u;
    for (unsigned frame = 0; frame < frames; frame++) {
        for (size_t k = 0; k < c->n_keys; k++) {
            if (!key_scored_at(&c->keys[k], frame, n_subsample)) {
                continue;
            }
            if (isfinite(want[frame][k]) &&
                vmaf_test_identical_f64(want[frame][k], got[frame][k])) {
                continue;
            }
            (void)fprintf(stderr,
                          "\nsubsample=%u frames=%u frame %u %s:\n  %s %.17g\n  SYCL %.17g\n",
                          n_subsample, frames, frame, c->keys[k].name, want_label, want[frame][k],
                          got[frame][k]);
            mismatches++;
        }
    }
    return mismatches;
}

/* True, and the run marked skipped, when no SYCL device is visible. */
static bool sycl_device_missing(void)
{
    VmafSyclState *state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&state, sycl_cfg) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        mu_skipped = 1;
        return true;
    }
    vmaf_sycl_state_free(&state);
    return false;
}

/* motion_sycl + vif_sycl against the CPU twins at the same subsample. */
static char *check_cpu_parity(unsigned frames, unsigned n_subsample)
{
    if (mu_skipped || sycl_device_missing()) {
        return NULL;
    }
    SubsampleScores cpu = {{0}};
    SubsampleScores gpu = {{0}};
    const int gpu_err = run_sycl_case(&MOTION_VIF_CPU_CASE, frames, n_subsample, gpu);
    mu_assert("SYCL motion+vif subsample run failed", !gpu_err);
    const int cpu_err = run_case(&MOTION_VIF_CPU_CASE, NULL, frames, n_subsample, cpu);
    mu_assert("CPU motion+vif subsample run failed", !cpu_err);
    const unsigned mismatches =
        compare_scored_frames(&MOTION_VIF_CPU_CASE, frames, n_subsample, cpu, gpu, "CPU ");
    mu_assert("SYCL motion+vif subsample scores differ from CPU", mismatches == 0u);
    return NULL;
}

/* One SYCL pair at SUBSAMPLE_FACTOR against itself at subsample 1. */
static unsigned check_schedule_case(const SubsampleCase *c)
{
    SubsampleScores full = {{0}};
    SubsampleScores sub = {{0}};
    if (run_sycl_case(c, SUBSAMPLE_MAX_FRAMES, 1u, full) != 0 ||
        run_sycl_case(c, SUBSAMPLE_MAX_FRAMES, SUBSAMPLE_FACTOR, sub) != 0) {
        (void)fprintf(stderr, "\n%s + %s: SYCL run failed\n", c->extractors[0].sycl,
                      c->extractors[1].sycl);
        return 1u;
    }
    return compare_scored_frames(c, SUBSAMPLE_MAX_FRAMES, SUBSAMPLE_FACTOR, full, sub,
                                 "SYCL subsample=1");
}

/* Verify parity of SYCL motion and VIF extractors against the CPU reference under
 * subsample 3 across 9 frames. Returns NULL on success or an assertion failure string. */
static char *test_subsample_three_motion_and_vif(void)
{
    return check_cpu_parity(SUBSAMPLE_MAX_FRAMES, SUBSAMPLE_FACTOR);
}

/* Verify the subsample 1 boundary condition against the CPU reference across 7 frames,
 * where every frame is scored without subsample skipping. Returns NULL on success or
 * an assertion failure string. */
static char *test_subsample_one_boundary(void)
{
    return check_cpu_parity(7u, 1u);
}

/* Verify the boundary condition where subsample 12 exceeds the total frame count of 7
 * against the CPU reference. Returns NULL on success or an assertion failure string. */
static char *test_subsample_exceeds_frame_count_boundary(void)
{
    return check_cpu_parity(7u, 12u);
}

/* Verify the schedule matrix of temporal and non-temporal SYCL extractor pairs at
 * subsample 3 against identical SYCL extractors evaluated at subsample 1. Returns NULL
 * on success or an assertion failure string. */
static char *test_subsample_schedule_matrix(void)
{
    if (mu_skipped || sycl_device_missing()) {
        return NULL;
    }
    unsigned failed = 0u;
    for (size_t i = 0; i < SCHEDULE_CASE_COUNT; i++) {
        failed += check_schedule_case(&SCHEDULE_CASES[i]);
    }
    mu_assert("SYCL subsampled pairs differ from their subsample 1 run", failed == 0u);
    return NULL;
}

/* Execute all SYCL subsampling unit test cases in sequence. Returns NULL on success
 * or the first failing assertion message string. */
char *run_tests(void)
{
    mu_run_test(test_subsample_three_motion_and_vif);
    mu_run_test(test_subsample_one_boundary);
    mu_run_test(test_subsample_exceeds_frame_count_boundary);
    mu_run_test(test_subsample_schedule_matrix);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
