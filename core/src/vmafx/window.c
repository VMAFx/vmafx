/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Window scores of the VMAFx API (RC4 WP4, ADR-2074; design section 2.8).
 *
 * A window asks for a target (a model, a model set or a feature) pooled with
 * a set of methods over a range of frames, and completes once every frame of
 * the range is final. Each context has one completion thread, started by its
 * first window: the calls that change the context's scores (vmafx_submit(),
 * vmafx_flush(), vmafx_context_import_score(), vmafx_window_submit()) and the
 * engine's worker jobs (the frame listener, vmafx_windows_frame_final())
 * raise a generation counter and signal it, and it looks at the open windows
 * once per change. It waits on a condition variable, never polls, so a
 * window completes when its last frame is final whether or not the feeding
 * thread calls again.
 *
 * The completion thread calls the engine (probes and pooling) through
 * vmafx_engine_enter(), which takes the context's engine lock (this set's
 * `engine_lock`); every API call that enters the engine takes it too, so the
 * engine sees one caller at a time as before. A score is written once and
 * then final (ADR-0154), so each window keeps a cursor: the frames before it
 * are known final, a pass looks only at frames it has not looked at, and it
 * never waits for work in flight (no fence).
 *
 * The values come from vmafx_pool_engine() (score.c), the pooling of the
 * synchronous calls, with the same arguments: equal bit for bit (HISS-19).
 *
 * Completion is published through a host fence (fence.c): the result is
 * written before the fence is signalled and never after, so
 * vmafx_window_poll() and vmafx_window_wait() read it on any thread without a
 * lock. Callbacks run on the completion thread, in completion order.
 *
 * Locking: VmafxWindowSet.lock guards the lists, the flags of the windows and
 * of the thread, the generation and the stream's state. Order: the engine
 * lock before the set lock, never the other way; nothing is freed while the
 * set lock is held (a window's last reference may drop the set's last one).
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "error_internal.h"
#include "internal.h"
#include "log.h"
#include "ref.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* MSVC recognises C11's _Thread_local in C but does not implement it; its
 * own storage class is the same thread-local storage. */
#if defined(_MSC_VER) && !defined(__clang__)
#define VMAFX_THREAD_LOCAL __declspec(thread)
#else
#define VMAFX_THREAD_LOCAL _Thread_local
#endif

/* Open windows per context: one more is VMAFX_E_BUSY (documented). */
#define VMAFX_WINDOW_MAX_OPEN 1024u
/* Marks a live window; a stale or foreign handle trips the assertions. */
#define VMAFX_WINDOW_MAGIC 0x76787764u /* "vxwd" */
/* Slots of VmafxWindowResult.value: VmafxPool 0 (NONE) to 8 (PERC20). */
#define VMAFX_WINDOW_POOL_SLOTS 9u
/* Every method bit of VmafxPoolMask (bits 1 to 8). */
#define VMAFX_WINDOW_POOL_BITS 0x1feu
/* Bound of the completion thread's loop and of a queue walk (HISS-02): one
 * round per change, pass or callback of the context's life. */
#define VMAFX_WINDOW_MAX_DELIVERIES UINT64_MAX

struct VmafxWindow {
    uint32_t magic;
    VmafRef *refs;          /* the caller's; the open list's; the callback queue's */
    VmafxWindowSet *set;    /* the window holds one reference of it */
    VmafxPoolTarget target; /* the model or set referenced, the feature copied */
    uint32_t pool_mask;
    uint64_t first;
    uint64_t last;
    VmafxWindowCallback on_complete;
    void *user;
    uint64_t cursor;          /* completion thread: frames [first, cursor) are final */
    bool open;                /* lock: on the open list */
    bool queued;              /* lock: on the callback queue */
    bool released;            /* lock: the caller released it */
    VmafxWindow *next;        /* lock: next on the open list or the queue */
    VmafxWindowResult result; /* written once, before `done` is signalled */
    VmafxHostFence *done;
};

