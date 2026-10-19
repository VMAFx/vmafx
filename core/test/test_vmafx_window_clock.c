/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The window clock of the VMAFx API (RC4 WP4, ADR-2074): the `n_stats`
 * semantics of #2138. Windows by time hold the frames whose presentation time
 * lies in [t0 + k * n, t0 + (k + 1) * n); windows by frame count the frames
 * [i0 + k * m, i0 + (k + 1) * m); a window closes when the first frame past
 * it arrives; empty windows are skipped; the window open at the end of the
 * stream is partial unless it is a full window by frame count.
 *
 * Failing first: none of the functions exists on the WP3 base.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* 60 frames per second: frame i at round(i * 1e9 / 60) ns. */
static int64_t pts_60(uint64_t i)
{
    return (int64_t)((i * 1000000000u + 30u) / 60u);
}

static VmafxWindowClock *clock_of(double n_stats, uint64_t n_stats_frames)
{
    VmafxWindowClockConfig config = VMAFX_WINDOW_CLOCK_CONFIG_INIT;
    config.n_stats = n_stats;
    config.n_stats_frames = n_stats_frames;
    VmafxWindowClock *clock = NULL;
    return vmafx_window_clock_create(&config, &clock, NULL) == VMAFX_OK ? clock : NULL;
}

static bool span_is(const VmafxWindowSpan *s, uint64_t window, uint64_t first, uint64_t last,
                    uint64_t n_frames, uint32_t flags)
{
    return s->window == window && s->first == first && s->last == last && s->n_frames == n_frames &&
           s->flags == flags;
}

/* Feed `clock` frame `index` at `pts`; true when it closed a window. */
static bool feed(VmafxWindowClock *clock, uint64_t index, int64_t pts, VmafxWindowSpan *span)
{
    *span = (VmafxWindowSpan)VMAFX_WINDOW_SPAN_INIT;
    return vmafx_window_clock_frame(clock, index, pts, span, NULL) == VMAFX_OK;
}

/* Frames [from, to) at 60 fps keep the open window open. */
static char *feed_open(VmafxWindowClock *clock, uint64_t from, uint64_t to)
{
    for (uint64_t i = from; i < to; i++) {
        VmafxWindowSpan span;
        mu_assert("window open", !feed(clock, i, pts_60(i), &span));
    }
    return NULL;
}

