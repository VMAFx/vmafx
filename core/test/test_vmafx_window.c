/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Window scores of the VMAFx API (RC4 WP4, ADR-2074; design section 2.8):
 * every value of a window equals the synchronous pooled call over its frames
 * bit for bit, for every pooling method, on sessions of imported scores
 * (deterministic, no video) and of scored frames; a window completes in the
 * call that makes its last frame final (the frame after `last` for motion2 /
 * motion3), partially at the flush, never before; subsampling is counted;
 * many windows stay open; release cancels; callbacks run once; refusals are
 * named.
 *
 * Failing first: none of the functions exists on the WP3 base. Planted
 * defects this file refuses are listed in ADR-2074 (a completion one frame
 * early, a pool slot off by one, a cursor that skips frames).
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"
#include "vmafx_window_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { W = 176, H = 144, N_IMPORTED = 40 };

static const char feature[] = "imported_score";

/* A deterministic, uneven score for frame `i`. */
static double imported_value(unsigned i)
{
    return 60.0 + 30.0 * sin((double)i * 0.37) + (double)(i % 7u) * 0.125;
}

static VmafxContext *plain_context(uint32_t n_threads, uint32_t n_subsample)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.n_threads = n_threads;
    config.n_subsample = n_subsample;
    VmafxContext *context = NULL;
    return vmafx_context_create(&config, &context, NULL) == VMAFX_OK ? context : NULL;
}

static bool import_range(VmafxContext *context, unsigned from, unsigned to)
{
    bool ok = true;
    for (unsigned i = from; i <= to && ok; i++) {
        ok = vmafx_context_import_score(context, feature, i, imported_value(i), NULL) == VMAFX_OK;
    }
    return ok;
}

/* ---- Imported scores --------------------------------------------------------------------- */

static char *check_imported_window(VmafxContext *context, uint64_t first, uint64_t last)
{
    VmafxWindow *window = vw_submit(context, vw_feature(feature), first, last);
    VmafxWindowResult r;
    mu_assert("complete: every frame is final", window && vw_complete(window, &r));
    mu_assert("result fields", r.status == VMAFX_OK && r.flags == 0 && r.first == first &&
                                   r.last == last && r.n_frames == last - first + 1u &&
                                   r.n_scored == r.n_frames &&
                                   r.target == VMAFX_WINDOW_TARGET_FEATURE &&
                                   r.pool_mask == VW_ALL_POOLS && !strcmp(r.name, feature));
    mu_assert("every method equals the synchronous call",
              vw_same_as_sync(context, vw_feature(feature), &r));
    mu_assert("NONE slot unused", r.value[VMAFX_POOL_NONE] == 0.0 && r.stddev[3] == 0.0);
    vmafx_window_release(window);
    return NULL;
}