struct VmafxWindowSet {
    VmafRef *refs; /* the context's, and one per window */
    /* The context's engine lock (vmafx_engine_enter()); taken before `lock`. */
    pthread_mutex_t engine_lock;
    pthread_mutex_t lock;
    pthread_cond_t wake;     /* work for the completion thread, or stop */
    pthread_cond_t callback; /* the callback queue gained a window, or stop */
    pthread_cond_t idle;     /* `delivering` or `paused` changed */
    VmafxWindow *open_head;
    VmafxWindow *open_tail;
    uint32_t n_open;
    VmafxWindow *queue_head;
    VmafxWindow *queue_tail;
    VmafxWindow *delivering; /* the window whose callback runs, or NULL */
    /* A change the completion thread has not looked at yet: `generation`
     * moves on every frame, import, flush and window submit; `seen` is the
     * generation of the thread's last pass. */
    uint64_t generation;
    uint64_t seen;
    /* The stream: the highest frame index submitted or imported (valid once
     * `have_scored`), and whether the context was flushed. */
    bool have_scored;
    uint64_t scored_last;
    bool flushed;
    bool pausing; /* vmafx_windows_pause(): no engine work until resumed */
    bool paused;  /* the thread acknowledged `pausing` */
    bool stop;
    bool thread_started;   /* the completion thread */
    bool callback_started; /* the callback thread */
    pthread_t thread;
    pthread_t callback_thread;
    VmafxContext *context; /* the completion thread's; valid until it is joined */
    /* The context's log callback, kept for the completion thread: a message
     * raised in a callback reaches the context's callback too. */
    VmafxLogCallback log_callback;
    void *log_user;
    VmafLogSink sink;
};

/* The set whose callbacks the calling thread is delivering, or NULL. */
static VMAFX_THREAD_LOCAL const VmafxWindowSet *vmafx_delivering_set;

/* ---- Sets and windows ---------------------------------------------------------- */

static void set_deliver(enum VmafLogLevel level, const char *message, void *user)
{
    const VmafxWindowSet *const set = user;
    set->log_callback((uint32_t)level, message, set->log_user);
}

/* Undo the first `stage` steps of set_new(). */
static void set_teardown(VmafxWindowSet *set, unsigned stage)
{
    if (stage > 4u) {
        (void)pthread_cond_destroy(&set->callback);
    }
    if (stage > 3u) {
        (void)pthread_cond_destroy(&set->idle);
    }
    if (stage > 2u) {
        (void)pthread_cond_destroy(&set->wake);
    }
    if (stage > 1u) {
        (void)pthread_mutex_destroy(&set->lock);
    }
    if (stage > 0u) {
        (void)pthread_mutex_destroy(&set->engine_lock);
    }
    if (set->refs) {
        (void)vmaf_ref_close(set->refs);
    }
    free(set);
}

static VmafxWindowSet *set_new(VmafxContext *context)
{
    VmafxWindowSet *const set = calloc(1, sizeof(*set));
    if (!set) {
        return NULL;
    }
    unsigned stage = 0;
    if (vmaf_ref_init(&set->refs) == 0 && pthread_mutex_init(&set->engine_lock, NULL) == 0) {
        stage = 1u;
    }
    if (stage == 1u && pthread_mutex_init(&set->lock, NULL) == 0) {
        stage = 2u;
    }
    if (stage == 2u && pthread_cond_init(&set->wake, NULL) == 0) {
        stage = 3u;
    }
    if (stage == 3u && pthread_cond_init(&set->idle, NULL) == 0) {
        stage = 4u;
    }
    if (stage == 4u && pthread_cond_init(&set->callback, NULL) == 0) {
        stage = 5u;
    }
    if (stage < 5u) {
        set_teardown(set, stage);
        return NULL;
    }
    set->context = context;
    set->log_callback = context->log_callback;
    set->log_user = context->log_user;
    set->sink = (VmafLogSink){.deliver = set_deliver, .user = set, .level = context->sink.level};
    return set;
}

static void set_unref(VmafxWindowSet *set)
{
    if (vmaf_ref_fetch_decrement(set->refs) != 1) {
        return;
    }
    assert(!set->open_head && !set->queue_head && !set->delivering);
    set_teardown(set, 5u);
}

static void window_unref(VmafxWindow *window)
{
    assert(window && window->magic == VMAFX_WINDOW_MAGIC);
    if (vmaf_ref_fetch_decrement(window->refs) != 1) {
        return;
    }
    VmafxWindowSet *const set = window->set;
    window->magic = 0u;
    if (window->target.kind == VMAFX_WINDOW_TARGET_MODEL) {
        vmafx_model_unref((VmafxModel *)window->target.model);
    } else if (window->target.kind == VMAFX_WINDOW_TARGET_MODEL_SET) {
        vmafx_model_set_unref((VmafxModelSet *)window->target.set);
    } else {
        free((char *)window->target.feature);
    }
    vmafx_host_fence_unref(window->done);
    (void)vmaf_ref_close(window->refs);
    free(window);
    set_unref(set);
}

/* Drop a reference of the window the caller knows is not its last: the
 * caller holds another one. */
static void drop_held_ref(VmafxWindow *window)
{
    const long before = vmaf_ref_fetch_decrement(window->refs);
    assert(before > 1);
    (void)before;
}

