/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Full log routing of the VMAFx API (ADR-1906, maintainer decision
 * 2026-10-05): every message the library raises for a context with a log
 * callback, on the calling thread and on the context's worker threads,
 * reaches that callback and nothing of it reaches the process log; two
 * contexts never receive each other's lines; a context without a callback
 * logs to the process log at its level; model loads route to the callback of
 * their configuration.
 *
 * The process log is observed by redirecting file descriptors 1 and 2 into a
 * temporary file around the calls under test (POSIX; the test is built on
 * Linux only).
 *
 * Failing first, measured with planted defects on this branch: without the
 * job sink in core/src/libvmaf.c (threaded_extract_batch_func) the worker
 * messages go to stderr and test_worker_messages_reach_callback and
 * test_two_contexts_concurrent_workers fail; a sink installed process-wide
 * instead of per thread fails test_two_contexts_interleaved; model loads
 * without their sink fail test_model_load_messages_routed.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#ifndef VMAFX_TEST_MODEL_DIR
#error "VMAFX_TEST_MODEL_DIR: the model directory, set by core/test/meson.build"
#endif

enum { W = 176, H = 144, N_FRAMES = 6, MAX_LINES = 64, CAPTURE_MAX = 4096 };

/* Set on every thread the test starts; library worker threads leave it 0. */
static _Thread_local int on_test_thread;

/* ---- A thread-safe recorder ------------------------------------------------------- */

typedef struct Recorder {
    pthread_mutex_t lock;
    unsigned lines;
    unsigned worker_lines;
    unsigned foreign_lines; /* lines without `tag`, or with `forbidden` */
    const char *tag;        /* NULL: any line is the recorder's own */
    const char *forbidden;  /* NULL: none */
    char last[512];
    /* Interleaving hooks (test_two_contexts_interleaved). */
    void (*on_first_line)(void *hook);
    void *hook;
} Recorder;

static void recorder_init(Recorder *r, const char *tag)
{
    memset(r, 0, sizeof(*r));
    (void)pthread_mutex_init(&r->lock, NULL);
    r->tag = tag;
}

static void record_line(uint32_t level, const char *message, void *user)
{
    (void)level;
    Recorder *const r = user;
    (void)pthread_mutex_lock(&r->lock);
    const bool first = r->lines == 0;
    r->lines++;
    r->worker_lines += on_test_thread ? 0u : 1u;
    r->foreign_lines += (r->tag && !strstr(message, r->tag)) ? 1u : 0u;
    r->foreign_lines += (r->forbidden && strstr(message, r->forbidden)) ? 1u : 0u;
    (void)snprintf(r->last, sizeof(r->last), "%s", message);
    (void)pthread_mutex_unlock(&r->lock);
    if (first && r->on_first_line) {
        r->on_first_line(r->hook);
    }
}

static VmafxContext *recorded_context(Recorder *r, uint32_t level, uint32_t n_threads)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.log_level = level;
    config.n_threads = n_threads;
    config.log_callback = record_line;
    config.log_user = r;
    VmafxContext *context = NULL;
    return vmafx_context_create(&config, &context, NULL) == VMAFX_OK ? context : NULL;
}

/* ---- The process log ------------------------------------------------------------------- */

/* stdout and stderr redirected into one temporary file. */
typedef struct Capture {
    FILE *file;
    int saved_out;
    int saved_err;
} Capture;

static bool capture_begin(Capture *c)
{
    (void)fflush(stdout);
    (void)fflush(stderr);
    c->file = tmpfile();
    c->saved_out = dup(STDOUT_FILENO);
    c->saved_err = dup(STDERR_FILENO);
    return c->file && c->saved_out >= 0 && c->saved_err >= 0 &&
           dup2(fileno(c->file), STDOUT_FILENO) >= 0 && dup2(fileno(c->file), STDERR_FILENO) >= 0;
}

/* Put a saved descriptor back on `fd`. */
static void restore_fd(int saved, int fd)
{
    if (saved >= 0) {
        (void)dup2(saved, fd);
        (void)close(saved);
    }
}

/* Restore stdout and stderr; `text` receives what was written meanwhile. */
static void capture_end(Capture *c, char text[CAPTURE_MAX])
{
    (void)fflush(stdout);
    (void)fflush(stderr);
    restore_fd(c->saved_out, STDOUT_FILENO);
    restore_fd(c->saved_err, STDERR_FILENO);
    text[0] = '\0';
    if (!c->file) {
        return;
    }
    if (fseek(c->file, 0, SEEK_SET) == 0) {
        const size_t n = fread(text, 1, CAPTURE_MAX - 1, c->file);
        text[n < CAPTURE_MAX ? n : 0] = '\0';
    }
    (void)fclose(c->file);
}

