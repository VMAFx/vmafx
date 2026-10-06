/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * motion2 / motion3 of the integer motion extractors, derived frame by frame
 * (ADR-2090): frame i is final as soon as the SAD scores of frames
 * 0 .. max(i + 1, min_idx) are in, with the value upstream's flush() over the
 * whole stream gives it.
 *
 *   positive  vmaf_motion_window_advance() after every SAD, then
 *             vmaf_motion_window_flush(), equals a transcription of
 *             upstream's flush() (written out a second time below) bit for
 *             bit, for both windows, the moving average, the blend and the
 *             cap, at 0 to 17 frames, with the SAD scores in order and out
 *             of order (worker threads); `motion` and `motion_v2` on a
 *             context, serial and with worker threads, have frame i final
 *             once frame i + 1 is read and before the flush, and
 *             float_motion keeps the same rule
 *   boundary  no frame is final before the SAD of frame min_idx; the last
 *             frame only at the flush; a second flush and an advance after
 *             the flush append nothing
 *   negative  an advance without a state, or a dictionary without the SAD
 *             key, is refused with -EINVAL
 *
 * Planted defect (ADR-2090): an advance that finalises frame i once the SAD
 * of frame i is in, one frame early, fails "final too early" and the value
 * comparison here, and the window test's step check in test_vmafx_window.c.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "float_bits.h"
#include "mu_table.h"
#include "test.h"

#include "dict.h"
#include "feature/feature_collector.h"
#include "feature/motion_blend_tools.h"
#include "feature/motion_window.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define MWI_MAX_FRAMES 17u
#define MWI_W 64u
#define MWI_H 48u
#define MWI_FEED 9u

static const char mwi_sad[] = "mwi_sad";
static const char mwi_motion2[] = "mwi_motion2";
static const char mwi_motion3[] = "mwi_motion3";

/* The options that shape the window. */
typedef struct MwiOpts {
    bool five;
    bool mma;
    double blend_factor;
    double blend_offset;
    double max_val;
} MwiOpts;

static const MwiOpts mwi_opts[] = {
    {false, false, 1.0, 40.0, 10000.0}, {true, false, 1.0, 40.0, 10000.0},
    {false, true, 1.0, 40.0, 10000.0},  {true, true, 1.0, 40.0, 10000.0},
    {false, true, 0.5, 0.2, 0.5},       {true, true, 0.5, 0.2, 0.5},
};

static const unsigned mwi_counts[] = {0u, 1u, 2u, 3u, 4u, 5u, 9u, MWI_MAX_FRAMES};

static unsigned mwi_min_idx(const MwiOpts *o)
{
    return o->five ? 2u : 1u;
}

/* A SAD score as the extractor stores it: 0 below min_idx, otherwise uneven,
 * with ties and values on both sides of the blend offset and the cap. */
static double mwi_sad_score(const MwiOpts *o, unsigned i)
{
    if (i < mwi_min_idx(o))
        return 0.0;
    return (double)((i * 37u) % 11u) * 0.0625 + (double)(i % 3u) * 0.25;
}

/* motion3's processed value: the blend, then the cap. */
static double mwi_processed(const MwiOpts *o, double motion2)
{
    return MIN(motion_blend(motion2, o->blend_factor, o->blend_offset), o->max_val);
}

/* motion2 of frame i >= min_idx of an n-frame stream (upstream flush()). */
static double mwi_motion2_at(const MwiOpts *o, const double *sad, unsigned i, unsigned n)
{
    if (i + 1u >= n)
        return sad[i];
    const unsigned lo = o->five ? i - 1u : i;
    if (lo >= mwi_min_idx(o))
        return sad[lo] < sad[i + 1u] ? sad[lo] : sad[i + 1u];
    return sad[i + 1u];
}

/* integer_motion.c::flush() (Netflix a4a1492d / a2b59b77) over n frames,
 * written out a second time. */
static void mwi_oracle(const MwiOpts *o, const double *sad, unsigned n, double *m2, double *m3)
{
    const unsigned min_idx = mwi_min_idx(o);
    const double stamp = n > min_idx ? mwi_processed(o, sad[min_idx]) : 0.0;
    double prev = 0.0;
    for (unsigned i = 0; i < n; i++) {
        if (i < min_idx) {
            m2[i] = 0.0;
            m3[i] = stamp;
            prev = stamp;
            continue;
        }
        m2[i] = mwi_motion2_at(o, sad, i, n);
        const double processed = mwi_processed(o, m2[i]);
        m3[i] = o->mma ? (processed + prev) / 2.0 : processed;
        prev = processed;
    }
}