/* ---- Requests ------------------------------------------------------------------ */

static VmafxStatus check_target(const VmafxReport *report, const VmafxWindowRequest *r)
{
    const bool known = r->target == VMAFX_WINDOW_TARGET_MODEL ||
                       r->target == VMAFX_WINDOW_TARGET_MODEL_SET ||
                       r->target == VMAFX_WINDOW_TARGET_FEATURE;
    if (!known) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "request.target",
                          "target %u is not a VmafxWindowTarget", (unsigned)r->target);
    }
    const bool set = r->target == VMAFX_WINDOW_TARGET_MODEL     ? r->model != NULL :
                     r->target == VMAFX_WINDOW_TARGET_MODEL_SET ? r->model_set != NULL :
                                                                  r->feature != NULL;
    if (!set) {
        const char *const field = r->target == VMAFX_WINDOW_TARGET_MODEL     ? "request.model" :
                                  r->target == VMAFX_WINDOW_TARGET_MODEL_SET ? "request.model_set" :
                                                                               "request.feature";
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, field,
                          "NULL for a window of target %u", (unsigned)r->target);
    }
    return VMAFX_OK;
}

static VmafxStatus check_range(const VmafxReport *report, const VmafxWindowRequest *r)
{
    if (!r->pool_mask || (r->pool_mask & ~(uint32_t)VMAFX_WINDOW_POOL_BITS)) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "request.pool_mask",
                          "pool mask 0x%x is not a non-empty set of VmafxPoolMask methods",
                          (unsigned)r->pool_mask);
    }
    if (r->first > r->last) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, "request.first",
                          "first frame %llu is after last frame %llu", (unsigned long long)r->first,
                          (unsigned long long)r->last);
    }
    if (r->last > UINT_MAX) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_FRAME, "request.last",
                          "frame index %llu exceeds %u", (unsigned long long)r->last, UINT_MAX);
    }
    return VMAFX_OK;
}

/* Read and check the caller's request into `r`. */
static VmafxStatus read_request(const VmafxReport *report, const VmafxWindowRequest *request,
                                VmafxWindowRequest *r)
{
    VmafxStatus status = vmafx_read_sized(report, r, (uint32_t)sizeof(*r), request,
                                          VMAFX_MIN_WINDOW_REQUEST, "request");
    if (status == VMAFX_OK) {
        status = check_target(report, r);
    }
    return status == VMAFX_OK ? check_range(report, r) : status;
}

/* The window's own copy of the target: a reference of the model or set, a
 * copy of the feature name. False when the copy cannot be allocated. */
static bool hold_target(VmafxWindow *window, const VmafxWindowRequest *r)
{
    window->target.kind = r->target;
    if (r->target == VMAFX_WINDOW_TARGET_MODEL) {
        window->target.model = vmafx_model_ref((VmafxModel *)r->model);
    } else if (r->target == VMAFX_WINDOW_TARGET_MODEL_SET) {
        window->target.set = vmafx_model_set_ref((VmafxModelSet *)r->model_set);
    } else {
        const size_t n = strlen(r->feature) + 1u;
        char *const copy = malloc(n);
        if (!copy) {
            return false;
        }
        memcpy(copy, r->feature, n);
        window->target.feature = copy;
    }
    return true;
}

/* A new window of `set` with two references (the caller's and the open
 * list's), or NULL (no memory). */
static VmafxWindow *window_new(VmafxWindowSet *set, const VmafxWindowRequest *r)
{
    VmafxWindow *const window = calloc(1, sizeof(*window));
    if (!window) {
        return NULL;
    }
    window->done = vmafx_host_fence_new();
    if (!window->done || vmaf_ref_init(&window->refs) != 0 || !hold_target(window, r)) {
        vmafx_host_fence_unref(window->done);
        if (window->refs) {
            (void)vmaf_ref_close(window->refs);
        }
        free(window);
        return NULL;
    }
    vmaf_ref_fetch_increment(window->refs);
    vmaf_ref_fetch_increment(set->refs);
    window->magic = VMAFX_WINDOW_MAGIC;
    window->set = set;
    window->pool_mask = r->pool_mask;
    window->first = r->first;
    window->last = r->last;
    window->cursor = r->first;
    window->on_complete = r->on_complete;
    window->user = r->user;
    window->result = (VmafxWindowResult)VMAFX_WINDOW_RESULT_INIT;
    window->result.target = r->target;
    window->result.pool_mask = r->pool_mask;
    window->result.first = r->first;
    window->result.last = r->last;
    window->result.name = vmafx_pool_target_name(&window->target);
    return window;
}

/* ---- Lists (lock held) ----------------------------------------------------------- */

