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

/* Queries the main thread makes at most while the submitter runs, and spins the submitter makes
 * at most waiting for the first query (HISS-02). One query per submitted frame is enough to
 * overlap the workers; a query loop without a pause holds the feature collector's lock so often
 * that the 16 frames, 10 ms of work, took 8 s (and timed out under gcov, T-PROVENANCE-THREADS-
 * QUERY-STARVES-SUBMITTER-2026-10-08). */
#define MAX_QUERIES 1000u
#define MAX_SPINS 100000000u

typedef struct Submitter {
    VmafxContext *context;
    atomic_uint submitted; /* frames handed to vmafx_submit() */
    atomic_bool queried;   /* the main thread made its first query */
    atomic_bool done;
    bool ok;
} Submitter;

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
        atomic_fetch_add(&s->submitted, 1u);
    }
    free(data);
    /* The workers are still scoring: hold the flush until the main thread has queried once, so
     * the query overlaps the run however fast this host submits. */
    for (unsigned spins = 0; spins < MAX_SPINS && !atomic_load(&s->queried); spins++) {
    }
    s->ok = ok && vmafx_flush(s->context, NULL) == VMAFX_OK;
    atomic_store(&s->done, true);
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

/* Query the record once, then once more per frame the submitter hands over, until it is done;
 * false on an inconsistent record or a failed query. The first query is made whether or not the
 * submitter is done, so a host that finishes first still checks one record. */
static bool query_while_submitting(VmafxContext *context, Submitter *s, unsigned *queries)
{
    uint64_t previous = 0;
    bool ok = true;
    *queries = 0;
    do {
        VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
        ok =
            vmafx_context_provenance(context, &rec, NULL) == VMAFX_OK && consistent(&rec, previous);
        previous = rec.n_frames;
        (*queries)++;
        atomic_store(&s->queried, true);
        const unsigned last_submitted = atomic_load(&s->submitted);
        /* Wait for the next frame; an atomic read takes no lock the workers need. */
        for (unsigned spins = 0; spins < MAX_SPINS && !atomic_load(&s->done) &&
                                 atomic_load(&s->submitted) == last_submitted;
             spins++) {
        }
    } while (ok && !atomic_load(&s->done) && *queries < MAX_QUERIES);
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
    atomic_init(&s.done, false);
    atomic_init(&s.queried, false);
    atomic_init(&s.submitted, 0u);
    pthread_t thread;
    const bool started = pthread_create(&thread, NULL, submit_frames, &s) == 0;
    unsigned queries = 0;
    const bool seen = started && query_while_submitting(context, &s, &queries);
    const bool joined = started && pthread_join(thread, NULL) == 0;
    const bool last = joined && final_record(context);
    (void)vmafx_context_destroy(context, NULL);
    vmafx_model_unref(model);
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
