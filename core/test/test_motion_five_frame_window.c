/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * motion_five_frame_window on the CPU extractors `motion` and `motion_v2`
 * (Netflix a2b59b77 / a4a1492d, ADR-1478).
 *
 * With the option the SAD of frame n is taken against frame n-2, which the
 * framework hands the extractor as fex->prev_prev_ref, and motion2 of frame n
 * is the smaller of the SADs of frames n-1 and n+1. The expected scores are
 * built here from the three-frame extractor, which this option leaves
 * unchanged: the SAD of frame n is the three-frame SAD of the two-frame
 * sequence (frame n-2, frame n). motion2 and motion3 follow from the
 * definition in integer_motion.c::flush(), written out a second time below.
 *
 *   positive  every score of a seven-frame run, with and without the moving
 *             average, serial and on worker threads, on both extractors; a
 *             preallocated pool of four pictures with the window on
 *   boundary  sequences of one, two and three frames; with the window off a
 *             pool of three pictures still serves a whole sequence, as it
 *             did before the port (the context keeps frame n-2 only for an
 *             extractor that reads it)
 *   negative  with the window on, a preallocated pool below four pictures is
 *             refused with -EINVAL at registration, whichever comes first;
 *             an extractor asked for frame 2 with no frame n-2 fails; a GPU
 *             twin that has no five-frame window (Metal) hands the option to
 *             the CPU extractor instead of scoring with another window
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "mu_table.h"
#include "test.h"
#include "float_bits.h"

#include "dict.h"
#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf_priv.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define FFW_W 64u
#define FFW_H 48u
#define FFW_FRAMES 7u
#define FFW_WATCHDOG_SECONDS 60u
#define FFW_NAME_MAX 96u

/* The names one extractor writes for one option set. */
typedef struct FfwNames {
    char sad[FFW_NAME_MAX];
    char motion2[FFW_NAME_MAX];
    char motion3[FFW_NAME_MAX];
} FfwNames;

/* One run: the extractor, whether the moving average is on, and how the
 * frames reach the context. */
typedef struct FfwRun {
    const char *extractor;
    bool moving_average;
    unsigned n_threads;
    unsigned n_frames;
    unsigned pool; /* pictures come from a preallocated pool of this many; 0: allocated */
} FfwRun;

typedef struct FfwScores {
    double sad[FFW_FRAMES];
    double motion2[FFW_FRAMES];
    double motion3[FFW_FRAMES];
} FfwScores;

/* A textured luma plane that moves by a different amount on every frame, so
 * consecutive SADs neither grow nor shrink monotonically and both arms of
 * the motion2 minimum are taken. */
static void ffw_fill(VmafPicture *pic, unsigned frame)
{
    static const unsigned shift[FFW_FRAMES] = {0u, 1u, 9u, 5u, 15u, 16u, 18u};
    uint8_t *luma = pic->data[0];
    for (unsigned y = 0; y < pic->h[0]; y++) {
        for (unsigned x = 0; x < pic->w[0]; x++) {
            const unsigned sx = x + shift[frame % FFW_FRAMES];
            const unsigned v = (sx * 7u + y * 13u + ((sx * y) >> 3)) & 0xFFu;
            luma[(size_t)y * (size_t)pic->stride[0] + x] = (uint8_t)v;
        }
    }
    for (unsigned p = 1; p < 3u; p++) {
        for (unsigned y = 0; y < pic->h[p]; y++)
            memset((uint8_t *)pic->data[p] + (size_t)y * (size_t)pic->stride[p], 128, pic->w[p]);
    }
}

/* The names the five-frame run writes: the option suffixes follow the
 * extractor's option table order (mffw before mma). */
static void ffw_names(const FfwRun *run, FfwNames *names)
{
    const bool v2 = strcmp(run->extractor, "motion_v2") == 0;
    const char *suffix = run->moving_average ? "_mffw_mma" : "_mffw";
    (void)snprintf(names->sad, sizeof(names->sad), "%s%s",
                   v2 ? "VMAF_integer_feature_motion_v2_sad_score" :
                        "VMAF_integer_feature_motion_sad_score",
                   suffix);
    (void)snprintf(names->motion2, sizeof(names->motion2), "%s%s",
                   v2 ? "VMAF_integer_feature_motion2_v2_score" : "integer_motion2", suffix);
    (void)snprintf(names->motion3, sizeof(names->motion3), "%s%s",
                   v2 ? "VMAF_integer_feature_motion3_v2_score" : "integer_motion3", suffix);
}

