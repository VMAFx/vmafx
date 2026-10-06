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
 * the range is final. Completion is found on the thread that feeds the
 * context, in the calls that change its scores (vmafx_submit(),
 * vmafx_flush(), vmafx_context_import_score(), vmafx_window_submit()). Those
 * calls are externally synchronised with every other engine call of the
 * context (design section 2.4), so finding and computing a window needs no
 * engine lock. A score is written once and then final (ADR-0154), so each
 * window keeps a cursor: the frames before it are known final, a call looks
 * only at frames it has not looked at, and it never waits for work in flight
 * (no fence): a frame still on a worker thread is looked at again by the next
 * call.
 *
 * The values come from vmafx_pool_engine() (score.c), the pooling of the
 * synchronous calls, with the same arguments: equal bit for bit (HISS-19).
 *
 * Completion is published through a host fence (fence.c): the result is
 * written before the fence is signalled and never after, so
 * vmafx_window_poll() and vmafx_window_wait() read it on any thread without a
 * lock. Callbacks run on one window thread per context, started by the first
 * window that has one, in completion order.
 *
 * Locking: VmafxWindowSet.lock guards the open list, the callback queue,
 * `released`, `queued` and `delivering`. Nothing is freed while it is held:
 * dropping a window's last reference may drop the set's last one.
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
/* Bound of the window thread's loop (HISS-02): one round per callback, and a
 * context completes at most one window per frame index and request. */
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
    uint64_t cursor;          /* feeding thread: frames [first, cursor) are final */
    bool open;                /* lock: on the open list */
    bool queued;              /* lock: on the callback queue */
    bool released;            /* lock: the caller released it */
    VmafxWindow *next;        /* lock: next on the open list or the queue */
    VmafxWindowResult result; /* written once, before `done` is signalled */
    VmafxHostFence *done;
};