/* ---- Sessions ----------------------------------------------------------------------- */

/* vmaf_v0.6.1 with ADM at a normalised viewing distance the default CSF
 * table refuses: every ADM extract fails and the engine warns about it on
 * the thread that runs the extract. */
static bool use_failing_adm(VmafxContext *context)
{
    VmafxModel *model = NULL;
    VmafxOptions *options = NULL;
    const bool ok = vmafx_model_load(NULL, "vmaf_v0.6.1", &model, NULL) == VMAFX_OK &&
                    vmafx_options_set(&options, "adm_norm_view_dist", "1.0", NULL) == VMAFX_OK &&
                    vmafx_model_override_feature(model, "adm", options, NULL) == VMAFX_OK &&
                    vmafx_context_use_model(context, model, NULL) == VMAFX_OK;
    vmafx_options_free(options);
    vmafx_model_unref(model);
    return ok;
}

/* Submit N_FRAMES generated frames and flush; returns the flush status. */
static VmafxStatus run_frames(VmafxContext *context)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    uint8_t *data = malloc(vt_frame_bytes(&desc));
    bool ok = data != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        vt_fill(&desc, data, i);
        VmafxFrame *ref = vt_copy_frame(&desc, data);
        vt_fill(&desc, data, i + 50u);
        VmafxFrame *dist = vt_copy_frame(&desc, data);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    free(data);
    VmafxError *error = NULL;
    const VmafxStatus status = ok ? vmafx_flush(context, &error) : VMAFX_E_INVALID;
    vmafx_error_free(error);
    return status;
}

/* ---- Worker threads ----------------------------------------------------------------- */