static int ffw_use_feature(VmafContext *vmaf, const FfwRun *run, bool five_frame)
{
    VmafFeatureDictionary *opts = NULL;
    int err = 0;
    if (five_frame)
        err = vmaf_feature_dictionary_set(&opts, "motion_five_frame_window", "true");
    if (!err && run->moving_average)
        err = vmaf_feature_dictionary_set(&opts, "motion_moving_average", "true");
    if (err) {
        (void)vmaf_feature_dictionary_free(&opts);
        return err;
    }
    return vmaf_use_feature(vmaf, run->extractor, opts);
}

static int ffw_next_picture(VmafContext *vmaf, bool pooled, VmafPicture *pic)
{
    if (pooled)
        return vmaf_fetch_preallocated_picture(vmaf, pic);
    return vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, FFW_W, FFW_H);
}

/* Feed `frames[0..n)` (indices into the fixture) as frames 0..n-1. */
static int ffw_feed(VmafContext *vmaf, const unsigned *frames, unsigned n, bool pooled)
{
    for (unsigned i = 0; i < n; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = ffw_next_picture(vmaf, pooled, &ref);
        if (err)
            return err;
        err = ffw_next_picture(vmaf, pooled, &dist);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        ffw_fill(&ref, frames[i]);
        ffw_fill(&dist, frames[i]);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int ffw_read_scores(VmafContext *vmaf, const FfwNames *names, unsigned n, FfwScores *out)
{
    for (unsigned i = 0; i < n; i++) {
        int err = vmaf_feature_score_at_index(vmaf, names->sad, &out->sad[i], i);
        if (!err)
            err = vmaf_feature_score_at_index(vmaf, names->motion2, &out->motion2[i], i);
        if (!err)
            err = vmaf_feature_score_at_index(vmaf, names->motion3, &out->motion3[i], i);
        if (err)
            return err;
    }
    return 0;
}

static VmafPictureConfiguration ffw_pool(unsigned pic_cnt)
{
    const VmafPictureConfiguration pool = {
        .pic_params = {.w = FFW_W, .h = FFW_H, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P},
        .pic_cnt = pic_cnt,
    };
    return pool;
}

/* Open a context for the run, feed `frames[0..n)` and flush. The caller
 * closes *out, also after a failure. */
static int ffw_run_frames(const FfwRun *run, bool five_frame, const unsigned *frames, unsigned n,
                          VmafContext **out)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = run->n_threads};
    int err = vmaf_init(out, cfg);
    if (err)
        return err;
    if (run->pool)
        err = vmaf_preallocate_pictures(*out, ffw_pool(run->pool));
    if (!err)
        err = ffw_use_feature(*out, run, five_frame);
    if (!err)
        err = ffw_feed(*out, frames, n, run->pool != 0u);
    return err;
}

/* Score `frames[0..n)` with the five-frame window. */
static int ffw_score(const FfwRun *run, const unsigned *frames, unsigned n, FfwScores *out)
{
    VmafContext *vmaf = NULL;
    int err = ffw_run_frames(run, true, frames, n, &vmaf);
    if (!err) {
        FfwNames names;
        ffw_names(run, &names);
        err = ffw_read_scores(vmaf, &names, n, out);
    }
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : close_err;
}

/* The SAD the five-frame window reports for frame i >= 2: the three-frame SAD
 * of the pair (frame i-2, frame i), read from a serial two-frame run with
 * default options. */
static int ffw_expected_sad(const FfwRun *run, unsigned i, double *sad)
{
    const unsigned pair[2] = {i - 2u, i};
    const bool v2 = strcmp(run->extractor, "motion_v2") == 0;
    const FfwRun three_frame = {.extractor = run->extractor, .n_frames = 2u};
    VmafContext *vmaf = NULL;
    int err = ffw_run_frames(&three_frame, false, pair, 2u, &vmaf);
    if (!err) {
        err = vmaf_feature_score_at_index(vmaf,
                                          v2 ? "VMAF_integer_feature_motion_v2_sad_score" :
                                               "VMAF_integer_feature_motion_sad_score",
                                          sad, 1u);
    }
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : close_err;
}