struct VmafxWindowSet {
    VmafRef *refs; /* the context's, and one per window */
    pthread_mutex_t lock;
    pthread_cond_t wake; /* the queue gained a window, or stop */
    pthread_cond_t idle; /* `delivering` changed */
    VmafxWindow *open_head;
    VmafxWindow *open_tail;
    uint32_t n_open;
    VmafxWindow *queue_head;
    VmafxWindow *queue_tail;
    VmafxWindow *delivering; /* the window whose callback runs, or NULL */
    bool stop;
    bool thread_started; /* feeding thread only */
    pthread_t thread;
    /* The context's log callback, kept for the window thread: a message
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

static VmafxWindowSet *set_new(const VmafxContext *context)
{
    VmafxWindowSet *const set = calloc(1, sizeof(*set));
    if (!set) {
        return NULL;
    }
    bool ok = vmaf_ref_init(&set->refs) == 0;
    const bool locked = ok && pthread_mutex_init(&set->lock, NULL) == 0;
    const bool waked = locked && pthread_cond_init(&set->wake, NULL) == 0;
    ok = waked && pthread_cond_init(&set->idle, NULL) == 0;
    if (!ok) {
        if (waked) {
            (void)pthread_cond_destroy(&set->wake);
        }
        if (locked) {
            (void)pthread_mutex_destroy(&set->lock);
        }
        if (set->refs) {
            (void)vmaf_ref_close(set->refs);
        }
        free(set);
        return NULL;
    }
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
    (void)pthread_cond_destroy(&set->idle);
    (void)pthread_cond_destroy(&set->wake);
    (void)pthread_mutex_destroy(&set->lock);
    (void)vmaf_ref_close(set->refs);
    free(set);
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
    (void)pthread_cond_signal(&set->wake);
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

/* ---- The window thread --------------------------------------------------------- */

/* Run the callback of the next queued window; false when the thread stops.
 * Called with the lock held; returns with it held. */
static bool deliver_next(VmafxWindowSet *set)
{
    while (!set->queue_head && !set->stop) {
        (void)pthread_cond_wait(&set->wake, &set->lock);
    }
    VmafxWindow *const window = set->queue_head;
    if (!window) {
        return false;
    }
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
    return true;
}

static void *window_thread(void *arg)
{
    VmafxWindowSet *const set = arg;
    vmafx_delivering_set = set;
    const VmafLogSink *const previous =
        vmaf_log_swap_thread_sink(set->log_callback ? &set->sink : NULL);
    (void)pthread_mutex_lock(&set->lock);
    for (uint64_t n = 0; n < VMAFX_WINDOW_MAX_DELIVERIES; n++) {
        if (!deliver_next(set)) {
            break;
        }
    }
    (void)pthread_mutex_unlock(&set->lock);
    (void)vmaf_log_swap_thread_sink(previous);
    vmafx_delivering_set = NULL;
    return NULL;
}

/* ---- Completion (feeding thread) --------------------------------------------------- */

/* Number of indices in [first, last] the context scores. */
static uint64_t scored_in(uint64_t first, uint64_t last, unsigned subsample)
{
    assert(first <= last && subsample >= 1u);
    const uint64_t below_first = first ? (first - 1u) / subsample + 1u : 0u;
    return last / subsample + 1u - below_first;
}

/* Whether frame `index` of the window is final: 0, or the engine's errno. */
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
    vmafx_engine_leave(previous);
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

/* Complete the window when it can be: VMAFX_OK when its result is written
 * (values or a failure), VMAFX_PENDING when it stays open. */
static VmafxStatus try_complete(VmafxContext *context, VmafxWindow *window)
{
    assert(window->magic == VMAFX_WINDOW_MAGIC);
    const bool flushed = vmaf_engine_is_flushed(context->engine);
    if (!context->have_scored || context->scored_last < window->first) {
        if (!flushed) {
            return VMAFX_PENDING;
        }
        window->result.flags = VMAFX_WINDOW_PARTIAL;
        fail(context, window, VMAFX_E_RANGE, 0, "the stream ended before its first frame");
        return VMAFX_OK;
    }
    const uint64_t end = context->scored_last < window->last ? context->scored_last : window->last;
    const int err = advance(context, window, end);
    if (err == -EAGAIN || err == -EINVAL) {
        if (!flushed) {
            return VMAFX_PENDING;
        }
        fail(context, window, VMAFX_E_NOTFOUND, err, "a frame has no score after the flush");
        return VMAFX_OK;
    }
    if (err) {
        fail(context, window, VMAFX_E_INTERNAL, err, "the engine could not score a frame");
        return VMAFX_OK;
    }
    if (end < window->last && !flushed) {
        return VMAFX_PENDING; /* frames past the stream's end may still come */
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

/* Look at every open window once: complete those whose frames are final. */
static void evaluate(VmafxContext *context)
{
    VmafxWindowSet *const set = context->windows;
    VmafxWindow *snapshot[VMAFX_WINDOW_MAX_OPEN];
    (void)pthread_mutex_lock(&set->lock);
    const uint32_t n = snapshot_open(set, snapshot);
    (void)pthread_mutex_unlock(&set->lock);
    for (uint32_t i = 0; i < n; i++) {
        VmafxWindow *const window = snapshot[i];
        if (try_complete(context, window) == VMAFX_OK) {
            (void)pthread_mutex_lock(&set->lock);
            const bool was_open = publish(set, window);
            (void)pthread_mutex_unlock(&set->lock);
            if (was_open) {
                drop_held_ref(window); /* the open list's; the snapshot holds one */
            }
        }
        window_unref(window);
    }
}

void vmafx_windows_note_index(VmafxContext *context, uint64_t index)
{
    assert(context);
    if (!context->have_scored || index > context->scored_last) {
        context->scored_last = index;
    }
    context->have_scored = true;
    if (context->windows) {
        evaluate(context);
    }
}

void vmafx_windows_note_flush(VmafxContext *context)
{
    assert(context && vmaf_engine_is_flushed(context->engine));
    if (context->windows) {
        evaluate(context);
    }
}

/* ---- Public functions ------------------------------------------------------------ */

/* The context's window set, created with its first window; the window thread
 * is started by the first window with a callback. */
static VmafxStatus prepare_set(const VmafxReport *report, VmafxContext *context, bool needs_thread)
{
    if (!context->windows) {
        context->windows = set_new(context);
        if (!context->windows) {
            return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_CONTEXT, "context",
                              "cannot allocate the window set");
        }
    }
    VmafxWindowSet *const set = context->windows;
    if (needs_thread && !set->thread_started) {
        if (pthread_create(&set->thread, NULL, window_thread, set) != 0) {
            return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_CONTEXT, "context",
                              "cannot start the window thread");
        }
        set->thread_started = true;
    }
    return VMAFX_OK;
}

/* Put a new window on the open list: VMAFX_E_BUSY when the set is full. */
static VmafxStatus open_window(const VmafxReport *report, VmafxWindowSet *set,
                               const VmafxWindowRequest *r, VmafxWindow **out)
{
    (void)pthread_mutex_lock(&set->lock);
    const bool full = set->n_open >= VMAFX_WINDOW_MAX_OPEN;
    (void)pthread_mutex_unlock(&set->lock);
    if (full) {
        return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "%u windows are open; complete or release one first",
                          (unsigned)VMAFX_WINDOW_MAX_OPEN);
    }
    VmafxWindow *const window = window_new(set, r);
    if (!window) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "cannot allocate a window");
    }
    (void)pthread_mutex_lock(&set->lock);
    open_push(set, window);
    (void)pthread_mutex_unlock(&set->lock);
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
    VmafxWindowRequest r = VMAFX_WINDOW_REQUEST_INIT;
    VmafxStatus status = read_request(&report, request, &r);
    if (status == VMAFX_OK) {
        status = prepare_set(&report, context, r.on_complete != NULL);
    }
    VmafxWindow *window = NULL;
    if (status == VMAFX_OK) {
        status = open_window(&report, context->windows, &r, &window);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    evaluate(context); /* a window already final completes here */
    *out = window;
    return VMAFX_OK;
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
    /* A callback of this window running on the window thread finishes first,
     * unless this is that callback. */
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

void vmafx_windows_close(VmafxContext *context)
{
    VmafxWindowSet *const set = context->windows;
    if (!set) {
        return;
    }
    assert(set->refs && context->engine);
    VmafxWindow *open[VMAFX_WINDOW_MAX_OPEN];
    bool was_open[VMAFX_WINDOW_MAX_OPEN];
    (void)pthread_mutex_lock(&set->lock);
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
    (void)pthread_mutex_unlock(&set->lock);
    if (set->thread_started) {
        (void)pthread_join(set->thread, NULL); /* runs every queued callback first */
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

uint32_t vmafx_context_max_in_flight(const VmafxContext *context)
{
    if (!context) {
        return 0u;
    }
    return vmaf_engine_max_in_flight(context->engine);
}

/* NOLINTEND(modernize-use-nullptr) */