static char *test_worker_messages_reach_callback(void)
{
    on_test_thread = 1;
    Recorder r;
    recorder_init(&r, "adm");
    VmafxContext *context = recorded_context(&r, VMAFX_LOG_LEVEL_WARNING, 4);
    mu_assert("context", context && use_failing_adm(context));
    Capture capture;
    char text[CAPTURE_MAX];
    const bool captured = capture_begin(&capture);
    const VmafxStatus status = run_frames(context);
    capture_end(&capture, text);
    mu_assert("capture", captured);
    mu_assert("the failing extractor fails the flush", status != VMAFX_OK);
    mu_assert("worker messages reached the callback", r.worker_lines > 0 && r.foreign_lines == 0);
    mu_assert("nothing reached the process log", text[0] == '\0');
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Two contexts, deterministic interleaving ----------------------------------------- */

/* Thread A registers a model at DEBUG, one call that logs several lines.
 * A's first line blocks A inside that call until thread B has logged its own
 * line and is blocked inside B's callback, with B's sink active on B's
 * thread. A then logs its remaining lines from the same call: they must
 * reach A, never B, and B's line must reach B only. A process-wide sink
 * would hand A's remaining lines to B. */
typedef struct Interleave {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool a_waiting;
    bool b_in_callback;
    bool a_done;
    unsigned a_lines_at_done;
    Recorder a;
    Recorder b;
} Interleave;

/* Wait on `s->changed` until `*flag`; bounded (HISS-02). */
static void wait_for(Interleave *s, const bool *flag)
{
    (void)pthread_mutex_lock(&s->lock);
    for (unsigned i = 0; i < 1000000u && !*flag; i++) {
        (void)pthread_cond_wait(&s->changed, &s->lock);
    }
    (void)pthread_mutex_unlock(&s->lock);
}

static void raise_flag(Interleave *s, bool *flag)
{
    (void)pthread_mutex_lock(&s->lock);
    *flag = true;
    (void)pthread_cond_broadcast(&s->changed);
    (void)pthread_mutex_unlock(&s->lock);
}

static void a_first_line(void *hook)
{
    Interleave *const s = hook;
    raise_flag(s, &s->a_waiting);
    wait_for(s, &s->b_in_callback);
}

static void b_first_line(void *hook)
{
    Interleave *const s = hook;
    raise_flag(s, &s->b_in_callback);
    wait_for(s, &s->a_done);
}

static void *thread_a(void *arg)
{
    on_test_thread = 1;
    Interleave *const s = arg;
    VmafxContext *context = recorded_context(&s->a, VMAFX_LOG_LEVEL_DEBUG, 0);
    VmafxModel *model = NULL;
    if (context && vmafx_model_load(NULL, "vmaf_v0.6.1", &model, NULL) == VMAFX_OK) {
        (void)vmafx_context_use_model(context, model, NULL);
    }
    vmafx_model_unref(model);
    (void)pthread_mutex_lock(&s->a.lock);
    s->a_lines_at_done = s->a.lines;
    (void)pthread_mutex_unlock(&s->a.lock);
    raise_flag(s, &s->a_done);
    (void)vmafx_context_destroy(context, NULL);
    return NULL;
}

static void *thread_b(void *arg)
{
    on_test_thread = 1;
    Interleave *const s = arg;
    wait_for(s, &s->a_waiting);
    VmafxContext *context = recorded_context(&s->b, VMAFX_LOG_LEVEL_ERROR, 0);
    VmafxOptions *options = NULL;
    VmafxError *error = NULL;
    if (context && vmafx_options_set(&options, "option_of_context_b", "1", NULL) == VMAFX_OK) {
        (void)vmafx_context_use_feature(context, "psnr", options, &error);
    }
    vmafx_error_free(error);
    vmafx_options_free(options);
    (void)vmafx_context_destroy(context, NULL);
    return NULL;
}

static char *test_two_contexts_interleaved(void)
{
    static Interleave s;
    memset(&s, 0, sizeof(s));
    (void)pthread_mutex_init(&s.lock, NULL);
    (void)pthread_cond_init(&s.changed, NULL);
    recorder_init(&s.a, NULL);
    recorder_init(&s.b, "option_of_context_b");
    s.a.forbidden = "option_of_context_b";
    s.a.on_first_line = a_first_line;
    s.a.hook = &s;
    s.b.on_first_line = b_first_line;
    s.b.hook = &s;
    pthread_t ta;
    pthread_t tb;
    mu_assert("threads", pthread_create(&ta, NULL, thread_a, &s) == 0 &&
                             pthread_create(&tb, NULL, thread_b, &s) == 0);
    mu_assert("join", pthread_join(ta, NULL) == 0 && pthread_join(tb, NULL) == 0);
    mu_assert("the threads interleaved", s.a_waiting && s.b_in_callback && s.a_done);
    mu_assert("A logged more lines while B's sink was active", s.a_lines_at_done >= 2);
    mu_assert("A's lines reached A only", s.a.foreign_lines == 0 && s.a.lines >= 2);
    mu_assert("B's line reached B only", s.b.lines == 1 && s.b.foreign_lines == 0);
    return NULL;
}

/* ---- Two contexts, concurrent worker threads ------------------------------------------- */

typedef struct Concurrent {
    Recorder *recorder;
    bool failing_adm;
    bool ok;
} Concurrent;

static void *run_session(void *arg)
{
    on_test_thread = 1;
    Concurrent *const c = arg;
    VmafxContext *context = recorded_context(c->recorder, VMAFX_LOG_LEVEL_WARNING, 2);
    c->ok = context &&
            (c->failing_adm ? use_failing_adm(context) :
                              vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK);
    const VmafxStatus status = c->ok ? run_frames(context) : VMAFX_E_INVALID;
    c->ok = c->ok && (status == VMAFX_OK) != c->failing_adm;
    c->ok = c->ok && vmafx_context_destroy(context, NULL) == VMAFX_OK;
    return NULL;
}

static char *test_two_contexts_concurrent_workers(void)
{
    for (unsigned round = 0; round < 3u; round++) {
        Recorder noisy;
        Recorder quiet;
        recorder_init(&noisy, "adm");
        recorder_init(&quiet, NULL);
        Concurrent a = {.recorder = &noisy, .failing_adm = true, .ok = false};
        Concurrent b = {.recorder = &quiet, .failing_adm = false, .ok = false};
        pthread_t ta;
        pthread_t tb;
        mu_assert("threads", pthread_create(&ta, NULL, run_session, &a) == 0 &&
                                 pthread_create(&tb, NULL, run_session, &b) == 0);
        mu_assert("join", pthread_join(ta, NULL) == 0 && pthread_join(tb, NULL) == 0);
        mu_assert("sessions", a.ok && b.ok);
        mu_assert("the noisy context got its worker lines",
                  noisy.worker_lines > 0 && noisy.foreign_lines == 0);
        mu_assert("the quiet context got none of them", quiet.lines == 0);
    }
    return NULL;
}

/* ---- Without a callback: the process log at the context's level ------------------------ */

static void log_unknown_option_plain(uint32_t level, char text[CAPTURE_MAX], bool *captured)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.log_level = level;
    VmafxContext *context = NULL;
    VmafxOptions *options = NULL;
    VmafxError *error = NULL;
    Capture capture;
    *captured = capture_begin(&capture);
    if (vmafx_context_create(&config, &context, NULL) == VMAFX_OK &&
        vmafx_options_set(&options, "option_of_plain_context", "1", NULL) == VMAFX_OK) {
        (void)vmafx_context_use_feature(context, "psnr", options, &error);
    }
    capture_end(&capture, text);
    vmafx_error_free(error);
    vmafx_options_free(options);
    (void)vmafx_context_destroy(context, NULL);
}

