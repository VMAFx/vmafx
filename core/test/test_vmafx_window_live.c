/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A headless live-plugin harness for the VMAFx window scores (RC4 WP4,
 * ADR-2074, #2238): the call pattern of a live-production plugin, on the CPU
 * device.
 *
 * - A render thread (this one) paces frames at 60 per second. Each frame is
 *   "rendered" into a texture of a ring (host buffers here; GPU textures on
 *   the device lanes), its acquire fence signalled, imported with
 *   vmafx_context_import_frame() and submitted; the ring has
 *   vmafx_context_max_in_flight() + 1 textures and a texture is reused only
 *   after its release fence is signalled.
 * - A window clock cuts the stream into windows of 0.2 s (#2138 n_stats);
 *   each closed window is submitted for the model and for each of its
 *   features.
 * - A poller thread polls every window with vmafx_window_poll() and records
 *   when it saw it complete.
 *
 * Holds: every window equals an offline session on the same frames bit for
 * bit; a window whose frames are final per frame completes within two frame
 * periods of its last frame's submit (a wall-clock check: held unless
 * VMAFX_TEST_TIMING is 0; the `timing` suite holds it and runs alone, the
 * `fast` suite sets 0, and test_vmafx_live_pacing.c checks the pacing and
 * budget arithmetic of vmafx_live_pacing.h exactly); the textures the
 * library holds never exceed vmafx_context_max_in_flight(), also when the
 * producer is not paced (backpressure); no frame is copied through the
 * host. Run with 0 and 2 worker threads. Windows over motion2 / motion3
 * (and models that read them) complete at the flush: the integer motion
 * extractors derive those for every frame there (motion_window.h, ADR-1478;
 * docs/state.md T-VMAFX-WINDOW-MOTION-AT-FLUSH-2026-10-06).
 *
 * POSIX threads and clocks, the host-copy counter of the static library:
 * Linux only (core/test/meson.build). Fixtures: the Netflix 576x324 pair
 * (skipped with 77 without them).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gpu_dispatch_env.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_fixture_util.h"
#include "vmafx_import_test_util.h"
#include "vmafx_live_pacing.h"
#include "vmafx_test_util.h"
#include "vmafx_window_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { MAX_WINDOWS = 96, MAX_RING = 32, MAX_FRAMES = 64, MAX_FEATURES = 8 };

#define WINDOW_S 0.2                /* n_stats: 12 frames at 60 fps */
#define FENCE_WAIT_NS 1000000000ull /* a texture's release, at most */
#define POLL_SLEEP_NS 200000u
#define POLL_ROUNDS 300000u /* 60 s of polling */
#define MODEL "vmaf_v0.6.1"
#define AHEAD 12u /* frames of a window submitted ahead */

/* The latency budget is held only in a plain optimised build: Meson passes
 * VMAFX_TIMING_BUILD=0 to a build with coverage counters, a sanitizer or no
 * optimisation, which runs the harness for everything but its timing
 * (vmafx_timing_build in core/test/meson.build). */
#ifndef VMAFX_TIMING_BUILD
#error "VMAFX_TIMING_BUILD comes from core/test/meson.build"
#endif
#define TIMED_BUILD VMAFX_TIMING_BUILD

/* How a window of the harness was submitted. */
enum { SPAN_CLOCK, SPAN_AHEAD, SPAN_AT_FLUSH };

typedef struct LiveWindow {
    VmafxWindow *window;
    VwTarget target;
    uint64_t last;
    bool at_flush;
    bool ahead; /* submitted before its frames (not cut by the clock) */
    uint64_t done_ns;
    VmafxWindowResult result;
} LiveWindow;

typedef struct Slot {
    uint8_t *data[2]; /* reference, distorted */
    VmafxFence released[2];
    bool busy;
} Slot;

typedef struct Live {
    VtClip clip;
    uint32_t n_threads;
    bool paced;
    VmafxContext *context;
    VmafxModel *model;
    const char *features[MAX_FEATURES];
    unsigned n_features;
    VmafxWindowClock *clock;
    Slot ring[MAX_RING];
    unsigned n_ring;
    uint32_t max_in_flight;
    unsigned worst_in_flight;
    uint64_t start_ns;
    uint64_t submitted_ns[MAX_FRAMES];
    uint64_t flushed_ns;
    pthread_mutex_t lock;
    LiveWindow windows[MAX_WINDOWS];
    unsigned n_windows;
    bool producer_done;
    bool failed;
} Live;

static uint64_t now_ns(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t monotonic_now(void *self)
{
    (void)self;
    return now_ns();
}

static void monotonic_sleep_until(void *self, uint64_t deadline_ns)
{
    (void)self;
    const struct timespec t = {.tv_sec = (time_t)(deadline_ns / 1000000000u),
                               .tv_nsec = (long)(deadline_ns % 1000000000u)};
    (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
}

/* The harness paces on the monotonic clock; test_vmafx_live_pacing.c runs
 * the same arithmetic (vmafx_live_pacing.h) on a virtual one. */
static const VlClock monotonic_clock = {monotonic_now, monotonic_sleep_until, NULL};

/* The wall-clock budget is held in the `timing` suite, which runs alone
 * (VMAFX_TEST_TIMING=1, the default); the `fast` suite sets
 * VMAFX_TEST_TIMING=0 and checks everything else (maintainer decision
 * Q-325). Read through the once-only environment snapshot (ADR-0488). */
static bool timed(void)
{
    const char *const v = vmaf_gpu_dispatch_env_get("VMAFX_TEST_TIMING");
    return TIMED_BUILD && !(v && v[0] == '0' && v[1] == '\0');
}

/* ---- Set-up ---------------------------------------------------------------------------- */

static bool open_context(Live *live)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.n_threads = live->n_threads;
    if (vmafx_context_create(&config, &live->context, NULL) != VMAFX_OK ||
        vmafx_model_load(NULL, MODEL, &live->model, NULL) != VMAFX_OK ||
        vmafx_context_use_model(live->context, live->model, NULL) != VMAFX_OK) {
        return false;
    }
    for (unsigned i = 0; i < MAX_FEATURES; i++) {
        live->features[i] = vmafx_model_feature_name(live->model, i);
        live->n_features += live->features[i] != NULL;
    }
    VmafxWindowClockConfig clock = VMAFX_WINDOW_CLOCK_CONFIG_INIT;
    clock.n_stats = WINDOW_S;
    live->max_in_flight = vmafx_context_max_in_flight(live->context);
    live->n_ring = live->max_in_flight + 1u; /* and the texture being rendered */
    return live->n_features > 0 && live->n_ring <= MAX_RING &&
           vmafx_window_clock_create(&clock, &live->clock, NULL) == VMAFX_OK;
}

static bool open_ring(Live *live)
{
    const size_t bytes = vt_frame_bytes(&live->clip.desc);
    for (unsigned s = 0; s < live->n_ring; s++) {
        for (unsigned k = 0; k < 2u; k++) {
            live->ring[s].data[k] = malloc(bytes);
            if (!live->ring[s].data[k]) {
                return false;
            }
        }
    }
    return true;
}

static void close_live(Live *live)
{
    for (unsigned s = 0; s < live->n_ring; s++) {
        free(live->ring[s].data[0]);
        free(live->ring[s].data[1]);
    }
    for (unsigned w = 0; w < live->n_windows; w++) {
        vmafx_window_release(live->windows[w].window);
    }
    vmafx_window_clock_destroy(live->clock);
    vmafx_model_unref(live->model);
    (void)pthread_mutex_destroy(&live->lock);
}

/* ---- The render thread ------------------------------------------------------------------ */

/* Wait for the texture's release (the library stopped reading it). */
static bool reuse_slot(Slot *slot)
{
    bool ok = true;
    for (unsigned k = 0; k < 2u && slot->busy; k++) {
        ok = ok && vmafx_fence_wait(&slot->released[k], FENCE_WAIT_NS, NULL) == VMAFX_OK;
        (void)vmafx_fence_destroy(&slot->released[k], NULL);
    }
    slot->busy = false;
    return ok;
}

/* "Render" input `k` of frame `i` into the slot, import it under the
 * import rule with its acquire fence, and ask for its release fence. */
static VmafxFrame *import_texture(Live *live, Slot *slot, unsigned k, unsigned i)
{
    const size_t bytes = vt_frame_bytes(&live->clip.desc);
    const uint8_t *const src = (k == 0u ? live->clip.ref : live->clip.dist) + bytes * i;
    memcpy(slot->data[k], src, bytes);
    VmafxFrameImport imp = vt_import_planar(&live->clip.desc, slot->data[k]);
    VmafxFrame *frame = NULL;
    if (vmafx_fence_create(NULL, VMAFX_FENCE_HOST, &imp.acquire, NULL) != VMAFX_OK) {
        return NULL;
    }
    (void)vmafx_fence_signal(&imp.acquire, NULL); /* the render finished */
    const VmafxStatus status = vmafx_context_import_frame(
        live->context, NULL, &imp, k == 0u ? "reference" : "main", &frame, NULL);
    (void)vmafx_fence_destroy(&imp.acquire, NULL);
    slot->released[k] = (VmafxFence)VMAFX_FENCE_INIT;
    if (status != VMAFX_OK ||
        vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &slot->released[k], NULL) != VMAFX_OK) {
        vmafx_frame_unref(frame);
        return NULL;
    }
    return frame;
}

/* Textures of either input whose release fence is not signalled: held by
 * the library; the larger count of the two inputs. */
static unsigned textures_held(const Live *live)
{
    unsigned held[2] = {0, 0};
    for (unsigned s = 0; s < live->n_ring; s++) {
        const Slot *const slot = &live->ring[s];
        for (unsigned k = 0; k < 2u; k++) {
            held[k] += slot->busy && vmafx_fence_wait(&slot->released[k], 0, NULL) == VMAFX_PENDING;
        }
    }
    return held[0] > held[1] ? held[0] : held[1];
}

static bool open_live_window(Live *live, VwTarget target, const VmafxWindowSpan *span,
                             unsigned kind)
{
    VmafxWindow *window = vw_submit(live->context, target, span->first, span->last);
    if (!window) {
        return false;
    }
    (void)pthread_mutex_lock(&live->lock);
    const bool room = live->n_windows < MAX_WINDOWS;
    if (room) {
        LiveWindow *const w = &live->windows[live->n_windows];
        memset(w, 0, sizeof(*w));
        w->window = window;
        w->target = target;
        w->last = span->last;
        w->at_flush = kind == SPAN_AT_FLUSH;
        w->ahead = kind == SPAN_AHEAD;
        live->n_windows++;
    }
    (void)pthread_mutex_unlock(&live->lock);
    if (!room) {
        vmafx_window_release(window);
    }
    return room;
}

/* One window per target: the model and each of its features. */
static bool open_span(Live *live, const VmafxWindowSpan *span, unsigned kind)
{
    bool ok = open_live_window(live, vw_model(live->model), span, kind);
    for (unsigned f = 0; f < live->n_features && ok; f++) {
        ok = open_live_window(live, vw_feature(live->features[f]), span, kind);
    }
    return ok;
}

/* Windows of AHEAD frames known before their frames arrive, as a plugin
 * with windows by frame count submits them: they complete in the submit
 * that makes their last frame final. */
static bool open_ahead(Live *live, unsigned i)
{
    if (i % AHEAD) {
        return true;
    }
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    span.first = i;
    span.last = i + AHEAD - 1u < live->clip.n_frames ? i + AHEAD - 1u : live->clip.n_frames - 1u;
    return open_span(live, &span, SPAN_AHEAD);
}

static bool produce_frame(Live *live, unsigned i)
{
    Slot *const slot = &live->ring[i % live->n_ring];
    if (live->paced) {
        (void)vl_pace(&monotonic_clock, live->start_ns, i);
    }
    if (!reuse_slot(slot)) {
        return false; /* the library held more textures than the ring */
    }
    VmafxFrame *ref = import_texture(live, slot, 0, i);
    VmafxFrame *dist = ref ? import_texture(live, slot, 1, i) : NULL;
    slot->busy = dist != NULL;
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    const int64_t pts = (int64_t)((uint64_t)i * 1000000000u / 60u);
    const VmafxStatus closed = vmafx_window_clock_frame(live->clock, i, pts, &span, NULL);
    if (!dist || (closed == VMAFX_OK && !open_span(live, &span, SPAN_CLOCK)) ||
        !open_ahead(live, i)) {
        vmafx_frame_unref(ref);
        return false;
    }
    live->submitted_ns[i] = now_ns(); /* the submit of frame i starts */
    const bool ok = vmafx_submit(live->context, ref, dist, i, NULL) == VMAFX_OK;
    const unsigned held = textures_held(live);
    live->worst_in_flight = held > live->worst_in_flight ? held : live->worst_in_flight;
    return ok;
}

static bool produce(Live *live)
{
    live->start_ns = now_ns();
    bool ok = live->clip.n_frames <= MAX_FRAMES;
    for (unsigned i = 0; i < live->clip.n_frames && ok; i++) {
        ok = produce_frame(live, i);
    }
    ok = ok && vmafx_flush(live->context, NULL) == VMAFX_OK;
    live->flushed_ns = now_ns();
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    ok = ok && vmafx_window_clock_finish(live->clock, &span, NULL) == VMAFX_OK &&
         open_span(live, &span, SPAN_AT_FLUSH);
    (void)pthread_mutex_lock(&live->lock);
    live->producer_done = true;
    live->failed = !ok;
    (void)pthread_mutex_unlock(&live->lock);
    return ok;
}

/* After the context is gone every texture is released. */
static bool ring_released(Live *live)
{
    bool ok = true;
    for (unsigned s = 0; s < live->n_ring; s++) {
        ok = reuse_slot(&live->ring[s]) && ok;
    }
    return ok;
}

/* ---- The poller thread ------------------------------------------------------------------- */

/* Poll every open window once; true when all are complete and the producer
 * is done. */
static bool poll_round(Live *live)
{
    (void)pthread_mutex_lock(&live->lock);
    const unsigned n = live->n_windows;
    const bool producer_done = live->producer_done;
    (void)pthread_mutex_unlock(&live->lock);
    bool all = true;
    for (unsigned w = 0; w < n; w++) {
        LiveWindow *const lw = &live->windows[w];
        if (!lw->done_ns && vw_done(lw->window, &lw->result)) {
            lw->done_ns = now_ns();
        }
        all = all && lw->done_ns != 0;
    }
    return all && producer_done;
}

static void *poller(void *arg)
{
    Live *const live = arg;
    for (unsigned round = 0; round < POLL_ROUNDS && !poll_round(live); round++) {
        const struct timespec t = {.tv_sec = 0, .tv_nsec = POLL_SLEEP_NS};
        (void)nanosleep(&t, NULL);
    }
    return NULL;
}

/* ---- Checks -------------------------------------------------------------------------------- */

/* The same frames scored offline: host frames, synchronous, flushed. */
static VmafxContext *offline_session(const Live *live)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK ||
        vmafx_context_use_model(context, live->model, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    const size_t bytes = vt_frame_bytes(&live->clip.desc);
    bool ok = true;
    for (unsigned i = 0; i < live->clip.n_frames && ok; i++) {
        VmafxFrame *ref = vt_copy_frame(&live->clip.desc, live->clip.ref + bytes * i);
        VmafxFrame *dist = vt_copy_frame(&live->clip.desc, live->clip.dist + bytes * i);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    if (!ok || vmafx_flush(context, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

static bool final_per_frame(const LiveWindow *w)
{
    return w->target.kind == VMAFX_WINDOW_TARGET_FEATURE && !strstr(w->target.feature, "motion");
}

/* The latency of a live window: from the start of the submit of its last
 * frame to the poller seeing it complete. */
static uint64_t latency_of(const Live *live, const LiveWindow *lw)
{
    return vl_latency(live->submitted_ns[lw->last], lw->done_ns);
}

static char *check_windows(const Live *live, VmafxContext *offline)
{
    VlLedger ledger = {{0, 0}, 0, 0}; /* clock windows, windows submitted ahead */
    const bool budget = timed();
    for (unsigned w = 0; w < live->n_windows; w++) {
        const LiveWindow *const lw = &live->windows[w];
        mu_assert("every window completes", lw->done_ns != 0 && lw->result.status == VMAFX_OK);
        mu_assert("equal to the offline session, every method",
                  vw_same_as_sync(offline, lw->target, &lw->result));
        if (!lw->at_flush && final_per_frame(lw)) {
            const uint64_t latency = latency_of(live, lw);
            vl_record(&ledger, lw->ahead, latency);
            mu_assert("within two frame periods of the submit of its last frame",
                      !budget || vl_within_budget(latency));
        }
    }
    mu_assert("live windows were measured", ledger.measured >= 8u);
    (void)fprintf(stderr,
                  "  %u threads %s: %u windows, %u live; worst latency %.2f ms submitted ahead, "
                  "%.2f ms cut by the clock (one frame later by rule; budget %s); textures held "
                  "at most %u of %u\n",
                  live->n_threads, live->paced ? "paced" : "unpaced", live->n_windows,
                  ledger.measured, (double)ledger.worst[1] / 1e6, (double)ledger.worst[0] / 1e6,
                  budget ? "held" : "not held: timing suite", live->worst_in_flight,
                  live->max_in_flight);
    return NULL;
}

/* With VMAFX_WINDOW_JSON set, write the windows there, one JSON object per
 * line with every value at 17 digits, for test_vmafx_window_cli.py to hold
 * against the CLI's per-frame scores. */
static bool dump_windows(const Live *live)
{
    /* The once-only environment snapshot (ADR-0488), not getenv(). */
    const char *const path = vmaf_gpu_dispatch_env_get("VMAFX_WINDOW_JSON");
    FILE *const file = path ? fopen(path, "w") : NULL;
    if (!path) {
        return true;
    }
    bool ok = file != NULL;
    for (unsigned w = 0; w < live->n_windows && ok; w++) {
        const LiveWindow *const lw = &live->windows[w];
        const VmafxWindowResult *const r = &lw->result;
        ok = fprintf(file, "{\"target\": \"%s\", \"first\": %llu, \"n_frames\": %llu, \"value\": [",
                     r->name, (unsigned long long)r->first, (unsigned long long)r->n_frames) > 0;
        for (unsigned p = 0; p < 9u && ok; p++) {
            ok = fprintf(file, p ? ", %.17g" : "%.17g", r->value[p]) > 0;
        }
        ok = ok && fprintf(file, "]}\n") > 0;
    }
    return file && fclose(file) == 0 && ok;
}

/* Stream the clip with the poller running; the contexts stay open. */
static char *stream(Live *live)
{
    mu_assert("context", open_context(live) && open_ring(live));
    vmafx_test_reset_counters();
    pthread_t thread;
    mu_assert("poller", pthread_create(&thread, NULL, poller, live) == 0);
    const bool produced = produce(live);
    mu_assert("joined", pthread_join(thread, NULL) == 0);
    mu_assert("produced, every texture released in time", produced && !live->failed);
    mu_assert("no host copy", vmafx_test_host_copies() == 0);
    mu_assert("backpressure: textures held within the bound",
              live->worst_in_flight <= live->max_in_flight);
    return NULL;
}

/* Every window against an offline session; the windows written for the CLI
 * comparison once (no workers, paced). */
static char *check_against_offline(Live *live)
{
    VmafxContext *offline = offline_session(live);
    mu_assert("offline", offline);
    mu_message_t msg = check_windows(live, offline);
    const bool dumped = live->n_threads != 0u || !live->paced || dump_windows(live);
    mu_assert("destroy offline", vmafx_context_destroy(offline, NULL) == VMAFX_OK);
    mu_assert_msg(msg);
    mu_assert("windows written", dumped);
    return NULL;
}

static char *run_live(uint32_t n_threads, bool paced)
{
    static Live live_state;
    Live *const live = &live_state;
    memset(live, 0, sizeof(*live));
    live->n_threads = n_threads;
    live->paced = paced;
    if (!vt_clip_open(&live->clip, &vt_inputs[0])) {
        vt_clip_close(&live->clip);
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("lock", pthread_mutex_init(&live->lock, NULL) == 0);
    mu_message_t msg = stream(live);
    msg = msg ? msg : check_against_offline(live);
    const bool destroyed = !live->context || vmafx_context_destroy(live->context, NULL) == VMAFX_OK;
    const bool released = ring_released(live);
    close_live(live);
    vt_clip_close(&live->clip);
    mu_assert_msg(msg);
    mu_assert("destroy", destroyed);
    mu_assert("every texture released", released);
    return NULL;
}

static char *test_live_without_workers(void)
{
    return run_live(0, true);
}

static char *test_live_with_workers(void)
{
    return run_live(2, true);
}

static char *test_unpaced_producer_is_held_back(void)
{
    return run_live(2, false);
}

/* ---- Release against delivery -------------------------------------------------------------- */

enum { N_RACE = 256 };

typedef struct Race {
    VmafxWindow *windows[N_RACE];
    atomic_int released[N_RACE]; /* set once vmafx_window_release() returned */
    atomic_int late;             /* callbacks running after their release returned */
    atomic_int calls;
} Race;

typedef struct RaceCall {
    Race *race;
    unsigned index;
} RaceCall;

static void race_callback(VmafxWindow *window, const VmafxWindowResult *result, void *user)
{
    (void)window;
    (void)result;
    const RaceCall *const call = user;
    if (atomic_load(&call->race->released[call->index])) {
        atomic_fetch_add(&call->race->late, 1);
    }
    const struct timespec t = {.tv_sec = 0, .tv_nsec = 20000};
    (void)nanosleep(&t, NULL); /* hold the delivery while the releaser runs */
    if (atomic_load(&call->race->released[call->index])) {
        atomic_fetch_add(&call->race->late, 1); /* the release did not wait */
    }
    atomic_fetch_add(&call->race->calls, 1);
}

static void *releaser(void *arg)
{
    Race *const race = arg;
    for (unsigned i = 0; i < N_RACE; i++) {
        vmafx_window_release(race->windows[(i * 7u) % N_RACE]); /* out of order */
        atomic_store(&race->released[(i * 7u) % N_RACE], 1);
    }
    return NULL;
}

/* Windows complete on the feeding thread and call back on the window thread
 * while a third thread releases them: no callback runs after its window's
 * release returned, neither one started later nor one still running (TSan
 * watches the rest). */
/* N_RACE windows of one frame each over frames 0 to 15, with callbacks. */
static char *submit_race_windows(VmafxContext *context, Race *race, RaceCall *calls)
{
    for (unsigned i = 0; i < N_RACE; i++) {
        calls[i] = (RaceCall){race, i};
        atomic_init(&race->released[i], 0);
        VmafxWindowRequest r = vw_request(vw_feature("race"), i % 16u, i % 16u, VW_ALL_POOLS);
        r.on_complete = race_callback;
        r.user = &calls[i];
        mu_assert("window", vmafx_window_submit(context, &r, &race->windows[i], NULL) == VMAFX_OK);
    }
    return NULL;
}

/* Scores of frames 0 to 15: every race window completes. */
static bool import_race_scores(VmafxContext *context)
{
    bool scored = true;
    for (unsigned i = 0; i < 16u && scored; i++) {
        scored = vmafx_context_import_score(context, "race", i, (double)i, NULL) == VMAFX_OK;
    }
    return scored;
}

static char *test_release_races_delivery(void)
{
    static Race race;
    static RaceCall calls[N_RACE];
    VmafxContext *context = NULL;
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    mu_assert_msg(submit_race_windows(context, &race, calls));
    pthread_t thread;
    mu_assert("releaser", pthread_create(&thread, NULL, releaser, &race) == 0);
    const bool scored = import_race_scores(context);
    mu_assert("joined", pthread_join(thread, NULL) == 0);
    mu_assert("scores", scored);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    mu_assert("no callback after its release",
              atomic_load(&race.late) == 0 && atomic_load(&race.calls) <= N_RACE);
    return NULL;
}

/* ---- A released window waiting in the callback queue ------------------------------------- */

typedef struct Gate {
    atomic_int entered; /* window A's callback runs */
    atomic_int open;    /* it may return */
    atomic_int b_calls;
} Gate;

static void blocking_callback(VmafxWindow *window, const VmafxWindowResult *result, void *user)
{
    (void)window;
    (void)result;
    Gate *const gate = user;
    atomic_store(&gate->entered, 1);
    for (unsigned i = 0; i < POLL_ROUNDS && !atomic_load(&gate->open); i++) {
        const struct timespec t = {.tv_sec = 0, .tv_nsec = POLL_SLEEP_NS};
        (void)nanosleep(&t, NULL);
    }
}

static void counting_callback(VmafxWindow *window, const VmafxWindowResult *result, void *user)
{
    (void)window;
    (void)result;
    atomic_fetch_add(&((Gate *)user)->b_calls, 1);
}

static VmafxWindow *gated_window(VmafxContext *context, uint64_t index,
                                 VmafxWindowCallback callback, Gate *gate)
{
    VmafxWindowRequest r = vw_request(vw_feature("gate"), index, index, VMAFX_POOL_MASK_MEAN);
    r.on_complete = callback;
    r.user = gate;
    VmafxWindow *window = NULL;
    return vmafx_window_submit(context, &r, &window, NULL) == VMAFX_OK ? window : NULL;
}

/* Wait until window A's callback runs (bounded). */
static bool wait_entered(Gate *gate)
{
    for (unsigned i = 0; i < POLL_ROUNDS && !atomic_load(&gate->entered); i++) {
        const struct timespec t = {.tv_sec = 0, .tv_nsec = POLL_SLEEP_NS};
        (void)nanosleep(&t, NULL);
    }
    return atomic_load(&gate->entered) != 0;
}

/* Complete A, wait until its callback holds the window thread, then
 * complete B, which waits in the callback queue. */
static char *complete_a_then_b(VmafxContext *context, Gate *gate, const VmafxWindow *b)
{
    mu_assert("A completes", vmafx_context_import_score(context, "gate", 0, 1.0, NULL) == VMAFX_OK);
    mu_assert("A's callback runs", wait_entered(gate));
    mu_assert("B completes, queued",
              vmafx_context_import_score(context, "gate", 1, 2.0, NULL) == VMAFX_OK);
    VmafxWindowResult r;
    mu_assert("B is complete", vw_complete(b, &r));
    return NULL;
}

/* Window B completes while A's callback holds the window thread, so B waits
 * in the callback queue; released there, B never calls back. */

static char *test_release_of_a_queued_window(void)
{
    static Gate gate;
    VmafxContext *context = NULL;
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    VmafxWindow *a = gated_window(context, 0, blocking_callback, &gate);
    VmafxWindow *b = gated_window(context, 1, counting_callback, &gate);
    mu_assert("windows", a && b);
    mu_assert_msg(complete_a_then_b(context, &gate, b));
    vmafx_window_release(b);
    atomic_store(&gate.open, 1);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    mu_assert("B never called back", atomic_load(&gate.b_calls) == 0);
    vmafx_window_release(a);
    return NULL;
}

/* ---- A stalled feeder --------------------------------------------------------------- */

enum { N_STALL = 16 };

/* One frame of 576x324 content, frame `i`; NULL on failure. */
static bool stall_submit(VmafxContext *context, unsigned i)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, 576, 324);
    return vw_submit_frames(context, &desc, i, i);
}

/* Window [i, i] of ADM, then frame i: the feeder makes no further call and
 * waits; the window completes anyway, within the budget of the submit's
 * start. Counts the windows still open when the submit returned (their frame
 * was on a worker): those completed through the frame listener alone. */
static char *stall_one(VmafxContext *context, unsigned i, unsigned *pending_at_return,
                       uint64_t *worst)
{
    VmafxWindow *window = vw_submit(context, vw_feature("VMAF_integer_feature_adm2_score"), i, i);
    mu_assert("window", window != NULL);
    const uint64_t began = now_ns();
    mu_assert("frame", stall_submit(context, i));
    VmafxWindowResult r;
    *pending_at_return += !vw_done(window, &r);
    /* The feeder stalls: no submit, flush or import until the window is in. */
    const bool done =
        vmafx_window_wait(window, timed() ? VL_BUDGET_NS : VW_WAIT_NS, &r, NULL) == VMAFX_OK;
    const uint64_t latency = now_ns() - began;
    *worst = latency > *worst ? latency : *worst;
    vmafx_window_release(window);
    mu_assert("complete while the feeder waits, within the budget", done && r.status == VMAFX_OK);
    return NULL;
}

/* ADR-2074 (maintainer decision): windows complete independently of the
 * feeding thread. With worker threads the frame of a window finishes after
 * the submit returned; the completion thread, woken by the worker, completes
 * it while the feeder waits instead of feeding. */
static char *test_window_completes_while_the_feeder_stalls(void)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.n_threads = 2;
    VmafxContext *context = NULL;
    VmafxModel *model = NULL;
    mu_assert("context", vmafx_context_create(&config, &context, NULL) == VMAFX_OK &&
                             vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK &&
                             vmafx_context_use_model(context, model, NULL) == VMAFX_OK);
    vmafx_model_unref(model);
    unsigned pending_at_return = 0;
    uint64_t worst = 0;
    for (unsigned i = 0; i < N_STALL; i++) {
        mu_assert_msg(stall_one(context, i, &pending_at_return, &worst));
    }
    (void)fprintf(stderr,
                  "  %u of %u windows completed after their submit returned, worst %.2f ms\n",
                  pending_at_return, (unsigned)N_STALL, (double)worst / 1e6);
    mu_assert("the stall was exercised: a frame finished on a worker", pending_at_return > 0u);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Destroy with a pending wake ---------------------------------------------------- */

enum { N_DESTROY_ROUNDS = 64 };

typedef struct DestroyCalls {
    atomic_int final_ok;  /* callbacks of windows whose scores were all in */
    atomic_int destroyed; /* callbacks of windows completed by the destroy */
    atomic_int other;
} DestroyCalls;

static void destroy_callback(VmafxWindow *window, const VmafxWindowResult *result, void *user)
{
    (void)window;
    DestroyCalls *const calls = user;
    if (result->status == VMAFX_OK) {
        atomic_fetch_add(&calls->final_ok, 1);
    } else if (result->status == VMAFX_E_INVALID) {
        atomic_fetch_add(&calls->destroyed, 1);
    } else {
        atomic_fetch_add(&calls->other, 1);
    }
}

/* One round: a window whose score is imported (a wake the completion thread
 * may not have looked at yet) and one that can never complete, then the
 * destroy at once, with frames still on the workers. */
static char *destroy_round(DestroyCalls *calls)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.n_threads = 2;
    VmafxContext *context = NULL;
    mu_assert("context", vmafx_context_create(&config, &context, NULL) == VMAFX_OK &&
                             vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK);
    VmafxWindow *windows[2] = {NULL, NULL};
    for (unsigned k = 0; k < 2u; k++) {
        VmafxWindowRequest r =
            vw_request(vw_feature("pending"), (uint64_t)k * 100u, (uint64_t)k * 100u, VW_ALL_POOLS);
        r.on_complete = destroy_callback;
        r.user = calls;
        mu_assert("window", vmafx_window_submit(context, &r, &windows[k], NULL) == VMAFX_OK);
    }
    mu_assert("frames in flight", stall_submit(context, 0) && stall_submit(context, 1));
    mu_assert("score", vmafx_context_import_score(context, "pending", 0, 1.0, NULL) == VMAFX_OK);
    mu_assert("destroy right away", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    vmafx_window_release(windows[0]);
    vmafx_window_release(windows[1]);
    return NULL;
}

/* Destroy while a wake is pending and frames are on the workers: no hang;
 * the window whose score was in before the destroy completes with it, the
 * other as destroyed; every callback runs once, before the destroy returns
 * (TSan watches the threads). */
static char *test_destroy_with_a_pending_wake(void)
{
    static DestroyCalls calls;
    for (unsigned round = 0; round < N_DESTROY_ROUNDS; round++) {
        mu_assert_msg(destroy_round(&calls));
    }
    mu_assert("every final window completed with its scores",
              atomic_load(&calls.final_ok) == N_DESTROY_ROUNDS);
    mu_assert("every open window completed as destroyed",
              atomic_load(&calls.destroyed) == N_DESTROY_ROUNDS && atomic_load(&calls.other) == 0);
    return NULL;
}

/* ---- The engine shared with the completion thread ----------------------------------- */

enum { N_SHARED = 24 };

/* Score every frame per frame on the feeding thread while the completion
 * thread predicts the same frames for the windows. */
static char *score_frames_meanwhile(VmafxContext *context, const VmafxModel *model,
                                    VmafxScore *scores)
{
    for (unsigned i = 0; i < N_SHARED; i++) {
        scores[i] = (VmafxScore)VMAFX_SCORE_INIT;
        mu_assert("per-frame score beside the completion thread",
                  vmafx_score_frame(context, model, i, &scores[i], NULL) == VMAFX_OK);
    }
    return NULL;
}

/* The completion thread predicts model scores (a write into the engine)
 * while the feeding thread scores the same frames: the context's engine lock
 * keeps them apart, so every call succeeds, each frame is predicted once and
 * both read the same value (TSan watches the engine). */
/* A context without worker threads using MODEL; NULL on failure. */
static VmafxContext *model_context(VmafxModel **model)
{
    VmafxContext *context = NULL;
    *model = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK) {
        return NULL;
    }
    if (vmafx_model_load(NULL, MODEL, model, NULL) != VMAFX_OK ||
        vmafx_context_use_model(context, *model, NULL) != VMAFX_OK) {
        vmafx_model_unref(*model);
        *model = NULL;
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

/* A window [i, i] of the model for every frame. */
static bool open_shared_windows(VmafxContext *context, const VmafxModel *model,
                                VmafxWindow **windows)
{
    bool opened = true;
    for (unsigned i = 0; i < N_SHARED; i++) {
        windows[i] = vw_submit(context, vw_model(model), i, i);
        opened = opened && windows[i] != NULL;
    }
    return opened;
}

/* Every window holds the per-frame score its frame was given. */
static char *check_shared(VmafxWindow *const *windows, const VmafxScore *scores)
{
    for (unsigned i = 0; i < N_SHARED; i++) {
        VmafxWindowResult r;
        mu_assert("window complete", vw_complete(windows[i], &r) && r.status == VMAFX_OK);
        mu_assert("one prediction per frame",
                  vt_same_bits(r.value[VMAFX_POOL_MEAN], scores[i].value));
    }
    return NULL;
}

static char *test_engine_shared_with_the_completion_thread(void)
{
    static VmafxWindow *windows[N_SHARED];
    static VmafxScore scores[N_SHARED];
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, 176, 144);
    VmafxModel *model = NULL;
    VmafxContext *context = model_context(&model);
    mu_assert("context", context != NULL);
    mu_assert("windows", open_shared_windows(context, model, windows));
    mu_assert("frames", vw_submit_frames(context, &desc, 0, N_SHARED - 1));
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK); /* motion2: final now */
    mu_assert_msg(score_frames_meanwhile(context, model, scores));
    mu_message_t msg = check_shared(windows, scores);
    for (unsigned i = 0; i < N_SHARED; i++) {
        vmafx_window_release(windows[i]);
    }
    mu_assert_msg(msg);
    vmafx_model_unref(model);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Long and unlimited waits -------------------------------------------------------- */

typedef struct LateScore {
    VmafxContext *context;
    uint64_t index;
    int slept;
    VmafxStatus imported;
} LateScore;

/* Import the score of `index` 20 ms from now, while the test waits. */
static void *import_late(void *arg)
{
    LateScore *const late = arg;
    const struct timespec t = {.tv_sec = 0, .tv_nsec = 20000000};
    late->slept = nanosleep(&t, NULL);
    late->imported = vmafx_context_import_score(late->context, "late", late->index, 1.0, NULL);
    return NULL;
}

/* Wait with `timeout` for a window whose score arrives 20 ms later: the wait
 * returns when it completes, well before any timeout. */
static char *wait_with(VmafxContext *context, uint64_t index, uint64_t timeout)
{
    VmafxWindow *window = vw_submit(context, vw_feature("late"), index, index);
    mu_assert("window", window != NULL);
    LateScore late = {context, index, -1, VMAFX_E_INVALID};
    pthread_t thread;
    mu_assert("importer", pthread_create(&thread, NULL, import_late, &late) == 0);
    VmafxWindowResult r = VMAFX_WINDOW_RESULT_INIT;
    const uint64_t began = now_ns();
    const VmafxStatus status = vmafx_window_wait(window, timeout, &r, NULL);
    const uint64_t waited = now_ns() - began;
    mu_assert("joined", pthread_join(thread, NULL) == 0);
    vmafx_window_release(window);
    mu_assert("the late score was imported", late.slept == 0 && late.imported == VMAFX_OK);
    mu_assert("the wait returns with the window, not at once and not at a timeout",
              status == VMAFX_OK && r.status == VMAFX_OK && waited >= 10000000u &&
                  waited < VW_WAIT_NS);
    return NULL;
}

/* Timeouts from "without a limit" (UINT64_MAX) down to 600 s, around the
 * fence's 2^62 ns cut. icx 2026.0 at -O3 ran no round of the fence's wait
 * loop for UINT64_MAX and UINT64_MAX - 1 (docs/state.md
 * T-VMAFX-WAIT-FOREVER-ICX-ZERO-ROUNDS-2026-10-06). */
static const uint64_t long_timeouts[] = {
    UINT64_MAX,        UINT64_MAX - 1u,          UINT64_C(1) << 63,      (UINT64_C(1) << 62) + 1u,
    UINT64_C(1) << 62, (UINT64_C(1) << 62) - 1u, UINT64_C(600000000000),
};

#define N_LONG_TIMEOUTS (sizeof(long_timeouts) / sizeof(long_timeouts[0]))

/* Each long timeout waits for a window whose score arrives later: none may
 * return at once. The import happens on another thread, so the feeding
 * thread's wait is the test. */
static char *test_wait_forever_and_long_timeouts(void)
{
    VmafxContext *context = NULL;
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    for (unsigned i = 0; i < N_LONG_TIMEOUTS; i++) {
        mu_assert_msg(wait_with(context, i, long_timeouts[i]));
    }
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

typedef struct LateSignal {
    VmafxFence fence;
    int slept;
    VmafxStatus signalled;
} LateSignal;

/* Signal the fence 20 ms from now, while the test waits. */
static void *signal_late(void *arg)
{
    LateSignal *const late = arg;
    const struct timespec t = {.tv_sec = 0, .tv_nsec = 20000000};
    late->slept = nanosleep(&t, NULL);
    late->signalled = vmafx_fence_signal(&late->fence, NULL);
    return NULL;
}

/* vmafx_fence_wait() waits through the same loop as vmafx_window_wait(). */
static char *fence_wait_with(uint64_t timeout)
{
    LateSignal late = {VMAFX_FENCE_INIT, -1, VMAFX_E_INVALID};
    mu_assert("fence", vmafx_fence_create(NULL, VMAFX_FENCE_HOST, &late.fence, NULL) == VMAFX_OK);
    pthread_t thread;
    mu_assert("signaller", pthread_create(&thread, NULL, signal_late, &late) == 0);
    const uint64_t began = now_ns();
    const VmafxStatus status = vmafx_fence_wait(&late.fence, timeout, NULL);
    const uint64_t waited = now_ns() - began;
    mu_assert("joined", pthread_join(thread, NULL) == 0);
    mu_assert("destroyed", vmafx_fence_destroy(&late.fence, NULL) == VMAFX_OK);
    mu_assert("the fence was signalled", late.slept == 0 && late.signalled == VMAFX_OK);
    mu_assert("the wait returns with the signal, not at once and not at a timeout",
              status == VMAFX_OK && waited >= 10000000u && waited < VW_WAIT_NS);
    return NULL;
}

static char *test_fence_wait_forever_and_long_timeouts(void)
{
    for (unsigned i = 0; i < N_LONG_TIMEOUTS; i++) {
        mu_assert_msg(fence_wait_with(long_timeouts[i]));
    }
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_live_without_workers),
        MU_TEST(test_live_with_workers),
        MU_TEST(test_unpaced_producer_is_held_back),
        MU_TEST(test_release_races_delivery),
        MU_TEST(test_release_of_a_queued_window),
        MU_TEST(test_window_completes_while_the_feeder_stalls),
        MU_TEST(test_destroy_with_a_pending_wake),
        MU_TEST(test_engine_shared_with_the_completion_thread),
        MU_TEST(test_wait_forever_and_long_timeouts),
        MU_TEST(test_fence_wait_forever_and_long_timeouts),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