static void open_push(VmafxWindowSet *set, VmafxWindow *window)
{
    window->next = NULL;
    if (set->open_tail) {
        set->open_tail->next = window;
    } else {
        set->open_head = window;
    }
    set->open_tail = window;
    set->n_open++;
    window->open = true;
}

/* Take `window` off the open list; true when it was on it (the list's
 * reference is the caller's to drop then). */
static bool open_unlink(VmafxWindowSet *set, VmafxWindow *window)
{
    if (!window->open) {
        return false;
    }
    VmafxWindow *previous = NULL;
    VmafxWindow *item = set->open_head;
    for (uint32_t i = 0; item && item != window && i < set->n_open; i++) {
        previous = item;
        item = item->next;
    }
    assert(item == window);
    if (previous) {
        previous->next = window->next;
    } else {
        set->open_head = window->next;
    }
    if (set->open_tail == window) {
        set->open_tail = previous;
    }
    set->n_open--;
    window->open = false;
    window->next = NULL;
    return true;
}

static void queue_push(VmafxWindowSet *set, VmafxWindow *window)
{
    window->next = NULL;
    if (set->queue_tail) {
        set->queue_tail->next = window;
    } else {
        set->queue_head = window;
    }
    set->queue_tail = window;
    window->queued = true;
    (void)pthread_cond_signal(&set->callback);
}

/* Take `window` off the callback queue; true when it was on it. The queue is
 * never longer than the windows of the set, which hold a reference each. */
static bool queue_unlink(VmafxWindowSet *set, VmafxWindow *window)
{
    if (!window->queued) {
        return false;
    }
    VmafxWindow *previous = NULL;
    VmafxWindow *item = set->queue_head;
    for (uint64_t i = 0; item && item != window && i < VMAFX_WINDOW_MAX_DELIVERIES; i++) {
        previous = item;
        item = item->next;
    }
    assert(item == window);
    if (previous) {
        previous->next = window->next;
    } else {
        set->queue_head = window->next;
    }
    if (set->queue_tail == window) {
        set->queue_tail = previous;
    }
    window->queued = false;
    window->next = NULL;
    return true;
}

/* ---- Completion (completion thread) ----------------------------------------------- */

/* The stream as a pass sees it, read under the set lock. */
typedef struct WindowStream {
    bool have_scored;
    uint64_t scored_last;
    bool flushed;
} WindowStream;

/* Number of indices in [first, last] the context scores. */
static uint64_t scored_in(uint64_t first, uint64_t last, unsigned subsample)
{
    assert(first <= last && subsample >= 1u);
    const uint64_t below_first = first ? (first - 1u) / subsample + 1u : 0u;
    return last / subsample + 1u - below_first;
}

/* Whether frame `index` of the window is final: 0, or the engine's errno.
 * Takes the engine lock (vmafx_engine_enter()). */
static int probe(VmafxContext *context, const VmafxWindow *window, unsigned index)
{
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    int err = -EINVAL;
    if (window->target.kind == VMAFX_WINDOW_TARGET_MODEL) {
        err = vmaf_engine_try_score_at_index(context->engine, window->target.model->engine, index);
    } else if (window->target.kind == VMAFX_WINDOW_TARGET_MODEL_SET) {
        err = vmaf_engine_try_score_at_index_model_collection(
            context->engine, vmafx_model_set_engine(window->target.set), index);
    } else {
        err = vmaf_engine_feature_written(context->engine, window->target.feature, index);
    }
    vmafx_engine_leave(context, previous);
    return err;
}

/* Move the cursor over the final frames up to `end`: 0 when every scored
 * frame of [cursor, end] is final, else the errno of the first that is not. */
static int advance(VmafxContext *context, VmafxWindow *window, uint64_t end)
{
    const unsigned subsample = vmaf_engine_subsample(context->engine);
    for (uint64_t i = window->cursor; i <= end && i <= UINT_MAX; i++) {
        if (subsample > 1u && i % subsample) {
            window->cursor = i + 1u;
            continue;
        }
        const int err = probe(context, window, (unsigned)i);
        if (err) {
            return err;
        }
        window->cursor = i + 1u;
    }
    return 0;
}