/* integer_motion.c::flush() with min_idx = stride = 2 and the default blend
 * (factor 1: the blended value is the value) and maximum (never reached by
 * this fixture), written out a second time. */
static void ffw_expected_window(const FfwRun *run, unsigned n, FfwScores *want)
{
    const double stamp = n > 2u ? want->sad[2] : 0.;
    double prev_processed = 0.;
    for (unsigned i = 0; i < n; i++) {
        if (i < 2u) {
            want->motion2[i] = 0.;
            want->motion3[i] = stamp;
            prev_processed = stamp;
            continue;
        }
        if (i + 1u >= n) {
            want->motion2[i] = want->sad[i];
        } else if (i >= 3u) {
            const double lo = want->sad[i - 1u];
            const double hi = want->sad[i + 1u];
            want->motion2[i] = lo < hi ? lo : hi;
        } else {
            want->motion2[i] = want->sad[i + 1u];
        }
        const double processed = want->motion2[i];
        want->motion3[i] = run->moving_average ? (processed + prev_processed) / 2.0 : processed;
        prev_processed = processed;
    }
}

static int ffw_expected(const FfwRun *run, FfwScores *want)
{
    for (unsigned i = 0; i < run->n_frames; i++) {
        want->sad[i] = 0.;
        if (i < 2u)
            continue;
        const int err = ffw_expected_sad(run, i, &want->sad[i]);
        if (err)
            return err;
    }
    ffw_expected_window(run, run->n_frames, want);
    return 0;
}

static bool ffw_same(const FfwScores *a, const FfwScores *b, unsigned n)
{
    return memcmp(a->sad, b->sad, n * sizeof(a->sad[0])) == 0 &&
           memcmp(a->motion2, b->motion2, n * sizeof(a->motion2[0])) == 0 &&
           memcmp(a->motion3, b->motion3, n * sizeof(a->motion3[0])) == 0;
}

static char *ffw_check_run(const FfwRun *run)
{
    static const unsigned in_order[FFW_FRAMES] = {0u, 1u, 2u, 3u, 4u, 5u, 6u};
    FfwScores want;
    FfwScores got;
    mu_assert("the three-frame reference run failed", ffw_expected(run, &want) == 0);
    mu_assert("the five-frame run failed", ffw_score(run, in_order, run->n_frames, &got) == 0);
    mu_assert("a five-frame score differs from the definition",
              ffw_same(&want, &got, run->n_frames));
    return NULL;
}

/* The fixture is only a test of the window when the SADs differ and the
 * minimum takes both sides. */
static char *test_fixture_exercises_both_arms(void)
{
    const FfwRun run = {.extractor = "motion", .n_frames = FFW_FRAMES};
    FfwScores want;
    mu_assert("the reference run failed", ffw_expected(&run, &want) == 0);
    bool took_lo = false;
    bool took_hi = false;
    for (unsigned i = 3u; i + 1u < FFW_FRAMES; i++) {
        mu_assert("a frame has no motion", want.sad[i] > 0.);
        took_lo = took_lo || want.sad[i - 1u] < want.sad[i + 1u];
        took_hi = took_hi || want.sad[i + 1u] < want.sad[i - 1u];
    }
    mu_assert("the minimum never took the earlier SAD", took_lo);
    mu_assert("the minimum never took the later SAD", took_hi);
    return NULL;
}

static char *test_motion_five_frame_scores(void)
{
    const FfwRun run = {.extractor = "motion", .n_frames = FFW_FRAMES};
    return ffw_check_run(&run);
}

static char *test_motion_five_frame_moving_average(void)
{
    const FfwRun run = {.extractor = "motion", .moving_average = true, .n_frames = FFW_FRAMES};
    return ffw_check_run(&run);
}

static char *test_motion_v2_five_frame_scores(void)
{
    const FfwRun run = {.extractor = "motion_v2", .n_frames = FFW_FRAMES};
    mu_assert_msg(ffw_check_run(&run));
    const FfwRun averaged = {
        .extractor = "motion_v2", .moving_average = true, .n_frames = FFW_FRAMES};
    return ffw_check_run(&averaged);
}

