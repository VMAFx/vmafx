/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A provenance record query while another thread submits (#2142, ADR-2073,
 * RC4 WP5). Design section 2.5 lets vmafx_context_provenance() run while the
 * context scores; the record's frame count, geometry and times are written by
 * the submitting thread. Under ThreadSanitizer (the Sanitizers job's TSan
 * build, or `-Db_sanitize=thread`) an unsynchronised read is reported and the
 * test exits nonzero; in every build each record the query sees must be
 * consistent: the geometry is there as soon as a frame is counted, and the
 * count never goes back.
 *
 * Failing first: with the frame count, geometry and times read from the
 * engine's plain fields, a TSan build reports `data race` on them here.
 *
 * The overlap is deterministic: the submitter waits after its first frame
 * until the main thread has queried once, and the main thread queries once
 * per frame the submitter hands over until it is done. Without that
 * handshake a host that submits the 16 frames before the main thread's
 * first query made no query at all (the macOS legs, "the query ran while
 * frames were submitted").
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { W = 176, H = 144, N_FRAMES = 16 };

/* A model whose features accept 176x144 frames. */
#define MODEL_VERSION "vmaf_v0.6.1" /* vmaf-model-pin: features fit 176x144 frames */

/* Queries the main thread makes at most while the submitter runs, one per
 * frame count it sees and one more; and wake-ups either thread takes at most
 * waiting for the other (HISS-02). Every exit path of both threads
 * broadcasts, so no wait outlives the other thread. */
#define MAX_QUERIES (N_FRAMES + 2u)
#define MAX_WAKEUPS 1000000u

/* The handshake between the submitting thread and the querying main thread.
 * `submitted`, `queried`, `done` and `stopped` are read and written under
 * `lock`; `sync_failed` records a failed pthread call on either thread. */
typedef struct Submitter {
    VmafxContext *context;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned submitted; /* frames handed to vmafx_submit() */
    unsigned queried;   /* queries the main thread completed */
    bool done;          /* the submitter flushed (or failed) */
    bool stopped;       /* the main thread stopped querying */
    atomic_bool sync_failed;
    bool ok;
} Submitter;

static bool sync_ok(Submitter *s, int err)
{
    if (err != 0) {
        atomic_store(&s->sync_failed, true);
    }
    return err == 0;
}

static bool lock(Submitter *s)
{
    return sync_ok(s, pthread_mutex_lock(&s->lock));
}

static void unlock(Submitter *s)
{
    (void)sync_ok(s, pthread_mutex_unlock(&s->lock));
}

/* Under s->lock: count `submitted` frames and `queried` queries, and wake the
 * other thread. */
static void announce(Submitter *s, unsigned submitted, unsigned queried)
{
    s->submitted += submitted;
    s->queried += queried;
    (void)sync_ok(s, pthread_cond_broadcast(&s->changed));
}

/* Under s->lock: one wait for the other thread; false (and recorded) when it
 * fails. */
static bool wait_once(Submitter *s)
{
    return sync_ok(s, pthread_cond_wait(&s->changed, &s->lock));
}

/* Submitter, under s->lock, after its first frame: wait until the main thread
 * has queried once, or stopped querying. */
static void wait_for_first_query(Submitter *s)
{
    for (unsigned w = 0; w < MAX_WAKEUPS && s->queried == 0u && !s->stopped; w++) {
        if (!wait_once(s)) {
            return;
        }
    }
}

/* Main thread, under s->lock, after a query: wait until the submitter hands
 * over another frame than the `seen` ones, or is done. */
static void wait_for_next_frame(Submitter *s, unsigned seen)
{
    for (unsigned w = 0; w < MAX_WAKEUPS && s->submitted == seen && !s->done; w++) {
        if (!wait_once(s)) {
            return;
        }
    }
}

/* Count a submitted frame; after the first one, wait until the main thread
 * has queried once, so that query overlaps the rest of the run however fast
 * this host submits. */
static void frame_submitted(Submitter *s, unsigned i)
{
    if (!lock(s)) {
        return;
    }
    announce(s, 1u, 0u);
    if (i == 0u) {
        wait_for_first_query(s);
    }
    unlock(s);
}

/* Mark `flag` (done or stopped) and wake the other thread. */
static void finish(Submitter *s, bool *flag)
{
    if (lock(s)) {
        *flag = true;
        announce(s, 0u, 0u);
        unlock(s);
    }
}

static void *submit_frames(void *arg)
{
    Submitter *const s = arg;
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    uint8_t *const data = malloc(vt_frame_bytes(&desc));
    bool ok = data != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        vt_fill(&desc, data, i);
        VmafxFrame *const ref = vt_copy_frame(&desc, data);
        vt_fill(&desc, data, i + 50u);
        VmafxFrame *const dist = vt_copy_frame(&desc, data);
        ok = vmafx_submit(s->context, ref, dist, i, NULL) == VMAFX_OK;
        frame_submitted(s, i);
    }
    free(data);
    s->ok = ok && vmafx_flush(s->context, NULL) == VMAFX_OK;
    finish(s, &s->done);
    return NULL;
}