/* A completed window without values: `status`, logged to the context. */
static void fail(VmafxContext *context, VmafxWindow *window, VmafxStatus status, int err,
                 const char *why)
{
    const VmafxReport report = {
        .error = NULL, .sink = vmafx_context_log_sink(context), .function = "vmafx_window"};
    const uint32_t kind = window->target.kind == VMAFX_WINDOW_TARGET_FEATURE ?
                              VMAFX_SUBJECT_FEATURE :
                              VMAFX_SUBJECT_MODEL;
    VmafxWindowResult *const r = &window->result;
    memset(r->value, 0, sizeof(r->value));
    memset(r->stddev, 0, sizeof(r->stddev));
    memset(r->ci95_lo, 0, sizeof(r->ci95_lo));
    memset(r->ci95_hi, 0, sizeof(r->ci95_hi));
    r->status =
        VMAFX_FAIL(&report, status, err, kind, r->name, "window of frames %llu to %llu: %s",
                   (unsigned long long)window->first, (unsigned long long)window->last, why);
}

/* Pool every requested method over [first, end]: VMAFX_PENDING when a frame
 * turned out not final (only before the flush), else complete. */
static VmafxStatus compute(VmafxContext *context, VmafxWindow *window, uint64_t end, bool flushed)
{
    VmafxWindowResult *const r = &window->result;
    for (uint32_t pool = 1; pool < VMAFX_WINDOW_POOL_SLOTS; pool++) {
        if (!(window->pool_mask & (1u << pool))) {
            continue;
        }
        VmafxPoolValue v = {0};
        const int err = vmafx_pool_engine(context, &window->target, pool, window->first, end, &v);
        if (err == -EAGAIN && !flushed) {
            return VMAFX_PENDING;
        }
        if (err) {
            fail(context, window, VMAFX_E_INTERNAL, err, "the engine could not pool it");
            return VMAFX_OK;
        }
        r->value[pool] = v.value;
        r->stddev[pool] = v.stddev;
        r->ci95_lo[pool] = v.ci95_lo;
        r->ci95_hi[pool] = v.ci95_hi;
    }
    r->status = VMAFX_OK;
    return VMAFX_OK;
}

/* The frames of a window the stream reaches are final: pool them, unless
 * frames past the stream's end may still come. */
static VmafxStatus complete_range(VmafxContext *context, VmafxWindow *window, uint64_t end,
                                  bool flushed)
{
    if (end < window->last && !flushed) {
        return VMAFX_PENDING;
    }
    VmafxWindowResult *const r = &window->result;
    r->n_frames = end - window->first + 1u;
    r->n_scored = scored_in(window->first, end, vmaf_engine_subsample(context->engine));
    r->flags = end < window->last ? VMAFX_WINDOW_PARTIAL : 0u;
    if (r->n_scored == 0u) {
        fail(context, window, VMAFX_E_RANGE, 0, "subsampling scores none of its frames");
        return VMAFX_OK;
    }
    return compute(context, window, end, flushed);
}

/* Complete the window when it can be: VMAFX_OK when its result is written
 * (values or a failure), VMAFX_PENDING when it stays open. */
static VmafxStatus try_complete(VmafxContext *context, VmafxWindow *window,
                                const WindowStream *stream)
{
    assert(window->magic == VMAFX_WINDOW_MAGIC);
    if (!stream->have_scored || stream->scored_last < window->first) {
        if (!stream->flushed) {
            return VMAFX_PENDING;
        }
        window->result.flags = VMAFX_WINDOW_PARTIAL;
        fail(context, window, VMAFX_E_RANGE, 0, "the stream ended before its first frame");
        return VMAFX_OK;
    }
    const uint64_t end = stream->scored_last < window->last ? stream->scored_last : window->last;
    const int err = advance(context, window, end);
    if (err == -EAGAIN || err == -EINVAL) {
        if (!stream->flushed) {
            return VMAFX_PENDING;
        }
        fail(context, window, VMAFX_E_NOTFOUND, err, "a frame has no score after the flush");
        return VMAFX_OK;
    }
    if (err) {
        fail(context, window, VMAFX_E_INTERNAL, err, "the engine could not score a frame");
        return VMAFX_OK;
    }
    return complete_range(context, window, end, stream->flushed);
}

/* Publish a completed window (lock held): off the open list, the result
 * visible, the callback queued unless it was released. True when it was on
 * the open list: that list's reference is the caller's to drop, after
 * unlocking. */
static bool publish(VmafxWindowSet *set, VmafxWindow *window)
{
    const bool was_open = open_unlink(set, window);
    vmafx_host_fence_signal(window->done);
    if (window->on_complete && !window->released) {
        vmaf_ref_fetch_increment(window->refs);
        queue_push(set, window);
    }
    return was_open;
}

/* Take a reference of every open window (lock held by the caller). */
static uint32_t snapshot_open(VmafxWindowSet *set, VmafxWindow **out)
{
    uint32_t n = 0;
    for (VmafxWindow *w = set->open_head; w && n < VMAFX_WINDOW_MAX_OPEN; w = w->next) {
        vmaf_ref_fetch_increment(w->refs);
        out[n++] = w;
    }
    return n;
}