static VmafMotionWindow mwi_window(const MwiOpts *o, VmafMotionWindowState *state)
{
    const VmafMotionWindow w = {
        .sad_feature = mwi_sad,
        .motion2_feature = mwi_motion2,
        .motion3_feature = mwi_motion3,
        .motion_blend_factor = o->blend_factor,
        .motion_blend_offset = o->blend_offset,
        .motion_max_val = o->max_val,
        .motion_five_frame_window = o->five,
        .motion_moving_average = o->mma,
        .state = state,
    };
    return w;
}

/* A collector and the dictionary of an extractor that names its keys as they
 * are. Returns 0 or the first error. */
static int mwi_open(VmafFeatureCollector **fc, VmafDictionary **dict)
{
    *fc = NULL;
    *dict = NULL;
    int err = vmaf_feature_collector_init(fc);
    if (!err)
        err = vmaf_dictionary_set(dict, mwi_sad, mwi_sad, 0);
    if (!err)
        err = vmaf_dictionary_set(dict, mwi_motion2, mwi_motion2, 0);
    if (!err)
        err = vmaf_dictionary_set(dict, mwi_motion3, mwi_motion3, 0);
    return err;
}

static void mwi_close(VmafFeatureCollector *fc, VmafDictionary **dict)
{
    vmaf_feature_collector_destroy(fc);
    (void)vmaf_dictionary_free(dict);
}

/* How many frames, from frame 0 on, have motion2 and motion3. */
static unsigned mwi_final_prefix(VmafFeatureCollector *fc, unsigned n)
{
    unsigned i = 0;
    double v = 0.0;
    while (i < n && !vmaf_feature_collector_get_score(fc, mwi_motion2, &v, i) &&
           !vmaf_feature_collector_get_score(fc, mwi_motion3, &v, i))
        i++;
    return i;
}

/* The frames ADR-2090 makes final with the SAD scores of frames 0 .. have - 1
 * in: max(i + 1, min_idx) < have. */
static unsigned mwi_expected_final(const MwiOpts *o, unsigned have)
{
    return have > mwi_min_idx(o) ? have - 1u : 0u;
}

/* Every frame of an n-frame stream carries the oracle's motion2 and motion3. */
static bool mwi_values_match(VmafFeatureCollector *fc, const MwiOpts *o, const double *sad,
                             unsigned n)
{
    double m2[MWI_MAX_FRAMES];
    double m3[MWI_MAX_FRAMES];
    mwi_oracle(o, sad, n, m2, m3);
    bool same = true;
    for (unsigned i = 0; i < n && same; i++) {
        double got2 = 0.0;
        double got3 = 0.0;
        same = !vmaf_feature_collector_get_score(fc, mwi_motion2, &got2, i) &&
               !vmaf_feature_collector_get_score(fc, mwi_motion3, &got3, i) &&
               vmaf_test_identical_f64(got2, m2[i]) && vmaf_test_identical_f64(got3, m3[i]);
        if (!same) {
            (void)fprintf(stderr,
                          "\nframe %u of %u: motion2 %.17g (want %.17g), motion3 %.17g "
                          "(want %.17g)\n",
                          i, n, got2, m2[i], got3, m3[i]);
        }
    }
    return same;
}

/* SAD arrival order: in order, or every block of three reversed, as worker
 * threads may append them. */
static unsigned mwi_arrival(unsigned k, unsigned n, bool shuffled)
{
    if (!shuffled)
        return k;
    const unsigned block = k - (k % 3u);
    const unsigned last = (block + 2u < n) ? block + 2u : n - 1u;
    return last - (k - block);
}

/* How many SAD scores, from frame 0 on, are in after `k + 1` arrivals. */
static unsigned mwi_have(unsigned k, unsigned n, bool shuffled)
{
    if (!shuffled)
        return k + 1u;
    const unsigned block = k - (k % 3u);
    const unsigned last = (block + 2u < n) ? block + 2u : n - 1u;
    return (k == last || k + 1u == n) ? last + 1u : block;
}

/* One stream: append each SAD, advance, check which frames are final, then
 * flush and check every value. */