/* Worker threads read the same two earlier frames as a serial run. */
static char *test_worker_threads_return_the_serial_scores(void)
{
    static const unsigned threads[] = {1u, 2u, 4u};
    static const char *const extractors[] = {"motion", "motion_v2"};
    for (size_t e = 0; e < sizeof(extractors) / sizeof(extractors[0]); e++) {
        for (size_t t = 0; t < sizeof(threads) / sizeof(threads[0]); t++) {
            const FfwRun run = {.extractor = extractors[e],
                                .moving_average = true,
                                .n_threads = threads[t],
                                .n_frames = FFW_FRAMES};
            mu_assert_msg(ffw_check_run(&run));
        }
    }
    return NULL;
}

/* One and two frames have no frame n-2: every score is 0. Three frames have
 * one SAD, which is motion2 of the last frame and motion3 of all three. */
static char *test_short_sequences(void)
{
    static const char *const extractors[] = {"motion", "motion_v2"};
    for (size_t e = 0; e < sizeof(extractors) / sizeof(extractors[0]); e++) {
        for (unsigned n = 1u; n <= 3u; n++) {
            const FfwRun run = {.extractor = extractors[e], .n_frames = n};
            mu_assert_msg(ffw_check_run(&run));
        }
        static const unsigned in_order[3] = {0u, 1u, 2u};
        const FfwRun three = {.extractor = extractors[e], .n_frames = 3u};
        FfwScores got;
        mu_assert("the three-frame sequence failed", ffw_score(&three, in_order, 3u, &got) == 0);
        mu_assert("frame 2 has no SAD", got.sad[2] > 0.);
        mu_assert("motion2 of the last frame is not its SAD",
                  vmaf_test_expect_identical_f64("motion2[2]", got.motion2[2], got.sad[2]));
        mu_assert("motion3 of frame 0 is not the SAD of frame 2",
                  vmaf_test_expect_identical_f64("motion3[0]", got.motion3[0], got.sad[2]));
        mu_assert("motion3 of frame 1 is not the SAD of frame 2",
                  vmaf_test_expect_identical_f64("motion3[1]", got.motion3[1], got.sad[2]));
    }
    return NULL;
}

/* With the window on, the context keeps the reference pictures of the two
 * frames before the current one. A pool of four pictures (those two and the
 * current pair) must be enough for any number of frames; a picture that is
 * not released turns into a fetch that never returns, which the alarm ends. */
static char *test_pool_of_four_pictures(void)
{
#ifndef _WIN32
    (void)alarm(FFW_WATCHDOG_SECONDS);
#endif
    static const unsigned threads[] = {0u, 1u};
    for (size_t t = 0; t < sizeof(threads) / sizeof(threads[0]); t++) {
        const FfwRun run = {
            .extractor = "motion", .n_threads = threads[t], .n_frames = FFW_FRAMES, .pool = 4u};
        mu_assert_msg(ffw_check_run(&run));
    }
#ifndef _WIN32
    (void)alarm(0);
#endif
    return NULL;
}

/* The three-frame scores of a run with default options. */
static int ffw_three_frame_scores(const FfwRun *run, FfwScores *out)
{
    static const unsigned in_order[FFW_FRAMES] = {0u, 1u, 2u, 3u, 4u, 5u, 6u};
    const bool v2 = strcmp(run->extractor, "motion_v2") == 0;
    FfwNames names;
    (void)snprintf(names.sad, sizeof(names.sad), "%s",
                   v2 ? "VMAF_integer_feature_motion_v2_sad_score" :
                        "VMAF_integer_feature_motion_sad_score");
    (void)snprintf(names.motion2, sizeof(names.motion2), "%s",
                   v2 ? "VMAF_integer_feature_motion2_v2_score" :
                        "VMAF_integer_feature_motion2_score");
    (void)snprintf(names.motion3, sizeof(names.motion3), "%s",
                   v2 ? "VMAF_integer_feature_motion3_v2_score" :
                        "VMAF_integer_feature_motion3_score");
    VmafContext *vmaf = NULL;
    int err = ffw_run_frames(run, false, in_order, run->n_frames, &vmaf);
    if (!err)
        err = ffw_read_scores(vmaf, &names, run->n_frames, out);
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : close_err;
}