static char *test_imported_window_equals_sync(void)
{
    VmafxContext *context = plain_context(0, 0);
    mu_assert("session", context && import_range(context, 0, N_IMPORTED - 1));
    mu_assert_msg(check_imported_window(context, 0, N_IMPORTED - 1));
    mu_assert_msg(check_imported_window(context, 0, 0));
    mu_assert_msg(check_imported_window(context, 7, 19));
    mu_assert_msg(check_imported_window(context, N_IMPORTED - 1, N_IMPORTED - 1));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* Import frames `from` down to `to`: the window stays open throughout. */
static char *import_backwards(VmafxContext *context, const VmafxWindow *window, unsigned from,
                              unsigned to)
{
    for (unsigned i = from; i >= to; i--) {
        VmafxWindowResult r;
        mu_assert("import", import_range(context, i, i));
        mu_assert("pending while an earlier frame is missing", !vw_done(window, &r));
    }
    return NULL;
}

/* An open window answers PENDING, without an error, to a poll and to a
 * bounded wait. */
static char *check_pending_answers(const VmafxWindow *window)
{
    VmafxWindowResult r = VMAFX_WINDOW_RESULT_INIT;
    VmafxError *error = NULL;
    mu_assert("PENDING is an answer, not an error",
              vmafx_window_poll(window, &r, &error) == VMAFX_PENDING && !error);
    mu_assert("bounded wait answers PENDING",
              vmafx_window_wait(window, 1000000u, &r, &error) == VMAFX_PENDING && !error);
    return NULL;
}

static char *test_window_waits_for_its_last_frame(void)
{
    VmafxContext *context = plain_context(0, 0);
    mu_assert("context", context);
    VmafxWindow *window = vw_submit(context, vw_feature(feature), 2, 9);
    VmafxWindowResult r;
    mu_assert("open", window && !vw_done(window, &r));
    /* Out of order: complete only when the last missing frame arrives. */
    mu_assert_msg(import_backwards(context, window, 9, 3));
    mu_assert_msg(check_pending_answers(window));
    mu_assert("the last missing frame", import_range(context, 2, 2) && vw_complete(window, &r));
    mu_assert("equal", r.n_frames == 8 && vw_same_as_sync(context, vw_feature(feature), &r));
    vmafx_window_release(window);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Scored frames: flush, subsampling, motion ----------------------------------------------- */

static VmafxContext *psnr_context(uint32_t n_subsample)
{
    VmafxContext *context = plain_context(0, n_subsample);
    if (context && vmafx_context_use_feature(context, "psnr", NULL, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

/* After the flush: a window past the stream's end is partial, one beyond it
 * has no frame. */
static char *check_flushed(VmafxContext *context, const VmafxWindow *past,
                           const VmafxWindow *beyond)
{
    VmafxWindowResult r;
    mu_assert("partial", vw_complete(past, &r) && r.status == VMAFX_OK &&
                             r.flags == VMAFX_WINDOW_PARTIAL && r.n_frames == 6 && r.last == 9 &&
                             vw_same_as_sync(context, vw_feature("psnr_y"), &r));
    mu_assert("no frame of it in the stream",
              vw_complete(beyond, &r) && r.status == VMAFX_E_RANGE &&
                  r.flags == VMAFX_WINDOW_PARTIAL && r.n_frames == 0 && r.value[3] == 0.0);
    VmafxWindow *after = vw_submit(context, vw_feature("psnr_y"), 1, 2);
    const bool done =
        after && vw_complete(after, &r) && vw_same_as_sync(context, vw_feature("psnr_y"), &r);
    vmafx_window_release(after);
    mu_assert("after the flush: complete", done);
    return NULL;
}

/* Before the flush: the window inside the stream is complete, the ones
 * reaching past its end are open. */
static char *check_unflushed(VmafxContext *context, const VmafxWindow *inside,
                             const VmafxWindow *past, const VmafxWindow *beyond)
{
    VmafxWindowResult r;
    mu_assert("inside: complete", vw_complete(inside, &r) && r.flags == 0 &&
                                      vw_same_as_sync(context, vw_feature("psnr_y"), &r));
    mu_assert("past the stream: open", !vw_done(past, &r) && !vw_done(beyond, &r));
    return NULL;
}

static char *test_flush_completes_partial_windows(void)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VtLog log = {0};
    VmafxContext *context = vt_logged_context(&log, VMAFX_LOG_LEVEL_ERROR);
    mu_assert("context",
              context && vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK);
    VmafxWindow *past = vw_submit(context, vw_feature("psnr_y"), 0, 9);
    VmafxWindow *inside = vw_submit(context, vw_feature("psnr_y"), 3, 4);
    VmafxWindow *beyond = vw_submit(context, vw_feature("psnr_y"), 8, 12);
    mu_assert("frames", past && inside && beyond && vw_submit_frames(context, &desc, 0, 5));
    mu_assert_msg(check_unflushed(context, inside, past, beyond));
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    mu_assert_msg(check_flushed(context, past, beyond));
    mu_assert("the window without a frame is logged once", log.errors == 1);
    vmafx_window_release(past);
    vmafx_window_release(inside);
    vmafx_window_release(beyond);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_subsampling_counts_scored_frames(void)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VmafxContext *context = psnr_context(3);
    mu_assert("context", context);
    VmafxWindow *window = vw_submit(context, vw_feature("psnr_y"), 1, 8);
    VmafxWindow *none = vw_submit(context, vw_feature("psnr_y"), 1, 2);
    VmafxWindowResult r;
    mu_assert("frames", window && none && vw_submit_frames(context, &desc, 0, 9));
    mu_assert("frames 3 and 6 scored", vw_complete(window, &r) && r.n_frames == 8 &&
                                           r.n_scored == 2 &&
                                           vw_same_as_sync(context, vw_feature("psnr_y"), &r));
    mu_assert("no scored frame", vw_complete(none, &r) && r.status == VMAFX_E_RANGE &&
                                     r.n_frames == 2 && r.n_scored == 0 && r.flags == 0);
    vmafx_window_release(window);
    vmafx_window_release(none);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* Every frame of [first, last] has a final score of `name` (the synchronous
 * per-frame call answers VMAFX_OK; its "no score yet" error is dropped). */
static bool all_final(VmafxContext *context, const char *name, unsigned first, unsigned last)
{
    bool final = true;
    for (unsigned i = first; i <= last && final; i++) {
        VmafxScore s = VMAFX_SCORE_INIT;
        VmafxError *error = NULL;
        final = vmafx_feature_score(context, name, i, &s, &error) == VMAFX_OK;
        vmafx_error_free(error);
    }
    return final;
}

/* Submit frames one by one, then flush (step `n_frames`): the window
 * completes in the step that makes the last of its frames final, and not
 * before. Returns that step, or UINT_MAX. */
static unsigned completing_step(VmafxContext *context, VmafxWindow *window, const char *name,
                                unsigned last, unsigned n_frames)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    for (unsigned k = 0; k <= n_frames; k++) {
        const bool fed = k < n_frames ? vw_submit_frames(context, &desc, k, k) :
                                        vmafx_flush(context, NULL) == VMAFX_OK;
        VmafxWindowResult r;
        const bool final = fed && all_final(context, name, 0, last);
        /* Never complete before its frames are final; complete soon after. */
        const bool done = final ? vw_complete(window, &r) : vw_done(window, &r);
        if (!fed || done != final) {
            return UINT_MAX; /* early, or late */
        }
        if (final) {
            return vw_same_as_sync(context, vw_feature(name), &r) ? k : UINT_MAX;
        }
    }
    return UINT_MAX;
}

/* motion2 / motion3 of a frame read the SAD of the frame after it; the
 * integer motion extractors derive both for every frame at the flush
 * (motion_window.h, ADR-1478), so a window over them completes there. The
 * test holds the rule, not the step: complete exactly when final. */
static char *test_motion_window_completes_when_its_frames_are_final(void)
{
    static const char *const names[] = {"VMAF_integer_feature_motion2_score",
                                        "VMAF_integer_feature_motion3_score"};
    for (unsigned n = 0; n < 2u; n++) {
        VmafxContext *context = plain_context(0, 0);
        mu_assert("context",
                  context && vmafx_context_use_feature(context, "motion", NULL, NULL) == VMAFX_OK);
        VmafxWindow *window = vw_submit(context, vw_feature(names[n]), 0, 3);
        const unsigned step = completing_step(context, window, names[n], 3, 8);
        mu_assert("completes in the step that makes frame 3 final, after frame 4 at the earliest",
                  step != UINT_MAX && step >= 4u);
        vmafx_window_release(window);
        mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    }
    return NULL;
}

/* ---- Models and model sets ------------------------------------------------------------------- */

typedef struct Models {
    VmafxModel *model;
    VmafxModelSet *set;
} Models;

/* A context scoring vmaf_v0.6.1 and the vmaf_b_v0.6.3 set. */
static VmafxContext *model_context(Models *m, uint32_t n_threads)
{
    VmafxContext *context = plain_context(n_threads, 0);
    const bool ok = context && vmafx_context_use_model(context, m->model, NULL) == VMAFX_OK &&
                    vmafx_context_use_model_set(context, m->set, NULL) == VMAFX_OK;
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

static char *check_models(VmafxContext *windows, VmafxContext *sync, const Models *m)
{
    const VwTarget targets[] = {vw_model(m->model), vw_set(m->set), vw_feature("vmaf")};
    static const uint64_t ranges[][2] = {{0, 5}, {6, 11}, {2, 9}};
    for (unsigned t = 0; t < 3u; t++) {
        for (unsigned g = 0; g < 3u; g++) {
            VmafxWindow *window = vw_submit(windows, targets[t], ranges[g][0], ranges[g][1]);
            VmafxWindowResult r;
            mu_assert("model windows complete after the flush", window && vw_complete(window, &r));
            mu_assert("equal to a separate synchronous session, every method",
                      vw_same_as_sync(sync, targets[t], &r));
            vmafx_window_release(window);
        }
    }
    return NULL;
}

static char *test_model_and_set_windows_equal_a_sync_session(void)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    Models m = {NULL, NULL};
    mu_assert("models", vmafx_model_load(NULL, "vmaf_v0.6.1", &m.model, NULL) == VMAFX_OK &&
                            vmafx_model_set_load(NULL, "vmaf_b_v0.6.3", &m.set, NULL) == VMAFX_OK);
    for (uint32_t threads = 0; threads <= 2u; threads += 2u) {
        VmafxContext *windows = model_context(&m, threads);
        VmafxContext *sync = model_context(&m, 0);
        mu_assert("sessions", windows && sync && vw_submit_frames(windows, &desc, 0, 11) &&
                                  vw_submit_frames(sync, &desc, 0, 11) &&
                                  vmafx_flush(windows, NULL) == VMAFX_OK &&
                                  vmafx_flush(sync, NULL) == VMAFX_OK);
        mu_assert_msg(check_models(windows, sync, &m));
        mu_assert("destroy", vmafx_context_destroy(windows, NULL) == VMAFX_OK &&
                                 vmafx_context_destroy(sync, NULL) == VMAFX_OK);
    }
    vmafx_model_unref(m.model);
    vmafx_model_set_unref(m.set);
    return NULL;
}

/* ---- Many windows, callbacks, release -------------------------------------------------------- */

enum { N_OPEN = 1024 };

/* Every open window completes once its scores are in, with the synchronous
 * values (every 97th checked against the synchronous call). */
static char *check_many(VmafxContext *context, VmafxWindow *const *windows)
{
    mu_assert("scores", import_range(context, 0, 303));
    for (unsigned i = 0; i < N_OPEN; i++) {
        VmafxWindowResult r;
        mu_assert("every window complete", vw_complete(windows[i], &r) && r.status == VMAFX_OK);
        mu_assert("equal", (i % 97u) || vw_same_as_sync(context, vw_feature(feature), &r));
    }
    return NULL;
}

static char *test_many_windows_in_flight(void)
{
    static VmafxWindow *windows[N_OPEN];
    VmafxContext *context = plain_context(0, 0);
    mu_assert("context", context);
    for (unsigned i = 0; i < N_OPEN; i++) {
        windows[i] = vw_submit(context, vw_feature(feature), i % 300u, i % 300u + i % 5u);
        mu_assert("open", windows[i] != NULL);
    }
    const VmafxWindowRequest one_more = vw_request(vw_feature(feature), 0, 0, VW_ALL_POOLS);
    VmafxWindow *refused = NULL;
    VmafxError *error = NULL;
    mu_assert("1025th open window",
              vmafx_window_submit(context, &one_more, &refused, &error) == VMAFX_E_BUSY &&
                  !refused && vt_failed(&error, VMAFX_E_BUSY, "context", VMAFX_SUBJECT_CONTEXT));
    mu_assert_msg(check_many(context, windows));
    for (unsigned i = 0; i < N_OPEN; i++) {
        vmafx_window_release(windows[i]);
    }
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

typedef struct Calls {
    unsigned count;
    VmafxStatus status;
    double mean;
    bool release_self;
} Calls;

static void count_call(VmafxWindow *window, const VmafxWindowResult *result, void *user)
{
    Calls *const calls = user;
    calls->count++;
    calls->status = result->status;
    calls->mean = result->value[VMAFX_POOL_MEAN];
    if (calls->release_self) {
        vmafx_window_release(window);
    }
}

static VmafxWindow *submit_with_callback(VmafxContext *context, uint64_t first, uint64_t last,
                                         Calls *calls)
{
    VmafxWindowRequest r = vw_request(vw_feature(feature), first, last, VW_ALL_POOLS);
    r.on_complete = count_call;
    r.user = calls;
    VmafxWindow *window = NULL;
    return vmafx_window_submit(context, &r, &window, NULL) == VMAFX_OK ? window : NULL;
}

static char *test_callbacks_run_once(void)
{
    /* Static: a failed assertion returns with the window thread alive. */
    static Calls done = {0, 0, 0.0, false};
    static Calls cancelled = {0, 0, 0.0, false};
    static Calls self = {0, 0, 0.0, true};
    VmafxContext *context = plain_context(0, 0);
    VmafxWindow *window = context ? submit_with_callback(context, 0, 9, &done) : NULL;
    VmafxWindow *gone = context ? submit_with_callback(context, 0, 9, &cancelled) : NULL;
    VmafxWindow *own = context ? submit_with_callback(context, 3, 4, &self) : NULL;
    mu_assert("open", window && gone && own);
    vmafx_window_release(gone); /* cancelled before it completes */
    mu_assert("scores", import_range(context, 0, 9));
    VmafxWindowResult r = VMAFX_WINDOW_RESULT_INIT;
    mu_assert("wait from the feeding thread after the frames",
              vmafx_window_wait(window, UINT64_MAX, &r, NULL) == VMAFX_OK);
    mu_assert("destroy joins the window thread", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    mu_assert("one callback with the polled result",
              done.count == 1 && done.status == VMAFX_OK &&
                  vt_same_bits(done.mean, r.value[VMAFX_POOL_MEAN]));
    mu_assert("a released window never calls back", cancelled.count == 0);
    mu_assert("released from its own callback", self.count == 1);
    vmafx_window_release(window);
    return NULL;
}

static char *test_destroy_completes_open_windows(void)
{
    static Calls calls = {0, 0, 0.0, false};
    VmafxContext *context = plain_context(0, 0);
    VmafxWindow *window = context ? submit_with_callback(context, 0, 9, &calls) : NULL;
    mu_assert("open", window && import_range(context, 0, 4));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    VmafxWindowResult r;
    mu_assert("completed by the destroy",
              calls.count == 1 && calls.status == VMAFX_E_INVALID && vw_complete(window, &r) &&
                  r.status == VMAFX_E_INVALID && r.flags == VMAFX_WINDOW_PARTIAL);
    vmafx_window_release(window); /* outlives its context */
    return NULL;
}

/* ---- In flight ------------------------------------------------------------------------------ */

static char *test_max_in_flight(void)
{
    VmafxContext *single = plain_context(0, 0);
    VmafxContext *threaded = plain_context(3, 0);
    mu_assert("contexts", single && threaded);
    mu_assert("no worker: the retained frame",
              vmafx_context_max_in_flight(single) == vmafx_context_frame_retention(single));
    const uint32_t r = vmafx_context_frame_retention(threaded);
    mu_assert("3 workers: 6 jobs in flight, each with its retained frames",
              vmafx_context_max_in_flight(threaded) == r + 6u * (r + 1u));
    mu_assert("NULL", vmafx_context_max_in_flight(NULL) == 0u);
    mu_assert("destroy", vmafx_context_destroy(single, NULL) == VMAFX_OK &&
                             vmafx_context_destroy(threaded, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Refusals ------------------------------------------------------------------------------- */

static bool refused(VmafxContext *context, VmafxWindowRequest r, VmafxStatus status,
                    const char *subject, uint32_t kind)
{
    VmafxWindow *window = NULL;
    VmafxError *error = NULL;
    return vmafx_window_submit(context, &r, &window, &error) == status && !window &&
           vt_failed(&error, status, subject, kind);
}

static char *check_argument_refusals(VmafxContext *context)
{
    VmafxWindowRequest r = vw_request(vw_feature(feature), 0, 1, VW_ALL_POOLS);
    VmafxWindow *window = NULL;
    VmafxError *error = NULL;
    mu_assert("NULL request",
              vmafx_window_submit(context, NULL, &window, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "request", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL out", vmafx_window_submit(context, &r, NULL, &error) == VMAFX_E_INVALID &&
                              vt_failed(&error, VMAFX_E_INVALID, "out", VMAFX_SUBJECT_PARAMETER));
    r.struct_size = 8u;
    mu_assert("struct below its first size",
              refused(context, r, VMAFX_E_ABI, "request", VMAFX_SUBJECT_PARAMETER));
    VmafxWindowResult out = VMAFX_WINDOW_RESULT_INIT;
    mu_assert("poll NULL",
              vmafx_window_poll(NULL, &out, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "window", VMAFX_SUBJECT_PARAMETER));
    vmafx_window_release(NULL);
    return NULL;
}

static char *check_request_refusals(VmafxContext *context)
{
    const VwTarget f = vw_feature(feature);
    VmafxWindowRequest r = vw_request(f, 0, 1, VW_ALL_POOLS);
    r.target = 9;
    mu_assert("target",
              refused(context, r, VMAFX_E_INVALID, "request.target", VMAFX_SUBJECT_PARAMETER));
    mu_assert("no model", refused(context, vw_request(vw_model(NULL), 0, 1, VW_ALL_POOLS),
                                  VMAFX_E_INVALID, "request.model", VMAFX_SUBJECT_PARAMETER));
    static const uint32_t bad_masks[] = {0u, 1u, 1u << 9u, VW_ALL_POOLS | 1u};
    for (unsigned i = 0; i < 4u; i++) {
        mu_assert("pool mask", refused(context, vw_request(f, 0, 1, bad_masks[i]), VMAFX_E_INVALID,
                                       "request.pool_mask", VMAFX_SUBJECT_PARAMETER));
    }
    mu_assert("first after last", refused(context, vw_request(f, 2, 1, VW_ALL_POOLS),
                                          VMAFX_E_INVALID, "request.first", VMAFX_SUBJECT_FRAME));
    mu_assert("past the engine's index",
              refused(context, vw_request(f, 0, (uint64_t)UINT_MAX + 1u, VW_ALL_POOLS),
                      VMAFX_E_RANGE, "request.last", VMAFX_SUBJECT_FRAME));
    return NULL;
}

static char *test_refusals_are_named(void)
{
    VmafxContext *context = plain_context(0, 0);
    mu_assert("context", context);
    mu_assert_msg(check_argument_refusals(context));
    mu_assert_msg(check_request_refusals(context));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_imported_window_equals_sync),
        MU_TEST(test_window_waits_for_its_last_frame),
        MU_TEST(test_flush_completes_partial_windows),
        MU_TEST(test_subsampling_counts_scored_frames),
        MU_TEST(test_motion_window_completes_when_its_frames_are_final),
        MU_TEST(test_model_and_set_windows_equal_a_sync_session),
        MU_TEST(test_many_windows_in_flight),
        MU_TEST(test_callbacks_run_once),
        MU_TEST(test_destroy_completes_open_windows),
        MU_TEST(test_max_in_flight),
        MU_TEST(test_refusals_are_named),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