static char *mwi_stream(const MwiOpts *o, unsigned n, bool shuffled)
{
    VmafFeatureCollector *fc = NULL;
    VmafDictionary *dict = NULL;
    VmafMotionWindowState state = {0};
    const VmafMotionWindow w = mwi_window(o, &state);
    double sad[MWI_MAX_FRAMES];
    int err = mwi_open(&fc, &dict);
    for (unsigned k = 0; k < n && !err; k++) {
        const unsigned i = mwi_arrival(k, n, shuffled);
        sad[i] = mwi_sad_score(o, i);
        err = vmaf_feature_collector_append(fc, mwi_sad, sad[i], i);
        err = err ? err : vmaf_motion_window_advance(fc, dict, &w);
        const unsigned want = mwi_expected_final(o, mwi_have(k, n, shuffled));
        if (!err && mwi_final_prefix(fc, n) != want) {
            mwi_close(fc, &dict);
            return "final too early or too late";
        }
    }
    err = err ? err : vmaf_motion_window_flush(fc, dict, &w);
    const bool same = !err && mwi_values_match(fc, o, sad, n);
    mwi_close(fc, &dict);
    mu_assert("advance / flush failed", !err);
    mu_assert("a value differs from upstream's flush over the whole stream", same);
    return NULL;
}

static char *test_advance_then_flush_equals_upstream_flush(void)
{
    for (size_t o = 0; o < MU_TABLE_LEN(mwi_opts); o++) {
        for (size_t c = 0; c < MU_TABLE_LEN(mwi_counts); c++) {
            mu_assert_msg(mwi_stream(&mwi_opts[o], mwi_counts[c], false));
            mu_assert_msg(mwi_stream(&mwi_opts[o], mwi_counts[c], true));
        }
    }
    return NULL;
}

/* Every SAD first, then one flush without a state: the derivation as it ran
 * before ADR-2090, which the advance must not change. */
static char *test_flush_without_state_equals_upstream_flush(void)
{
    for (size_t o = 0; o < MU_TABLE_LEN(mwi_opts); o++) {
        VmafFeatureCollector *fc = NULL;
        VmafDictionary *dict = NULL;
        const VmafMotionWindow w = mwi_window(&mwi_opts[o], NULL);
        double sad[MWI_MAX_FRAMES];
        int err = mwi_open(&fc, &dict);
        for (unsigned i = 0; i < MWI_MAX_FRAMES && !err; i++) {
            sad[i] = mwi_sad_score(&mwi_opts[o], i);
            err = vmaf_feature_collector_append(fc, mwi_sad, sad[i], i);
        }
        err = err ? err : vmaf_motion_window_flush(fc, dict, &w);
        const bool same = !err && mwi_values_match(fc, &mwi_opts[o], sad, MWI_MAX_FRAMES);
        mwi_close(fc, &dict);
        mu_assert("flush without a state differs from upstream's", same);
    }
    return NULL;
}

/* A second flush (a retried vmaf_read_pictures(NULL, NULL)) and an advance
 * after the flush append nothing: an append to a written frame would fail. */
static char *test_second_flush_and_late_advance_append_nothing(void)
{
    const MwiOpts *o = &mwi_opts[3];
    VmafFeatureCollector *fc = NULL;
    VmafDictionary *dict = NULL;
    VmafMotionWindowState state = {0};
    const VmafMotionWindow w = mwi_window(o, &state);
    int err = mwi_open(&fc, &dict);
    for (unsigned i = 0; i < 6u && !err; i++)
        err = vmaf_feature_collector_append(fc, mwi_sad, mwi_sad_score(o, i), i);
    err = err ? err : vmaf_motion_window_advance(fc, dict, &w);
    const unsigned before_flush = mwi_final_prefix(fc, 6u);
    err = err ? err : vmaf_motion_window_flush(fc, dict, &w);
    const int second = err ? err : vmaf_motion_window_flush(fc, dict, &w);
    const int late = err ? err : vmaf_motion_window_advance(fc, dict, &w);
    const unsigned after = mwi_final_prefix(fc, 7u);
    mwi_close(fc, &dict);
    mu_assert("the last frame was final before the flush", before_flush == 5u);
    mu_assert("first flush", !err);
    mu_assert("the second flush or the late advance appended", !second && !late && after == 6u);
    return NULL;
}

