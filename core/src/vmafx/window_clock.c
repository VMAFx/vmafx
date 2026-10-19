/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The window clock of the VMAFx API (RC4 WP4, ADR-2074; #2138): the `n_stats`
 * semantics in one place for every consumer (the FFmpeg filter, the
 * GStreamer element, a live plugin), so they cut a stream into the same
 * windows.
 *
 * Window k by time holds the frames whose presentation time lies in
 * [t0 + k * n, t0 + (k + 1) * n), t0 the first frame's; by frame count the
 * frames whose index lies in [i0 + k * m, i0 + (k + 1) * m). A window is
 * complete when the first frame past it arrives (whatever its length, the
 * clock cannot know a window by time is complete before), or at the end of
 * the stream, where it is flagged partial unless it is a full window by
 * frame count. Windows without a frame are skipped. Times are whole
 * nanoseconds, so window membership is integer arithmetic.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "error_internal.h"
#include "internal.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Nanoseconds per second. */
#define VMAFX_CLOCK_NS_PER_S 1e9
/* Longest window by time: the length must fit int64 nanoseconds. */
#define VMAFX_CLOCK_MAX_S 9.2e9

struct VmafxWindowClock {
    uint64_t window_ns; /* window length by time; 0: by frame count */
    uint64_t frames;    /* window length by frame count; 0: by time */
    bool have_frame;
    bool finished;
    uint64_t index0; /* i0 */
    int64_t t0;
    uint64_t prev_index;
    int64_t prev_pts;
    uint64_t k; /* the open window */
    uint64_t first;
    uint64_t last;
    uint64_t n_frames;
    int64_t first_pts;
    int64_t last_pts;
};

/* The window length in nanoseconds of `n_stats` seconds (0 for 0). */
static VmafxStatus window_ns(const VmafxReport *report, double n_stats, uint64_t *out)
{
    if (!isfinite(n_stats) || n_stats < 0.0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "config.n_stats",
                          "%g is not a window length in seconds", n_stats);
    }
    if (n_stats > VMAFX_CLOCK_MAX_S) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PARAMETER, "config.n_stats",
                          "%g s is longer than %g s", n_stats, VMAFX_CLOCK_MAX_S);
    }
    const double ns = nearbyint(n_stats * VMAFX_CLOCK_NS_PER_S);
    if (n_stats > 0.0 && ns < 1.0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "config.n_stats",
                          "%g s rounds to 0 ns", n_stats);
    }
    *out = (uint64_t)ns;
    return VMAFX_OK;
}

static VmafxStatus read_config(const VmafxReport *report, const VmafxWindowClockConfig *config,
                               uint64_t *ns, uint64_t *frames)
{
    if (!config) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "config",
                          "NULL argument");
    }
    VmafxWindowClockConfig c = VMAFX_WINDOW_CLOCK_CONFIG_INIT;
    VmafxStatus status = vmafx_read_sized(report, &c, (uint32_t)sizeof(c), config,
                                          VMAFX_MIN_WINDOW_CLOCK_CONFIG, "config");
    if (status == VMAFX_OK) {
        status = window_ns(report, c.n_stats, ns);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    if ((*ns != 0u) == (c.n_stats_frames != 0u)) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          "config.n_stats_frames",
                          "set exactly one of n_stats (%g s) and n_stats_frames (%llu)", c.n_stats,
                          (unsigned long long)c.n_stats_frames);
    }
    *frames = c.n_stats_frames;
    return VMAFX_OK;
}

VmafxStatus vmafx_window_clock_create(const VmafxWindowClockConfig *config, VmafxWindowClock **out,
                                      VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "out",
                          "no place to store the clock");
    }
    *out = NULL;
    uint64_t ns = 0;
    uint64_t frames = 0;
    const VmafxStatus status = read_config(&report, config, &ns, &frames);
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxWindowClock *const clock = calloc(1, sizeof(*clock));
    if (!clock) {
        return VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_PARAMETER, "out",
                          "cannot allocate a window clock");
    }
    clock->window_ns = ns;
    clock->frames = frames;
    *out = clock;
    return VMAFX_OK;
}

void vmafx_window_clock_destroy(VmafxWindowClock *clock)
{
    free(clock);
}

/* The window `index` at `pts_ns` lies in (the clock has its first frame). */
static uint64_t window_of(const VmafxWindowClock *clock, uint64_t index, int64_t pts_ns)
{
    assert(clock->have_frame && pts_ns >= clock->t0 && index >= clock->index0);
    if (clock->frames) {
        return (index - clock->index0) / clock->frames;
    }
    return ((uint64_t)pts_ns - (uint64_t)clock->t0) / clock->window_ns;
}

