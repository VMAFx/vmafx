/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * motion_rust derives motion2 / motion3 frame by frame, as the C `motion`
 * does (ADR-2090, ADR-1713 lane request MI-1): the twin implements
 * Extractor::advance(), the shim reaches it through VmafxRsTwin.advance, and
 * the engine initialises the shared context of a pooled Rust twin before its
 * first advance.
 *
 *   positive  on a context, serial and with worker threads, both windows:
 *             frame i of motion_rust is final once frame i + 1 is read and
 *             before the flush, the flush succeeds, no value changes after it
 *             became final, and every SAD, motion2 and motion3 value equals
 *             the C `motion` extractor's on the same frames bit for bit
 *   boundary  no frame is final before the SAD of frame min_idx; the last
 *             frame only at the flush
 *   negative  (failing first) a twin that inherits the C descriptor's
 *             advance() without the trampoline lets the C advance() derive
 *             motion2 / motion3 from the Rust SADs, and the Rust flush
 *             appends every frame again: the flush fails with -EINVAL; with
 *             worker threads the shared context is marked initialised by the
 *             advance and the Rust flush has no instance (-EINVAL)
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "float_bits.h"
#include "mu_table.h"
#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define RMW_W 64u
#define RMW_H 48u
#define RMW_FEED 9u

/* One window and thread count, with the collector names both extractors use
 * (the twin shares the C extractor's provided features and options). */
typedef struct RmwRun {
    bool five;
    unsigned n_threads;
    const char *sad;
    const char *motion2;
    const char *motion3;
} RmwRun;

static const RmwRun rmw_runs[] = {
    {false, 0u, "VMAF_integer_feature_motion_sad_score", "VMAF_integer_feature_motion2_score",
     "VMAF_integer_feature_motion3_score"},
    {true, 0u, "VMAF_integer_feature_motion_sad_score_mffw", "integer_motion2_mffw",
     "integer_motion3_mffw"},
    {false, 2u, "VMAF_integer_feature_motion_sad_score", "VMAF_integer_feature_motion2_score",
     "VMAF_integer_feature_motion3_score"},
    {true, 3u, "VMAF_integer_feature_motion_sad_score_mffw", "integer_motion2_mffw",
     "integer_motion3_mffw"},
};

/* What one session read: each frame's values when it became final, and after
 * the flush. */
typedef struct RmwValues {
    double m2[RMW_FEED];
    double m3[RMW_FEED];
    double f2[RMW_FEED];
    double f3[RMW_FEED];
    double sad[RMW_FEED];
} RmwValues;