/* Boundary: with the window off the context keeps one earlier reference
 * picture, as before the port, so a pool of three (that picture and the
 * current pair) serves a whole sequence, serial and with a worker, and the
 * scores are those of pictures allocated one by one. */
static char *test_pool_of_three_without_the_window(void)
{
#ifndef _WIN32
    (void)alarm(FFW_WATCHDOG_SECONDS);
#endif
    static const unsigned threads[] = {0u, 1u};
    static const char *const extractors[] = {"motion", "motion_v2"};
    for (size_t e = 0; e < sizeof(extractors) / sizeof(extractors[0]); e++) {
        for (size_t t = 0; t < sizeof(threads) / sizeof(threads[0]); t++) {
            const FfwRun pooled = {.extractor = extractors[e],
                                   .n_threads = threads[t],
                                   .n_frames = FFW_FRAMES,
                                   .pool = 3u};
            const FfwRun allocated = {
                .extractor = extractors[e], .n_threads = threads[t], .n_frames = FFW_FRAMES};
            FfwScores want;
            FfwScores got;
            mu_assert("the run without a pool failed",
                      ffw_three_frame_scores(&allocated, &want) == 0);
            mu_assert("a pool of three stalled or failed without the window",
                      ffw_three_frame_scores(&pooled, &got) == 0);
            mu_assert("a pooled score differs", ffw_same(&want, &got, FFW_FRAMES));
        }
    }
#ifndef _WIN32
    (void)alarm(0);
#endif
    return NULL;
}

/* Register `extractor` with the window on next to a pool of `pic_cnt`
 * pictures, the pool first or the extractor first. Returns the first
 * error. */
static int ffw_register_with_pool(const char *extractor, unsigned pic_cnt, bool pool_first)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;
    const FfwRun run = {.extractor = extractor};
    if (pool_first) {
        err = vmaf_preallocate_pictures(vmaf, ffw_pool(pic_cnt));
        if (!err)
            err = ffw_use_feature(vmaf, &run, true);
    } else {
        err = ffw_use_feature(vmaf, &run, true);
        if (!err)
            err = vmaf_preallocate_pictures(vmaf, ffw_pool(pic_cnt));
    }
    const int close_err = vmaf_close(vmaf);
    return err ? err : close_err;
}

/* An _hfr model sets the window on its motion feature: registering it next
 * to a pool of three fails the same way. */
static int ffw_register_model_with_pool(unsigned pic_cnt)
{
    VmafModel *model = NULL;
    VmafModelConfig model_cfg = {.name = "hfr"};
    int err = vmaf_model_load_from_path(
        &model, &model_cfg, JSON_MODEL_PATH "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_3d0h.json");
    if (err)
        return -ENOENT;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = vmaf_preallocate_pictures(vmaf, ffw_pool(pic_cnt));
    if (!err)
        err = vmaf_use_features_from_model(vmaf, model);
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    vmaf_model_destroy(model);
    return err ? err : close_err;
}

/* Negative: with the window on, a preallocated pool below four pictures
 * would stall on the third frame. It is refused at registration with
 * -EINVAL, whichever of the two comes first, and four is accepted. */
static char *test_pool_below_four_is_refused(void)
{
    static const char *const extractors[] = {"motion", "motion_v2"};
    for (size_t e = 0; e < sizeof(extractors) / sizeof(extractors[0]); e++) {
        mu_assert("a pool of three before the extractor was accepted",
                  ffw_register_with_pool(extractors[e], 3u, true) == -EINVAL);
        mu_assert("a pool of three after the extractor was accepted",
                  ffw_register_with_pool(extractors[e], 3u, false) == -EINVAL);
        mu_assert("a pool of one was accepted",
                  ffw_register_with_pool(extractors[e], 1u, true) == -EINVAL);
        mu_assert("a pool of four before the extractor was refused",
                  ffw_register_with_pool(extractors[e], 4u, true) == 0);
        mu_assert("a pool of four after the extractor was refused",
                  ffw_register_with_pool(extractors[e], 4u, false) == 0);
    }
    mu_assert("an _hfr model next to a pool of three was accepted",
              ffw_register_model_with_pool(3u) == -EINVAL);
    mu_assert("an _hfr model next to a pool of four was refused",
              ffw_register_model_with_pool(4u) == 0);
    return NULL;
}