/* ADR-2090: let the engine append what the scores already in make final
 * (motion2 / motion3 of the frames whose window a worker's SAD completed)
 * before the windows are probed, so a window over them completes while the
 * feeder stalls. Engine lock only; a failure leaves the frames to the next
 * pass or the flush, and is logged to the context. */
static void advance_engine(VmafxContext *context)
{
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_advance(context->engine);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmafx_window: the engine could not derive scores (%d)\n",
                 err);
    }
    vmafx_engine_leave(context, previous);
}

/* One pass: look at every open window once and complete those whose frames
 * are final. Called and returns with the set lock held; runs the engine
 * work without it. */
static void evaluate(VmafxWindowSet *set)
{
    VmafxWindow *snapshot[VMAFX_WINDOW_MAX_OPEN];
    const uint32_t n = snapshot_open(set, snapshot);
    const WindowStream stream = {set->have_scored, set->scored_last, set->flushed};
    (void)pthread_mutex_unlock(&set->lock);
    if (n > 0u) {
        advance_engine(set->context);
    }
    for (uint32_t i = 0; i < n; i++) {
        VmafxWindow *const window = snapshot[i];
        if (try_complete(set->context, window, &stream) == VMAFX_OK) {
            (void)pthread_mutex_lock(&set->lock);
            const bool was_open = publish(set, window);
            (void)pthread_mutex_unlock(&set->lock);
            if (was_open) {
                drop_held_ref(window); /* the open list's; the snapshot holds one */
            }
        }
        window_unref(window);
    }
    (void)pthread_mutex_lock(&set->lock);
}

/* ---- The completion thread and the callback thread ---------------------------------- */

enum { WORK_NONE, WORK_PAUSE, WORK_EVALUATE, WORK_STOP };

/* What the completion thread does next (set lock held). A pause waits for
 * the changes already signalled: a window whose frames were final before
 * vmafx_context_destroy() completes with its values, not as destroyed. */
static int next_work(const VmafxWindowSet *set)
{
    if (!set->paused && set->seen != set->generation) {
        return WORK_EVALUATE;
    }
    if (set->pausing && !set->paused) {
        return WORK_PAUSE;
    }
    return set->stop ? WORK_STOP : WORK_NONE;
}

/* The completion thread: one pass per change of the context's scores, woken
 * through `wake`; it never polls. */
static void *completion_thread(void *arg)
{
    VmafxWindowSet *const set = arg;
    (void)pthread_mutex_lock(&set->lock);
    for (uint64_t round = 0; round < VMAFX_WINDOW_MAX_DELIVERIES; round++) {
        int work = next_work(set);
        while (work == WORK_NONE) {
            (void)pthread_cond_wait(&set->wake, &set->lock);
            work = next_work(set);
        }
        if (work == WORK_STOP) {
            break;
        }
        if (work == WORK_PAUSE) {
            set->paused = true;
            (void)pthread_cond_broadcast(&set->idle);
        } else {
            set->seen = set->generation;
            evaluate(set);
        }
    }
    (void)pthread_mutex_unlock(&set->lock);
    return NULL;
}

/* Run the callback of the first queued window. Called and returns with the
 * set lock held. */
static void deliver_one(VmafxWindowSet *set)
{
    VmafxWindow *const window = set->queue_head;
    assert(window && window->queued);
    (void)queue_unlink(set, window);
    set->delivering = window;
    (void)pthread_mutex_unlock(&set->lock);
    window->on_complete(window, &window->result, window->user);
    (void)pthread_mutex_lock(&set->lock);
    set->delivering = NULL;
    (void)pthread_cond_broadcast(&set->idle);
    /* Drop the queue's reference unlocked: it may be the window's last, and
     * that drops a reference of the set (never its last: the context holds
     * one until this thread is joined). */
    (void)pthread_mutex_unlock(&set->lock);
    window_unref(window);
    (void)pthread_mutex_lock(&set->lock);
}

/* The callback thread: runs callbacks in completion order, apart from the
 * completion thread so that a slow callback never holds up the completion
 * of other windows; drains the queue before it stops. */
static void *callback_thread(void *arg)
{
    VmafxWindowSet *const set = arg;
    vmafx_delivering_set = set;
    const VmafLogSink *const previous =
        vmaf_log_swap_thread_sink(set->log_callback ? &set->sink : NULL);
    (void)pthread_mutex_lock(&set->lock);
    for (uint64_t round = 0; round < VMAFX_WINDOW_MAX_DELIVERIES; round++) {
        while (!set->queue_head && !set->stop) {
            (void)pthread_cond_wait(&set->callback, &set->lock);
        }
        if (!set->queue_head) {
            break;
        }
        deliver_one(set);
    }
    (void)pthread_mutex_unlock(&set->lock);
    (void)vmaf_log_swap_thread_sink(previous);
    vmafx_delivering_set = NULL;
    return NULL;
}

