/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The process log level is shared by every thread
 * (T-LOG-LEVEL-GLOBAL-DATA-RACE-2026-10-06).
 *
 * vmaf_init() sets the process log level (vmaf_set_log_level() writes the
 * level and the stderr tty flag in core/src/log.cpp) and vmaf_log() reads
 * both on whatever thread logs: a caller's thread or a context's worker
 * thread. Two contexts created on two threads, or one created while another
 * logs, wrote and read plain globals at the same time: a data race, undefined
 * behaviour in C and C++.
 *
 * This test creates and closes contexts on two threads while a third thread
 * logs. It only proves something under ThreadSanitizer (`-Db_sanitize=thread`,
 * the nightly and master TSan jobs run the fast suite): before the fix TSan
 * reports the race and the test exits 66; with the level and the flag atomic
 * it is clean. Without TSan it checks that concurrent creation, logging and
 * closing complete.
 *
 * Failing first, measured on master 782eba01f under TSan: "WARNING:
 * ThreadSanitizer: data race" on `(anonymous namespace)::vmaf_log_level`
 * (write in vmaf_set_log_level() from vmaf_init(), read in vmaf_log()).
 */

#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "log.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { ROUNDS = 64 };

typedef struct Start {
    pthread_mutex_t lock;
    pthread_cond_t go;
    unsigned waiting;
    bool started;
} Start;

typedef struct Worker {
    Start *start;
    enum VmafLogLevel level;
    bool ok;
} Worker;

/* Wait until every thread is ready, so the loops overlap. */
static void wait_for_start(Start *s)
{
    (void)pthread_mutex_lock(&s->lock);
    s->waiting++;
    (void)pthread_cond_broadcast(&s->go);
    for (unsigned i = 0; i < 1000000u && !s->started; i++) {
        (void)pthread_cond_wait(&s->go, &s->lock);
    }
    (void)pthread_mutex_unlock(&s->lock);
}

/* Create and close contexts at `level`: each vmaf_init() sets the process
 * level. NONE and ERROR keep the logging thread's DEBUG lines silent. */
static void *create_contexts(void *arg)
{
    Worker *const w = arg;
    wait_for_start(w->start);
    w->ok = true;
    for (unsigned i = 0; i < ROUNDS && w->ok; i++) {
        VmafConfiguration cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.log_level = w->level;
        VmafContext *vmaf = NULL;
        w->ok = vmaf_init(&vmaf, cfg) == 0 && vmaf_close(vmaf) == 0;
    }
    return NULL;
}

/* Log below the level the other threads set: vmaf_log() reads the level
 * (and, for a printed line, the tty flag) on this thread. */
static void *log_lines(void *arg)
{
    Worker *const w = arg;
    wait_for_start(w->start);
    for (unsigned i = 0; i < ROUNDS * 4u; i++) {
        vmaf_log(VMAF_LOG_LEVEL_DEBUG, "log_level_threads: line %u\n", i);
    }
    w->ok = true;
    return NULL;
}

static char *test_level_shared_across_threads(void)
{
    static Start start;
    memset(&start, 0, sizeof(start));
    (void)pthread_mutex_init(&start.lock, NULL);
    (void)pthread_cond_init(&start.go, NULL);
    Worker a = {.start = &start, .level = VMAF_LOG_LEVEL_NONE, .ok = false};
    Worker b = {.start = &start, .level = VMAF_LOG_LEVEL_ERROR, .ok = false};
    Worker c = {.start = &start, .level = VMAF_LOG_LEVEL_NONE, .ok = false};
    pthread_t ta;
    pthread_t tb;
    pthread_t tc;
    mu_assert("threads", pthread_create(&ta, NULL, create_contexts, &a) == 0 &&
                             pthread_create(&tb, NULL, create_contexts, &b) == 0 &&
                             pthread_create(&tc, NULL, log_lines, &c) == 0);
    (void)pthread_mutex_lock(&start.lock);
    for (unsigned i = 0; i < 1000000u && start.waiting < 3u; i++) {
        (void)pthread_cond_wait(&start.go, &start.lock);
    }
    start.started = true;
    (void)pthread_cond_broadcast(&start.go);
    (void)pthread_mutex_unlock(&start.lock);
    mu_assert("join", pthread_join(ta, NULL) == 0 && pthread_join(tb, NULL) == 0 &&
                          pthread_join(tc, NULL) == 0);
    mu_assert("contexts created and closed", a.ok && b.ok && c.ok);
    vmaf_set_log_level(VMAF_LOG_LEVEL_INFO);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_level_shared_across_threads);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