static char *test_refusals(void)
{
    VmafFeatureCollector *fc = NULL;
    VmafDictionary *dict = NULL;
    VmafDictionary *empty = NULL;
    VmafMotionWindowState state = {0};
    const VmafMotionWindow stateless = mwi_window(&mwi_opts[0], NULL);
    const VmafMotionWindow w = mwi_window(&mwi_opts[0], &state);
    int err = mwi_open(&fc, &dict);
    err = err ? err : vmaf_dictionary_set(&empty, "other", "other", 0);
    const int no_state = err ? 0 : vmaf_motion_window_advance(fc, dict, &stateless);
    const int no_key_advance = err ? 0 : vmaf_motion_window_advance(fc, empty, &w);
    const int no_key_flush = err ? 0 : vmaf_motion_window_flush(fc, empty, &w);
    (void)vmaf_dictionary_free(&empty);
    mwi_close(fc, &dict);
    mu_assert("setup", !err);
    mu_assert("an advance without a state was accepted", no_state == -EINVAL);
    mu_assert("a dictionary without the SAD key was accepted",
              no_key_advance == -EINVAL && no_key_flush == -EINVAL);
    return NULL;
}

/* ---- On a context: `motion`, `motion_v2`, `float_motion` ------------------------------------ */

/* One extractor run on a context and the keys it writes. */
typedef struct MwiRun {
    const char *extractor;
    bool five;
    unsigned n_threads;
    const char *sad;     /* NULL for float_motion */
    const char *motion2; /* collector names */
    const char *motion3;
} MwiRun;

static const MwiRun mwi_runs[] = {
    {"motion", false, 0u, "VMAF_integer_feature_motion_sad_score",
     "VMAF_integer_feature_motion2_score", "VMAF_integer_feature_motion3_score"},
    {"motion", true, 0u, "VMAF_integer_feature_motion_sad_score_mffw", "integer_motion2_mffw",
     "integer_motion3_mffw"},
    {"motion_v2", false, 0u, "VMAF_integer_feature_motion_v2_sad_score",
     "VMAF_integer_feature_motion2_v2_score", "VMAF_integer_feature_motion3_v2_score"},
    {"motion_v2", true, 0u, "VMAF_integer_feature_motion_v2_sad_score_mffw",
     "VMAF_integer_feature_motion2_v2_score_mffw", "VMAF_integer_feature_motion3_v2_score_mffw"},
    {"motion", false, 2u, "VMAF_integer_feature_motion_sad_score",
     "VMAF_integer_feature_motion2_score", "VMAF_integer_feature_motion3_score"},
    {"motion", true, 3u, "VMAF_integer_feature_motion_sad_score_mffw", "integer_motion2_mffw",
     "integer_motion3_mffw"},
    {"motion_v2", true, 2u, "VMAF_integer_feature_motion_v2_sad_score_mffw",
     "VMAF_integer_feature_motion2_v2_score_mffw", "VMAF_integer_feature_motion3_v2_score_mffw"},
    {"float_motion", false, 0u, NULL, "VMAF_feature_motion2_score", "VMAF_feature_motion3_score"},
};

/* A luma plane that moves by an uneven amount on every frame. */
static int mwi_picture(VmafPicture *pic, unsigned frame)
{
    static const unsigned shift[MWI_FEED] = {0u, 1u, 9u, 5u, 15u, 16u, 18u, 4u, 11u};
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, MWI_W, MWI_H);
    for (unsigned y = 0; !err && y < pic->h[0]; y++) {
        for (unsigned x = 0; x < pic->w[0]; x++) {
            const unsigned sx = x + shift[frame % MWI_FEED];
            ((uint8_t *)pic->data[0])[(size_t)y * (size_t)pic->stride[0] + x] =
                (uint8_t)((sx * 7u + y * 13u + ((sx * y) >> 3)) & 0xFFu);
        }
    }
    for (unsigned p = 1; !err && p < 3u; p++) {
        for (unsigned y = 0; y < pic->h[p]; y++)
            memset((uint8_t *)pic->data[p] + (size_t)y * (size_t)pic->stride[p], 128, pic->w[p]);
    }
    return err;
}