/* ---- Hooks ------------------------------------------------------------------------- */

/* A change for the completion thread to look at (set lock held). */
static void wake_locked(VmafxWindowSet *set)
{
    set->generation++;
    (void)pthread_cond_signal(&set->wake);
}

bool vmafx_windows_init(VmafxContext *context)
{
    assert(context && !context->windows);
    context->windows = set_new(context);
    return context->windows != NULL;
}

void vmafx_windows_frame_final(void *user)
{
    VmafxWindowSet *const set = user;
    (void)pthread_mutex_lock(&set->lock);
    wake_locked(set);
    (void)pthread_mutex_unlock(&set->lock);
}

void vmafx_context_lock(const VmafxContext *context)
{
    if (context && context->windows) {
        (void)pthread_mutex_lock(&context->windows->engine_lock);
    }
}

void vmafx_context_unlock(const VmafxContext *context)
{
    if (context && context->windows) {
        (void)pthread_mutex_unlock(&context->windows->engine_lock);
    }
}

void vmafx_windows_note_index(VmafxContext *context, uint64_t index)
{
    VmafxWindowSet *const set = context->windows;
    assert(set);
    (void)pthread_mutex_lock(&set->lock);
    if (!set->have_scored || index > set->scored_last) {
        set->scored_last = index;
    }
    set->have_scored = true;
    wake_locked(set);
    (void)pthread_mutex_unlock(&set->lock);
}

void vmafx_windows_note_flush(VmafxContext *context)
{
    VmafxWindowSet *const set = context->windows;
    assert(set && vmaf_engine_is_flushed(context->engine));
    (void)pthread_mutex_lock(&set->lock);
    set->flushed = true;
    wake_locked(set);
    (void)pthread_mutex_unlock(&set->lock);
}

/* Stop the completion thread's engine work (set lock held): it acknowledges
 * once it has looked at every change signalled so far. */
static void pause_locked(VmafxWindowSet *set)
{
    set->pausing = true;
    if (!set->thread_started) {
        set->paused = true;
        return;
    }
    (void)pthread_cond_signal(&set->wake);
    while (!set->paused) {
        (void)pthread_cond_wait(&set->idle, &set->lock);
    }
}

void vmafx_windows_pause(VmafxContext *context)
{
    VmafxWindowSet *const set = context->windows;
    assert(set);
    (void)pthread_mutex_lock(&set->lock);
    pause_locked(set);
    (void)pthread_mutex_unlock(&set->lock);
}

void vmafx_windows_resume(VmafxContext *context)
{
    VmafxWindowSet *const set = context->windows;
    assert(set);
    (void)pthread_mutex_lock(&set->lock);
    set->pausing = false;
    set->paused = false;
    wake_locked(set);
    (void)pthread_mutex_unlock(&set->lock);
}

void vmafx_windows_close(VmafxContext *context)
{
    VmafxWindowSet *const set = context->windows;
    if (!set) {
        return;
    }
    assert(set->context == context);
    VmafxWindow *open[VMAFX_WINDOW_MAX_OPEN];
    bool was_open[VMAFX_WINDOW_MAX_OPEN];
    (void)pthread_mutex_lock(&set->lock);
    pause_locked(set); /* the thread no longer touches a window or the engine */
    const uint32_t n = snapshot_open(set, open);
    (void)pthread_mutex_unlock(&set->lock);
    for (uint32_t i = 0; i < n; i++) {
        open[i]->result.flags = VMAFX_WINDOW_PARTIAL;
        fail(context, open[i], VMAFX_E_INVALID, 0, "the context was destroyed first");
    }
    (void)pthread_mutex_lock(&set->lock);
    for (uint32_t i = 0; i < n; i++) {
        was_open[i] = publish(set, open[i]);
    }
    set->stop = true;
    (void)pthread_cond_signal(&set->wake);
    (void)pthread_cond_signal(&set->callback);
    const bool started = set->thread_started;
    const bool callbacks = set->callback_started;
    (void)pthread_mutex_unlock(&set->lock);
    if (started) {
        (void)pthread_join(set->thread, NULL);
    }
    if (callbacks) {
        (void)pthread_join(set->callback_thread, NULL); /* runs every queued callback first */
    }
    for (uint32_t i = 0; i < n; i++) {
        if (was_open[i]) {
            drop_held_ref(open[i]); /* the open list's; the snapshot holds one */
        }
        window_unref(open[i]);
    }
    context->windows = NULL;
    set_unref(set);
}