/* After the end of the stream: nothing more to finish, no frame taken. */
static char *check_finished(VmafxWindowClock *clock)
{
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    VmafxError *error = NULL;
    mu_assert("nothing left", vmafx_window_clock_finish(clock, &span, NULL) == VMAFX_PENDING);
    mu_assert("no frame after the end",
              vmafx_window_clock_frame(clock, 25, pts_60(25), &span, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "clock", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

static char *test_windows_by_time(void)
{
    VmafxWindowClock *clock = clock_of(0.2, 0);
    mu_assert("clock", clock);
    VmafxWindowSpan span;
    /* 0.2 s at 60 fps: frames 0..11 are window 0, 12..23 window 1. */
    mu_assert_msg(feed_open(clock, 0, 12));
    mu_assert("frame 12 closes window 0", feed(clock, 12, pts_60(12), &span) &&
                                              span_is(&span, 0, 0, 11, 12, 0) &&
                                              span.start_ns == 0 && span.end_ns == 200000000);
    mu_assert_msg(feed_open(clock, 13, 24));
    mu_assert("frame 24 closes window 1",
              feed(clock, 24, pts_60(24), &span) && span_is(&span, 1, 12, 23, 12, 0) &&
                  span.start_ns == 200000000 && span.end_ns == 400000000);
    mu_assert("end of stream: partial window 2",
              vmafx_window_clock_finish(clock, &span, NULL) == VMAFX_OK &&
                  span_is(&span, 2, 24, 24, 1, VMAFX_WINDOW_PARTIAL));
    mu_assert_msg(check_finished(clock));
    vmafx_window_clock_destroy(clock);
    return NULL;
}

static char *test_gaps_skip_windows_and_t0_is_the_first_frame(void)
{
    VmafxWindowClock *clock = clock_of(1.0, 0);
    mu_assert("clock", clock);
    VmafxWindowSpan span;
    const int64_t t0 = -500000000; /* streams may start before 0 */
    mu_assert("first", !feed(clock, 10, t0, &span));
    mu_assert("same window", !feed(clock, 11, t0 + 999999999, &span));
    /* A gap of 3 s: windows 1 and 2 hold no frame. */
    mu_assert("window 0 closes", feed(clock, 12, t0 + 3200000000, &span) &&
                                     span_is(&span, 0, 10, 11, 2, 0) && span.start_ns == t0 &&
                                     span.end_ns == t0 + 1000000000);
    mu_assert("window 3 on the boundary", feed(clock, 13, t0 + 4000000000, &span) &&
                                              span_is(&span, 3, 12, 12, 1, 0) &&
                                              span.start_ns == t0 + 3000000000);
    mu_assert("equal times stay in one window", !feed(clock, 14, t0 + 4000000000, &span));
    mu_assert("last", vmafx_window_clock_finish(clock, &span, NULL) == VMAFX_OK &&
                          span_is(&span, 4, 13, 14, 2, VMAFX_WINDOW_PARTIAL));
    vmafx_window_clock_destroy(clock);
    return NULL;
}

/* A short last window by frame count is partial. */
static char *check_short_last_window(void)
{
    VmafxWindowClock *clock = clock_of(0.0, 5);
    VmafxWindowSpan span;
    mu_assert("clock", clock && !feed(clock, 0, 0, &span) && !feed(clock, 1, 1, &span));
    const bool partial = vmafx_window_clock_finish(clock, &span, NULL) == VMAFX_OK &&
                         span_is(&span, 0, 0, 1, 2, VMAFX_WINDOW_PARTIAL);
    vmafx_window_clock_destroy(clock);
    mu_assert("a short last window is", partial);
    return NULL;
}

static char *test_windows_by_frame_count(void)
{
    VmafxWindowClock *clock = clock_of(0.0, 5);
    mu_assert("clock", clock);
    VmafxWindowSpan span;
    mu_assert_msg(feed_open(clock, 3, 8)); /* window 0: frames 3 to 7 */
    mu_assert("frame 8 closes window 0",
              feed(clock, 8, pts_60(8), &span) && span_is(&span, 0, 3, 7, 5, 0) &&
                  span.start_ns == pts_60(3) && span.end_ns == pts_60(7));
    /* Index 9 to 12 is window 1; frames 10 and 11 were dropped. */
    mu_assert("window 1", !feed(clock, 9, pts_60(9), &span) && !feed(clock, 12, pts_60(12), &span));
    mu_assert("frame 13 closes it",
              feed(clock, 13, pts_60(13), &span) && span_is(&span, 1, 8, 12, 3, 0));
    mu_assert_msg(feed_open(clock, 14, 18));
    mu_assert("a full window at the end is not partial",
              vmafx_window_clock_finish(clock, &span, NULL) == VMAFX_OK &&
                  span_is(&span, 2, 13, 17, 5, 0));
    vmafx_window_clock_destroy(clock);
    return check_short_last_window();
}

static char *test_n_stats_rounds_to_nanoseconds(void)
{
    VmafxWindowClock *clock = clock_of(1.0 / 3.0, 0);
    mu_assert("clock", clock);
    VmafxWindowSpan span;
    mu_assert("first", !feed(clock, 0, 0, &span));
    mu_assert("333333332 ns is window 0", !feed(clock, 1, 333333332, &span));
    mu_assert("333333333 ns is window 1",
              feed(clock, 2, 333333333, &span) && span.end_ns == 333333333);
    vmafx_window_clock_destroy(clock);
    return NULL;
}

static bool config_refused(double n_stats, uint64_t frames, VmafxStatus status, const char *field)
{
    VmafxWindowClockConfig config = VMAFX_WINDOW_CLOCK_CONFIG_INIT;
    config.n_stats = n_stats;
    config.n_stats_frames = frames;
    VmafxWindowClock *clock = NULL;
    VmafxError *error = NULL;
    return vmafx_window_clock_create(&config, &clock, &error) == status && !clock &&
           vt_failed(&error, status, field, VMAFX_SUBJECT_PARAMETER);
}

static char *check_frame_refusals(void)
{
    VmafxWindowClock *clock = clock_of(2.0, 0);
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    VmafxError *error = NULL;
    mu_assert("clock", clock && !feed(clock, 5, 100, &span));
    mu_assert("index repeats",
              vmafx_window_clock_frame(clock, 5, 200, &span, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "index", VMAFX_SUBJECT_FRAME));
    mu_assert("time goes back",
              vmafx_window_clock_frame(clock, 6, 99, &span, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "pts_ns", VMAFX_SUBJECT_FRAME));
    mu_assert("refused frames leave it unchanged", !feed(clock, 6, 100, &span));
    mu_assert("NULL out",
              vmafx_window_clock_frame(clock, 7, 100, NULL, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "out", VMAFX_SUBJECT_PARAMETER));
    vmafx_window_clock_destroy(clock);
    vmafx_window_clock_destroy(NULL);
    return NULL;
}

static char *test_refusals_are_named(void)
{
    mu_assert("neither", config_refused(0.0, 0, VMAFX_E_INVALID, "config.n_stats_frames"));
    mu_assert("both", config_refused(2.0, 48, VMAFX_E_INVALID, "config.n_stats_frames"));
    mu_assert("negative", config_refused(-1.0, 0, VMAFX_E_INVALID, "config.n_stats"));
    mu_assert("not a number", config_refused(NAN, 0, VMAFX_E_INVALID, "config.n_stats"));
    mu_assert("infinite", config_refused(INFINITY, 0, VMAFX_E_INVALID, "config.n_stats"));
    mu_assert("below 1 ns", config_refused(1e-12, 0, VMAFX_E_INVALID, "config.n_stats"));
    mu_assert("past int64 ns", config_refused(1e10, 0, VMAFX_E_RANGE, "config.n_stats"));
    return check_frame_refusals();
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_windows_by_time),
        MU_TEST(test_gaps_skip_windows_and_t0_is_the_first_frame),
        MU_TEST(test_windows_by_frame_count),
        MU_TEST(test_n_stats_rounds_to_nanoseconds),
        MU_TEST(test_refusals_are_named),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