/* A luma plane that moves by an uneven amount on every frame. */
static int rmw_picture(VmafPicture *pic, unsigned frame)
{
    static const unsigned shift[RMW_FEED] = {0u, 1u, 9u, 5u, 15u, 16u, 18u, 4u, 11u};
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, RMW_W, RMW_H);
    for (unsigned y = 0; !err && y < pic->h[0]; y++) {
        for (unsigned x = 0; x < pic->w[0]; x++) {
            const unsigned sx = x + shift[frame % RMW_FEED];
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

static int rmw_feed(VmafContext *vmaf, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = rmw_picture(&ref, frame);
    if (err)
        return err;
    err = rmw_picture(&dist, frame);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

static int rmw_context(VmafContext **vmaf, const char *extractor, const RmwRun *run)
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
    return vmaf_use_feature(*vmaf, extractor, opts);
}

/* Whether frame i has motion2 and motion3 (a fenced read). */
static bool rmw_readable(VmafContext *vmaf, const RmwRun *run, unsigned i, double *m2, double *m3)
{
    return !vmaf_feature_score_at_index(vmaf, run->motion2, m2, i) &&
           !vmaf_feature_score_at_index(vmaf, run->motion3, m3, i);
}

/* After frame k is read: frames with max(i + 1, min_idx) <= k are final, the
 * others are not. Records the values read. */
static bool rmw_check_after(VmafContext *vmaf, const char *extractor, const RmwRun *run, unsigned k,
                            RmwValues *v)
{
    const unsigned min_idx = run->five ? 2u : 1u;
    bool ok = true;
    for (unsigned i = 0; i <= k && ok; i++) {
        const unsigned needed = (i + 1u > min_idx) ? i + 1u : min_idx;
        ok = rmw_readable(vmaf, run, i, &v->m2[i], &v->m3[i]) == (needed <= k);
        if (!ok) {
            (void)fprintf(stderr, "\n%s (five %d, %u threads): frame %u after frame %u: %s\n",
                          extractor, run->five, run->n_threads, i, k,
                          needed <= k ? "not final" : "final too early");
        }
    }
    return ok;
}

/* After the flush: every frame readable, each value the one it had when it
 * became final (the last frame becomes final at the flush). */
static bool rmw_check_final(VmafContext *vmaf, const RmwRun *run, RmwValues *v)
{
    bool ok = true;
    for (unsigned i = 0; i < RMW_FEED && ok; i++) {
        ok = rmw_readable(vmaf, run, i, &v->f2[i], &v->f3[i]) &&
             !vmaf_feature_score_at_index(vmaf, run->sad, &v->sad[i], i) &&
             (i + 1u == RMW_FEED || (vmaf_test_identical_f64(v->f2[i], v->m2[i]) &&
                                     vmaf_test_identical_f64(v->f3[i], v->m3[i])));
    }
    return ok;
}

/* One session of `extractor` over the frames: NULL, or what failed. */
static char *rmw_session(const char *extractor, const RmwRun *run, RmwValues *v)
{
    assert(extractor != NULL && run != NULL && v != NULL);
    VmafContext *vmaf = NULL;
    int err = rmw_context(&vmaf, extractor, run);
    bool timely = true;
    for (unsigned k = 0; k < RMW_FEED && !err && timely; k++) {
        err = rmw_feed(vmaf, k);
        timely = err || rmw_check_after(vmaf, extractor, run, k, v);
    }
    const int flush_err = (err || !timely) ? 0 : vmaf_read_pictures(vmaf, NULL, NULL, 0);
    const bool final = !err && timely && !flush_err && rmw_check_final(vmaf, run, v);
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    if (err || flush_err) {
        (void)fprintf(stderr, "\n%s (five %d, %u threads): %s returned %d\n", extractor, run->five,
                      run->n_threads, err ? "scoring" : "flush", err ? err : flush_err);
    }
    if (err || closed)
        return "scoring failed";
    if (!timely)
        return "a frame became final too early or too late";
    if (flush_err)
        return "the flush failed";
    return final ? NULL : "a value changed after it became final";
}

/* Every SAD, motion2 and motion3 value of the twin is the C extractor's. */
static bool rmw_equal(const RmwValues *rust, const RmwValues *c)
{
    bool ok = true;
    for (unsigned i = 0; i < RMW_FEED && ok; i++) {
        ok = vmaf_test_identical_f64(rust->sad[i], c->sad[i]) &&
             vmaf_test_identical_f64(rust->f2[i], c->f2[i]) &&
             vmaf_test_identical_f64(rust->f3[i], c->f3[i]);
        if (!ok)
            (void)fprintf(stderr, "\nframe %u: motion_rust differs from motion\n", i);
    }
    return ok;
}

static char *rmw_run(const RmwRun *run)
{
    RmwValues rust;
    RmwValues c;
    (void)memset(&rust, 0, sizeof(rust));
    (void)memset(&c, 0, sizeof(c));
    mu_assert_msg(rmw_session("motion_rust", run, &rust));
    mu_assert_msg(rmw_session("motion", run, &c));
    mu_assert("motion_rust and motion differ", rmw_equal(&rust, &c));
    return NULL;
}

static char *test_motion_rust_frames_final_after_the_next_frame(void)
{
    for (size_t r = 0; r < MU_TABLE_LEN(rmw_runs); r++)
        mu_assert_msg(rmw_run(&rmw_runs[r]));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_motion_rust_frames_final_after_the_next_frame),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