/* ---- Public functions ------------------------------------------------------------ */

/* Start the context's completion thread with its first window, and its
 * callback thread with its first window that has a callback. */
static VmafxStatus start_threads(const VmafxReport *report, VmafxWindowSet *set, bool callback)
{
    bool ok = true;
    (void)pthread_mutex_lock(&set->lock);
    if (!set->thread_started) {
        ok = pthread_create(&set->thread, NULL, completion_thread, set) == 0;
        set->thread_started = ok;
    }
    if (ok && callback && !set->callback_started) {
        ok = pthread_create(&set->callback_thread, NULL, callback_thread, set) == 0;
        set->callback_started = ok;
    }
    (void)pthread_mutex_unlock(&set->lock);
    return ok ? VMAFX_OK :
                VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_CONTEXT, "context",
                           "cannot start the window threads");
}

/* Put a new window on the open list and wake the completion thread:
 * VMAFX_E_BUSY when the set is full. */
static VmafxStatus open_window(const VmafxReport *report, VmafxWindowSet *set,
                               const VmafxWindowRequest *r, VmafxWindow **out)
{
    VmafxWindow *const window = window_new(set, r);
    if (!window) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "cannot allocate a window");
    }
    (void)pthread_mutex_lock(&set->lock);
    const bool full = set->n_open >= VMAFX_WINDOW_MAX_OPEN;
    if (!full) {
        open_push(set, window);
        wake_locked(set);
    }
    (void)pthread_mutex_unlock(&set->lock);
    if (full) {
        drop_held_ref(window); /* the open list's, never taken */
        window_unref(window);
        return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "%u windows are open; complete or release one first",
                          (unsigned)VMAFX_WINDOW_MAX_OPEN);
    }
    *out = window;
    return VMAFX_OK;
}

VmafxStatus vmafx_window_submit(VmafxContext *context, const VmafxWindowRequest *request,
                                VmafxWindow **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (out) {
        *out = NULL;
    }
    if (!context || !request || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" :
                          !request ? "request" :
                                     "out",
                          "NULL argument");
    }
    assert(context->windows);
    VmafxWindowRequest r = VMAFX_WINDOW_REQUEST_INIT;
    VmafxStatus status = read_request(&report, request, &r);
    if (status == VMAFX_OK) {
        status = start_threads(&report, context->windows, r.on_complete != NULL);
    }
    return status == VMAFX_OK ? open_window(&report, context->windows, &r, out) : status;
}

VmafxStatus vmafx_window_poll(const VmafxWindow *window, VmafxWindowResult *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!window || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !window ? "window" : "out", "NULL argument");
    }
    assert(window->magic == VMAFX_WINDOW_MAGIC);
    if (!vmafx_host_fence_signalled(window->done)) {
        return VMAFX_PENDING;
    }
    return vmafx_write_sized(&report, out, &window->result, (uint32_t)sizeof(window->result),
                             "out");
}

VmafxStatus vmafx_window_wait(const VmafxWindow *window, uint64_t timeout_ns,
                              VmafxWindowResult *out, VmafxError **error)
{
    if (window && out) {
        assert(window->magic == VMAFX_WINDOW_MAGIC);
        (void)vmafx_host_fence_wait(window->done, timeout_ns);
    }
    return vmafx_window_poll(window, out, error);
}

void vmafx_window_release(VmafxWindow *window)
{
    if (!window) {
        return;
    }
    assert(window->magic == VMAFX_WINDOW_MAGIC);
    VmafxWindowSet *const set = window->set;
    (void)pthread_mutex_lock(&set->lock);
    window->released = true;
    const bool was_open = open_unlink(set, window);
    const bool was_queued = queue_unlink(set, window);
    /* A callback of this window running on the completion thread finishes
     * first, unless this is that callback. */
    while (set->delivering == window && vmafx_delivering_set != set) {
        (void)pthread_cond_wait(&set->idle, &set->lock);
    }
    (void)pthread_mutex_unlock(&set->lock);
    if (was_open) {
        drop_held_ref(window); /* the caller holds one more */
    }
    if (was_queued) {
        drop_held_ref(window);
    }
    window_unref(window); /* the caller's */
}

uint32_t vmafx_context_max_in_flight(const VmafxContext *context)
{
    if (!context) {
        return 0u;
    }
    return vmaf_engine_max_in_flight(context->engine);
}

/* NOLINTEND(modernize-use-nullptr) */