static char *test_plain_context_logs_to_process_log(void)
{
    on_test_thread = 1;
    char text[CAPTURE_MAX];
    bool captured = false;
    log_unknown_option_plain(VMAFX_LOG_LEVEL_ERROR, text, &captured);
    mu_assert("at ERROR the engine's line reaches the process log",
              captured && strstr(text, "option_of_plain_context") != NULL);
    log_unknown_option_plain(VMAFX_LOG_LEVEL_NONE, text, &captured);
    mu_assert("at NONE nothing does", captured && text[0] == '\0');
    vmaf_set_log_level(VMAF_LOG_LEVEL_INFO);
    return NULL;
}

/* ---- Model loads ---------------------------------------------------------------------- */

/* A copy of vmaf_v1.0.16_3d0h.json whose cambi_max_val is an array, which
 * the JSON loader refuses with an ERROR line naming the key. */
static bool write_broken_model(char path[256])
{
    size_t size = 0;
    char *text = NULL;
    FILE *in = fopen(VMAFX_TEST_MODEL_DIR "/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json", "rb");
    if (in && fseek(in, 0, SEEK_END) == 0) {
        const long end = ftell(in);
        size = end > 0 ? (size_t)end : 0u;
        text = size && fseek(in, 0, SEEK_SET) == 0 ? calloc(1, size + 2u) : NULL;
    }
    const bool read = text && fread(text, 1, size, in) == size;
    if (in) {
        (void)fclose(in);
    }
    char *const key = read ? strstr(text, "\"cambi_max_val\": 17.0") : NULL;
    if (key) {
        /* 17.0 -> [17], the same length. */
        char *const value = key + sizeof("\"cambi_max_val\": ") - 1u;
        value[0] = '[';
        value[1] = '1';
        value[2] = '7';
        value[3] = ']';
    }
    (void)snprintf(path, 256, "%s", "vmafx_broken_model_XXXXXX");
    const int fd = key ? mkstemp(path) : -1;
    const bool ok = fd >= 0 && write(fd, text, size) == (ssize_t)size;
    if (fd >= 0) {
        (void)close(fd);
    }
    free(text);
    return ok;
}

static char *test_model_load_messages_routed(void)
{
    on_test_thread = 1;
    char path[256];
    mu_assert("fixture", write_broken_model(path));
    Recorder r;
    recorder_init(&r, "cambi_max_val");
    VmafxModelConfig config = VMAFX_MODEL_CONFIG_INIT;
    config.log_level = VMAFX_LOG_LEVEL_ERROR;
    config.log_callback = record_line;
    config.log_user = &r;
    VmafxModel *model = NULL;
    VmafxError *error = NULL;
    Capture capture;
    char text[CAPTURE_MAX];
    const bool captured = capture_begin(&capture);
    const VmafxStatus status = vmafx_model_load_file(&config, path, &model, &error);
    capture_end(&capture, text);
    (void)unlink(path);
    vmafx_error_free(error);
    mu_assert("refused", captured && status == VMAFX_E_INVALID && model == NULL);
    mu_assert("the loader's line reached the callback", r.lines >= 1 && r.foreign_lines == 0);
    mu_assert("nothing reached the process log", text[0] == '\0');
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_worker_messages_reach_callback),    MU_TEST(test_two_contexts_interleaved),
        MU_TEST(test_two_contexts_concurrent_workers),   MU_TEST(test_model_load_messages_routed),
        MU_TEST(test_plain_context_logs_to_process_log),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