static int mwi_feed(VmafContext *vmaf, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = mwi_picture(&ref, frame);
    if (err)
        return err;
    err = mwi_picture(&dist, frame);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

static int mwi_context(VmafContext **vmaf, const MwiRun *run)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = run->n_threads};
    VmafFeatureDictionary *opts = NULL;
    int err = vmaf_init(vmaf, cfg);
    if (!err && run->five)
        err = vmaf_feature_dictionary_set(&opts, "motion_five_frame_window", "true");
    if (err) {
        (void)vmaf_feature_dictionary_free(&opts);
        return err;
    }
    return vmaf_use_feature(*vmaf, run->extractor, opts);
}

/* Whether frame i has motion2 and motion3 (a fenced read). */
static bool mwi_readable(VmafContext *vmaf, const MwiRun *run, unsigned i, double *m2, double *m3)
{
    return !vmaf_feature_score_at_index(vmaf, run->motion2, m2, i) &&
           !vmaf_feature_score_at_index(vmaf, run->motion3, m3, i);
}

/* After frame k is read: frames with max(i + 1, min_idx) <= k are final, the
 * others are not. Records the values read. */
static bool mwi_check_after(VmafContext *vmaf, const MwiRun *run, unsigned k, double *m2,
                            double *m3)
{
    const unsigned min_idx = run->five ? 2u : 1u;
    bool ok = true;
    for (unsigned i = 0; i <= k && ok; i++) {
        const unsigned needed = (i + 1u > min_idx) ? i + 1u : min_idx;
        ok = mwi_readable(vmaf, run, i, &m2[i], &m3[i]) == (needed <= k);
        if (!ok) {
            (void)fprintf(stderr, "\n%s (five %d, %u threads): frame %u after frame %u: %s\n",
                          run->extractor, run->five, run->n_threads, i, k,
                          needed <= k ? "not final" : "final too early");
        }
    }
    return ok;
}

/* After the flush every frame reads the value it had when it became final,
 * and the integer extractors' values are upstream's flush() over their SADs. */
static bool mwi_check_final(VmafContext *vmaf, const MwiRun *run, const double *m2,
                            const double *m3)
{
    double sad[MWI_FEED];
    double f2[MWI_FEED];
    double f3[MWI_FEED];
    bool ok = true;
    for (unsigned i = 0; i < MWI_FEED && ok; i++) {
        ok = mwi_readable(vmaf, run, i, &f2[i], &f3[i]) &&
             (i + 1u == MWI_FEED ||
              (vmaf_test_identical_f64(f2[i], m2[i]) && vmaf_test_identical_f64(f3[i], m3[i])));
        ok = ok && (!run->sad || !vmaf_feature_score_at_index(vmaf, run->sad, &sad[i], i));
    }
    if (!ok || !run->sad)
        return ok;
    const MwiOpts o = {run->five, false, 1.0, 40.0, 10000.0};
    double want2[MWI_FEED];
    double want3[MWI_FEED];
    mwi_oracle(&o, sad, MWI_FEED, want2, want3);
    for (unsigned i = 0; i < MWI_FEED && ok; i++)
        ok = vmaf_test_identical_f64(f2[i], want2[i]) && vmaf_test_identical_f64(f3[i], want3[i]);
    return ok;
}

static char *mwi_context_run(const MwiRun *run)
{
    double m2[MWI_FEED] = {0.0};
    double m3[MWI_FEED] = {0.0};
    VmafContext *vmaf = NULL;
    int err = mwi_context(&vmaf, run);
    bool timely = true;
    for (unsigned k = 0; k < MWI_FEED && !err && timely; k++) {
        err = mwi_feed(vmaf, k);
        timely = err || mwi_check_after(vmaf, run, k, m2, m3);
    }
    err = err ? err : vmaf_read_pictures(vmaf, NULL, NULL, 0);
    const bool final = !err && timely && mwi_check_final(vmaf, run, m2, m3);
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    mu_assert("scoring failed", !err && !closed);
    mu_assert("a frame became final too early or too late", timely);
    mu_assert("a value changed after it became final, or is not upstream's", final);
    return NULL;
}

static char *test_context_frames_final_after_the_next_frame(void)
{
    for (size_t r = 0; r < MU_TABLE_LEN(mwi_runs); r++)
        mu_assert_msg(mwi_context_run(&mwi_runs[r]));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_advance_then_flush_equals_upstream_flush),
        MU_TEST(test_flush_without_state_equals_upstream_flush),
        MU_TEST(test_second_flush_and_late_advance_append_nothing),
        MU_TEST(test_refusals),
        MU_TEST(test_context_frames_final_after_the_next_frame),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