/* A record seen mid-run: the count did not go back, and a counted frame comes
 * with its geometry. */
static bool consistent(const VmafxProvenance *rec, uint64_t previous)
{
    if (rec->n_frames < previous || rec->n_frames > N_FRAMES) {
        return false;
    }
    return rec->n_frames == 0 || (rec->frame_width == W && rec->frame_height == H &&
                                  rec->bpc == 8u && rec->pix_fmt == VMAFX_PIXEL_FORMAT_YUV420P);
}

/* Whether the submitter is done. */
static bool submitter_done(Submitter *s)
{
    if (!lock(s)) {
        return true;
    }
    const bool done = s->done;
    unlock(s);
    return done;
}

/* Count a query, then wait until the submitter hands over another frame than
 * the `seen` ones, or is done. Returns the frames handed over so far. */
static unsigned query_made(Submitter *s, unsigned seen)
{
    if (!lock(s)) {
        return seen;
    }
    announce(s, 0u, 1u);
    wait_for_next_frame(s, seen);
    const unsigned submitted = s->submitted;
    unlock(s);
    return submitted;
}

/* Query the record while the submitter runs, once per frame count it sees,
 * until it is done; `queries` counts the queries made before it was done.
 * false on an inconsistent record or a failed query. */
static bool query_while_submitting(VmafxContext *context, Submitter *s, unsigned *queries)
{
    uint64_t previous = 0;
    unsigned seen = 0;
    bool ok = true;
    for (*queries = 0; ok && !submitter_done(s) && *queries < MAX_QUERIES; (*queries)++) {
        VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
        ok =
            vmafx_context_provenance(context, &rec, NULL) == VMAFX_OK && consistent(&rec, previous);
        previous = rec.n_frames;
        seen = query_made(s, seen);
    }
    finish(s, &s->stopped);
    return ok;
}

static VmafxContext *scoring_context(uint32_t n_threads, VmafxModel **model)
{
    VmafxContext *context = NULL;
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.n_threads = n_threads;
    const bool ok = vmafx_context_create(&config, &context, NULL) == VMAFX_OK &&
                    vmafx_model_load(NULL, MODEL_VERSION, model, NULL) == VMAFX_OK &&
                    vmafx_context_use_model(context, *model, NULL) == VMAFX_OK;
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

static bool final_record(VmafxContext *context)
{
    VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
    return vmafx_context_provenance(context, &rec, NULL) == VMAFX_OK && rec.n_frames == N_FRAMES &&
           consistent(&rec, N_FRAMES) && rec.elapsed_ns > 0u;
}

/* Submit on one thread, query on this one, with `n_threads` workers scoring. */
static char *query_while_submitting_with(uint32_t n_threads)
{
    VmafxModel *model = NULL;
    VmafxContext *const context = scoring_context(n_threads, &model);
    mu_assert("session", context != NULL);
    Submitter s = {.context = context, .ok = false};
    atomic_init(&s.sync_failed, false);
    const bool sync = pthread_mutex_init(&s.lock, NULL) == 0;
    const bool cond = sync && pthread_cond_init(&s.changed, NULL) == 0;
    pthread_t thread;
    const bool started = cond && pthread_create(&thread, NULL, submit_frames, &s) == 0;
    unsigned queries = 0;
    const bool seen = started && query_while_submitting(context, &s, &queries);
    const bool joined = started && pthread_join(thread, NULL) == 0;
    const bool last = joined && final_record(context);
    (void)vmafx_context_destroy(context, NULL);
    vmafx_model_unref(model);
    const bool released = (!cond || pthread_cond_destroy(&s.changed) == 0) &&
                          (!sync || pthread_mutex_destroy(&s.lock) == 0);
    mu_assert("handshake", cond && released && !atomic_load(&s.sync_failed));
    mu_assert("submitter thread", started && joined && s.ok);
    mu_assert("every record seen mid-run is consistent", seen);
    mu_assert("the query ran while frames were submitted", queries > 0u);
    mu_assert("final record", last);
    return NULL;
}

static char *test_query_while_submitting(void)
{
    char *const failed = query_while_submitting_with(0u);
    return failed ? failed : query_while_submitting_with(2u);
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_query_while_submitting),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