/* `a + b` for non-negative `b`, held at INT64_MAX. */
static int64_t add_ns(int64_t a, uint64_t b)
{
    const uint64_t room = (uint64_t)INT64_MAX - (uint64_t)a;
    return b > room ? INT64_MAX : (int64_t)((uint64_t)a + b);
}

/* The span of the open window. */
static VmafxWindowSpan open_span(const VmafxWindowClock *clock)
{
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    span.window = clock->k;
    span.first = clock->first;
    span.last = clock->last;
    span.n_frames = clock->n_frames;
    if (clock->frames) {
        span.start_ns = clock->first_pts;
        span.end_ns = clock->last_pts;
    } else {
        /* k * window_ns <= first_pts - t0, so the start fits. */
        span.start_ns = add_ns(clock->t0, clock->k * clock->window_ns);
        span.end_ns = add_ns(span.start_ns, clock->window_ns);
    }
    return span;
}

/* Open window `k` with the frame `index` at `pts_ns`. */
static void open_window(VmafxWindowClock *clock, uint64_t k, uint64_t index, int64_t pts_ns)
{
    clock->k = k;
    clock->first = index;
    clock->last = index;
    clock->n_frames = 1u;
    clock->first_pts = pts_ns;
    clock->last_pts = pts_ns;
}

static VmafxStatus check_frame(const VmafxReport *report, const VmafxWindowClock *clock,
                               uint64_t index, int64_t pts_ns)
{
    if (clock->finished) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "clock",
                          "the stream was finished; use a new clock");
    }
    if (clock->have_frame && index <= clock->prev_index) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, "index",
                          "frame index %llu does not follow %llu; indices increase strictly",
                          (unsigned long long)index, (unsigned long long)clock->prev_index);
    }
    if (clock->have_frame && pts_ns < clock->prev_pts) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, "pts_ns",
                          "time %lld ns is before the previous frame's %lld ns", (long long)pts_ns,
                          (long long)clock->prev_pts);
    }
    return VMAFX_OK;
}

/* Add the frame to the open window, or close the window and open the next:
 * VMAFX_OK with the closed window's span in `out`, else VMAFX_PENDING (or a
 * failure to write `out`, with the clock unchanged). */
static VmafxStatus advance_clock(const VmafxReport *report, VmafxWindowClock *clock, uint64_t index,
                                 int64_t pts_ns, VmafxWindowSpan *out)
{
    if (!clock->have_frame) {
        clock->have_frame = true;
        clock->index0 = index;
        clock->t0 = pts_ns;
        open_window(clock, 0u, index, pts_ns);
        return VMAFX_PENDING;
    }
    const uint64_t k = window_of(clock, index, pts_ns);
    if (k == clock->k) {
        clock->last = index;
        clock->last_pts = pts_ns;
        clock->n_frames++;
        return VMAFX_PENDING;
    }
    const VmafxWindowSpan span = open_span(clock);
    const VmafxStatus status = vmafx_write_sized(report, out, &span, (uint32_t)sizeof(span), "out");
    if (status == VMAFX_OK) {
        open_window(clock, k, index, pts_ns);
    }
    return status;
}

VmafxStatus vmafx_window_clock_frame(VmafxWindowClock *clock, uint64_t index, int64_t pts_ns,
                                     VmafxWindowSpan *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!clock || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !clock ? "clock" : "out", "NULL argument");
    }
    VmafxStatus status = check_frame(&report, clock, index, pts_ns);
    if (status == VMAFX_OK || status == VMAFX_PENDING) {
        status = advance_clock(&report, clock, index, pts_ns, out);
    }
    if (status == VMAFX_OK || status == VMAFX_PENDING) {
        clock->prev_index = index;
        clock->prev_pts = pts_ns;
    }
    return status;
}

VmafxStatus vmafx_window_clock_finish(VmafxWindowClock *clock, VmafxWindowSpan *out,
                                      VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!clock || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !clock ? "clock" : "out", "NULL argument");
    }
    const bool open = !clock->finished && clock->n_frames != 0u;
    clock->finished = true;
    if (!open) {
        return VMAFX_PENDING;
    }
    VmafxWindowSpan span = open_span(clock);
    /* A window by frame count that reached its last index is full. */
    const bool full =
        clock->frames && clock->last - clock->index0 == (clock->k + 1u) * clock->frames - 1u;
    span.flags = full ? 0u : VMAFX_WINDOW_PARTIAL;
    return vmafx_write_sized(&report, out, &span, (uint32_t)sizeof(span), "out");
}

/* NOLINTEND(modernize-use-nullptr) */