/* Frame 2 with no frame n-2: the extractor has nothing to difference
 * against and reports -EINVAL instead of reading an empty picture. */
static char *ffw_extract_without_window(const char *name)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(name);
    mu_assert("extractor missing", fex != NULL);
    VmafDictionary *opts = NULL;
    mu_assert("set option", vmaf_dictionary_set(&opts, "motion_five_frame_window", "true", 0) == 0);
    VmafFeatureExtractorContext *ctx = NULL;
    mu_assert("context_create", vmaf_feature_extractor_context_create(&ctx, fex, opts) == 0);
    VmafFeatureCollector *fc = NULL;
    mu_assert("collector_init", vmaf_feature_collector_init(&fc) == 0);
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("alloc ref", vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, 8, FFW_W, FFW_H) == 0);
    mu_assert("alloc dist", vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, 8, FFW_W, FFW_H) == 0);
    ffw_fill(&ref, 0u);
    ffw_fill(&dist, 0u);

    const int err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 2u, fc);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    (void)vmaf_picture_unref(&ref);
    (void)vmaf_picture_unref(&dist);
    mu_assert("frame 2 without frame 0 was accepted", err == -EINVAL);
    return NULL;
}

static char *test_missing_window_is_rejected(void)
{
    mu_assert_msg(ffw_extract_without_window("motion"));
    return ffw_extract_without_window("motion_v2");
}

/* The twin-selection verdict (ADR-1359) for the option set to `value` on
 * the registered extractor `name`; -ENOENT when this build has no such
 * extractor. */
static int ffw_twin_verdict(const char *name, const char *value)
{
    const VmafFeatureExtractor *twin = vmaf_get_feature_extractor_by_name(name);
    if (!twin)
        return -ENOENT;
    VmafDictionary *opts = NULL;
    if (vmaf_dictionary_set(&opts, "motion_five_frame_window", value, 0))
        return -ENOMEM;
    const char *unsupported = NULL;
    const int verdict = vmaf_backend_twin_verdict_for_test(twin, opts, NULL, &unsupported);
    (void)vmaf_dictionary_free(&opts);
    return verdict;
}

/* The CUDA, SYCL, HIP and Metal twins of `motion` and `motion_v2` compute
 * the window (ADR-1491; Metal since ADR-1498): model dispatch keeps the twin,
 * verdict 0. A build without the backend has no such extractor and nothing to
 * check. */
static char *test_twins_honour_the_option_or_leave_it_to_the_cpu(void)
{
    static const char *const with_window[] = {
        "motion_cuda",    "motion_sycl",    "motion_hip",    "integer_motion_metal",
        "motion_v2_cuda", "motion_v2_sycl", "motion_v2_hip", "motion_v2_metal",
    };
    for (size_t i = 0; i < sizeof(with_window) / sizeof(with_window[0]); i++) {
        const int verdict = ffw_twin_verdict(with_window[i], "true");
        mu_assert("a twin with the five-frame window handed the option to the CPU",
                  verdict == -ENOENT || verdict == 0);
    }
    /* The CPU extractors honour both values. */
    mu_assert("motion refuses the option", ffw_twin_verdict("motion", "true") == 0);
    mu_assert("motion_v2 refuses the option", ffw_twin_verdict("motion_v2", "true") == 0);
    mu_assert("motion refuses the default", ffw_twin_verdict("motion", "false") == 0);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_fixture_exercises_both_arms),
        MU_TEST(test_motion_five_frame_scores),
        MU_TEST(test_motion_five_frame_moving_average),
        MU_TEST(test_motion_v2_five_frame_scores),
        MU_TEST(test_worker_threads_return_the_serial_scores),
        MU_TEST(test_short_sequences),
        MU_TEST(test_pool_of_four_pictures),
        MU_TEST(test_pool_of_three_without_the_window),
        MU_TEST(test_pool_below_four_is_refused),
        MU_TEST(test_missing_window_is_rejected),
        MU_TEST(test_twins_honour_the_option_or_leave_it_to_the_cpu),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
