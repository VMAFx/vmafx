/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Fence ordering, the import rule (decision D8), admission and the host-copy
 * counter of VMAFx imported frames on the CPU device (RC4 WP3 common lane).
 *
 * Each test runs once correctly and once with the defect it must catch
 * planted through a test-only switch (core/src/vmafx/frame_import_hooks.h),
 * and asserts the planted run goes wrong; a test that passed with its defect
 * planted would be no test:
 *
 * - acquire: a producer thread writes a frame and then signals its HOST
 *   acquire fence. vmafx_context_import_frame() waits for it and the score
 *   is right; with the wait skipped the frame is read before it is written
 *   and the score is wrong.
 * - release: after every submit the producer overwrites each buffer whose
 *   release fence is signalled (a canary). The scores equal a run without
 *   canaries; with the release signalled when the submit returns, the
 *   canary lands in frame n-1 while motion_v2 still reads it and the scores
 *   change.
 * - host copies: a planar import binds the producer's memory and the
 *   counter stays 0; with the copy planted the counter sees it.
 * - D8: a transient failure is retried once, a second one fails, a
 *   non-transient one is not retried; each failure names the import.
 * - admission: a frame in device memory is refused naming every extractor
 *   that would need a host copy of it.
 *
 * POSIX threads: Linux only (core/test/meson.build).
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_import_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { W = 176, H = 144, N_FRAMES = 6 };

#define MOTION_SAD "VMAF_integer_feature_motion_v2_sad_score"

/* Worker threads of the contexts the canary runs make (0: the caller's). */
static uint32_t canary_threads;

static VmafxContext *feature_context(const char *a, const char *b)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.n_threads = canary_threads;
    VmafxContext *context = NULL;
    if (vmafx_context_create(&config, &context, NULL) != VMAFX_OK) {
        return NULL;
    }
    if (vmafx_context_use_feature(context, a, NULL, NULL) != VMAFX_OK ||
        (b && vmafx_context_use_feature(context, b, NULL, NULL) != VMAFX_OK)) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

static bool contains(const char *text, const char *part)
{
    return strstr(text, part) != NULL;
}

/* ---- Acquire fences ------------------------------------------------------------------------ */

/* Bytes of one 8-bit 4:2:0 W x H frame (W and H are even). */
#define FRAME_BYTES ((size_t)W * (size_t)H * 3u / 2u)

/* Frames the producer writes, the content they get, and the canary ring. */
static uint8_t produced[2][FRAME_BYTES];
static uint8_t source[2 * N_FRAMES][FRAME_BYTES];
static uint8_t ring_buf[2 * N_FRAMES][FRAME_BYTES];

typedef struct Producer {
    VmafxFence go;    /* the producer starts writing once this is signalled */
    VmafxFence ready; /* the acquire fence the producer signals */
} Producer;

/* The producer: wait for `go`, write both frames, signal `ready`. */
static void *produce(void *arg)
{
    Producer *const p = arg;
    if (vmafx_fence_wait(&p->go, UINT64_MAX, NULL) == VMAFX_OK) {
        memcpy(produced, source, sizeof(produced));
        (void)vmafx_fence_signal(&p->ready, NULL);
    }
    return NULL;
}

/* Import both produced frames through the import rule and submit them as
 * frame 0. */
static bool submit_produced(VmafxContext *context, const Producer *p, const VmafxFrameDesc *d)
{
    VmafxFrameImport ref = vt_import_planar(d, produced[0]);
    VmafxFrameImport dist = vt_import_planar(d, produced[1]);
    ref.acquire = p->ready;
    dist.acquire = p->ready;
    VmafxFrame *frames[2] = {NULL, NULL};
    if (vmafx_context_import_frame(context, NULL, &ref, "reference", &frames[0], NULL) !=
        VMAFX_OK) {
        return false;
    }
    if (vmafx_context_import_frame(context, NULL, &dist, "main", &frames[1], NULL) != VMAFX_OK) {
        vmafx_frame_unref(frames[0]);
        return false;
    }
    return vmafx_submit(context, frames[0], frames[1], 0, NULL) == VMAFX_OK;
}

/* psnr_y of frame 0 once the context is flushed. */
static bool psnr_y_of(VmafxContext *context, double *psnr)
{
    VmafxScore score = VMAFX_SCORE_INIT;
    if (vmafx_flush(context, NULL) != VMAFX_OK ||
        vmafx_feature_score(context, "psnr_y", 0, &score, NULL) != VMAFX_OK) {
        return false;
    }
    *psnr = score.value;
    return true;
}

/* PSNR of frame 0 scored from the frames a producer thread writes, with the
 * acquire wait skipped (planted) or not. */
static char *psnr_after_producer(const VmafxFrameDesc *d, bool skip, double *psnr)
{
    memset(produced, 0, sizeof(produced));
    VmafxContext *const context = feature_context("psnr", NULL);
    Producer p = {.go = VMAFX_FENCE_INIT, .ready = VMAFX_FENCE_INIT};
    mu_assert("setup", context &&
                           vmafx_fence_create(NULL, VMAFX_FENCE_HOST, &p.go, NULL) == VMAFX_OK &&
                           vmafx_fence_create(NULL, VMAFX_FENCE_HOST, &p.ready, NULL) == VMAFX_OK);
    pthread_t thread;
    mu_assert("thread", pthread_create(&thread, NULL, produce, &p) == 0);
    vmafx_test_set_switches(skip ? VMAFX_TEST_SKIP_ACQUIRE_WAIT : 0u);
    if (!skip) {
        (void)vmafx_fence_signal(&p.go, NULL); /* written before the import may read */
    }
    const bool submitted = submit_produced(context, &p, d);
    vmafx_test_set_switches(0u);
    (void)vmafx_fence_signal(&p.go, NULL); /* skip: written after the submit read */
    mu_assert("join", pthread_join(thread, NULL) == 0);
    mu_assert("scored", submitted && psnr_y_of(context, psnr));
    mu_assert("teardown", vmafx_context_destroy(context, NULL) == VMAFX_OK &&
                              vmafx_fence_destroy(&p.go, NULL) == VMAFX_OK &&
                              vmafx_fence_destroy(&p.ready, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_acquire_ordering(void)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    vt_fill(&d, source[0], 1u);
    vt_fill(&d, source[1], 9u);
    VmafxContext *const context = feature_context("psnr", NULL);
    double expected = 0.0;
    mu_assert("expected",
              context &&
                  vmafx_submit(context, vt_wrap_frame(&d, source[0], NULL),
                               vt_wrap_frame(&d, source[1], NULL), 0, NULL) == VMAFX_OK &&
                  psnr_y_of(context, &expected) &&
                  vmafx_context_destroy(context, NULL) == VMAFX_OK);
    double waited = 0.0;
    double skipped = 0.0;
    mu_assert_msg(psnr_after_producer(&d, false, &waited));
    mu_assert_msg(psnr_after_producer(&d, true, &skipped));
    mu_assert("with the acquire wait: the producer's frame", vt_same_bits(waited, expected));
    mu_assert("acquire wait skipped (planted): a frame read before it was written",
              !vt_same_bits(skipped, expected));
    return NULL;
}

/* ---- Release fences --------------------------------------------------------------------------- */

typedef struct Ring {
    VmafxFence released[2 * N_FRAMES];
    bool overwritten[2 * N_FRAMES];
} Ring;

/* Overwrite every buffer whose release fence is signalled: what a producer
 * that reuses its memory at the release fence does. */
static void canary(Ring *ring, unsigned n)
{
    for (unsigned j = 0; j < n; j++) {
        if (!ring->overwritten[j] && vmafx_fence_wait(&ring->released[j], 0u, NULL) == VMAFX_OK) {
            memset(ring_buf[j], 0xa5, FRAME_BYTES);
            ring->overwritten[j] = true;
        }
    }
}

/* Import ring buffer `j` with a release fence. */
static VmafxFrame *import_released(Ring *ring, unsigned j, const VmafxFrameDesc *d)
{
    const VmafxFrameImport imp = vt_import_planar(d, ring_buf[j]);
    VmafxFrame *frame = NULL;
    if (vmafx_frame_import(NULL, &imp, &frame, NULL) != VMAFX_OK) {
        return NULL;
    }
    if (vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &ring->released[j], NULL) != VMAFX_OK) {
        vmafx_frame_unref(frame);
        return NULL;
    }
    return frame;
}

/* Submit every frame of the ring and overwrite released buffers after each
 * submit. */
static char *canary_submit(VmafxContext *context, Ring *ring, const VmafxFrameDesc *d)
{
    for (unsigned i = 0; i < N_FRAMES; i++) {
        VmafxFrame *ref = import_released(ring, 2u * i, d);
        VmafxFrame *dist = import_released(ring, 2u * i + 1u, d);
        mu_assert("imported", ref && dist);
        mu_assert("submit", vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK);
        canary(ring, 2u * i + 2u);
    }
    return NULL;
}

/* motion_v2's SAD of every frame, after the flush. */
static char *read_sad(VmafxContext *context, double sad[N_FRAMES])
{
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    for (unsigned i = 0; i < N_FRAMES; i++) {
        VmafxScore s = VMAFX_SCORE_INIT;
        mu_assert("sad", vmafx_feature_score(context, MOTION_SAD, i, &s, NULL) == VMAFX_OK);
        sad[i] = s.value;
    }
    return NULL;
}

/* Every frame was released by the time its contexts are gone. */
static char *all_released(Ring *ring)
{
    bool released = true;
    for (unsigned j = 0; j < 2u * N_FRAMES; j++) {
        released = released && vmafx_fence_wait(&ring->released[j], 0u, NULL) == VMAFX_OK;
        (void)vmafx_fence_destroy(&ring->released[j], NULL);
    }
    mu_assert("every frame released after the last reader", released);
    return NULL;
}

/* Score N_FRAMES imported frames with canaries; `sad` receives motion_v2's
 * SAD of every frame. */
static char *run_with_canaries(bool early, double sad[N_FRAMES])
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    Ring ring;
    for (unsigned j = 0; j < 2u * N_FRAMES; j++) {
        ring.released[j] = (VmafxFence)VMAFX_FENCE_INIT;
        ring.overwritten[j] = false;
    }
    memcpy(ring_buf, source, sizeof(ring_buf));
    VmafxContext *const context = feature_context("motion_v2", "psnr");
    mu_assert("context", context != NULL);
    vmafx_test_set_switches(early ? VMAFX_TEST_EARLY_RELEASE : 0u);
    char *msg = canary_submit(context, &ring, &d);
    vmafx_test_set_switches(0u);
    msg = msg ? msg : read_sad(context, sad);
    const bool destroyed = vmafx_context_destroy(context, NULL) == VMAFX_OK;
    char *const released = all_released(&ring);
    mu_assert_msg(msg);
    mu_assert("destroy", destroyed);
    return released;
}

/* motion_v2's SAD of the same frames created on the host. */
static char *clean_sad(double sad[N_FRAMES])
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VmafxContext *const context = feature_context("motion_v2", "psnr");
    mu_assert("context", context != NULL);
    for (unsigned i = 0; i < N_FRAMES; i++) {
        mu_assert("host", vmafx_submit(context, vt_wrap_frame(&d, source[(size_t)2u * i], NULL),
                                       vt_wrap_frame(&d, source[(size_t)2u * i + 1u], NULL), i,
                                       NULL) == VMAFX_OK);
    }
    char *const msg = read_sad(context, sad);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return msg;
}

static bool same_sad(const double a[N_FRAMES], const double b[N_FRAMES])
{
    bool same = true;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        same = same && vt_same_bits(a[i], b[i]);
    }
    return same;
}

static char *test_release_canary(void)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    for (unsigned j = 0; j < 2u * N_FRAMES; j++) {
        vt_fill(&d, source[j], j);
    }
    double clean[N_FRAMES];
    double fenced[N_FRAMES];
    double early[N_FRAMES];
    double threaded[N_FRAMES];
    mu_assert_msg(clean_sad(clean));
    mu_assert_msg(run_with_canaries(false, fenced));
    mu_assert_msg(run_with_canaries(true, early));
    mu_assert("canaries only in released memory", same_sad(clean, fenced));
    mu_assert("release signalled early (planted): a canary in a frame still read",
              !same_sad(clean, early));
    /* With worker threads the last reader drops the frame on a worker: the
     * fence is signalled there, and still only after the read. */
    canary_threads = 3u;
    char *const msg = run_with_canaries(false, threaded);
    canary_threads = 0u;
    mu_assert_msg(msg);
    mu_assert("worker threads: canaries only in released memory", same_sad(clean, threaded));
    return NULL;
}

/* ---- Host copies ------------------------------------------------------------------------------- */

static char *test_host_copy_counter(void)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 10, W, H);
    static uint8_t data[2u * FRAME_BYTES];
    const size_t bytes = vt_frame_bytes(&d);
    vt_fill(&d, data, 3u);
    const VmafxFrameImport imp = vt_import_planar(&d, data);
    VmafxFrame *frame = NULL;
    vmafx_test_reset_counters();
    mu_assert("bound", vmafx_frame_import(NULL, &imp, &frame, NULL) == VMAFX_OK &&
                           vmafx_test_host_copies() == 0u && vmafx_test_conversions() == 0u);
    vmafx_frame_unref(frame);
    vmafx_test_set_switches(VMAFX_TEST_FORCE_HOST_COPY);
    const VmafxStatus copied = vmafx_frame_import(NULL, &imp, &frame, NULL);
    vmafx_test_set_switches(0u);
    mu_assert("copy planted: the counter sees it", copied == VMAFX_OK &&
                                                       vmafx_test_host_copies() == 1u &&
                                                       vmafx_test_host_copy_bytes() >= bytes);
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    mu_assert("copied", vmafx_frame_planes(frame, &planes, NULL) == VMAFX_OK &&
                            planes.data[0] != (void *)data &&
                            !memcmp(planes.data[0], data, (size_t)W * 2u));
    vmafx_frame_unref(frame);
    vmafx_test_reset_counters();
    return NULL;
}

/* ---- The import rule (D8) ------------------------------------------------------------------------ */

/* Import through the rule on a psnr context; `*attempts` counts the
 * imports it made. */
static VmafxStatus import_by_rule(const VmafxFrameImport *imp, VmafxError **error,
                                  uint64_t *attempts)
{
    VmafxContext *const context = feature_context("psnr", NULL);
    VmafxFrame *frame = NULL;
    const uint64_t before = vmafx_test_import_attempts();
    const VmafxStatus status =
        vmafx_context_import_frame(context, NULL, imp, "main", &frame, error);
    *attempts = vmafx_test_import_attempts() - before;
    vmafx_frame_unref(frame);
    (void)vmafx_context_destroy(context, NULL);
    return status;
}

static char *test_import_rule(void)
{
    static uint8_t data[W * H * 3 / 2];
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VmafxFrameImport imp = vt_import_semiplanar(&d, VMAFX_PIXEL_FORMAT_NV12, 8u, data);
    VmafxError *error = NULL;
    uint64_t attempts = 0;
    vmafx_test_fail_imports(VMAFX_E_BUSY, 1u);
    mu_assert("one transient failure: retried",
              import_by_rule(&imp, NULL, &attempts) == VMAFX_OK && attempts == 2u);
    vmafx_test_fail_imports(VMAFX_E_TIMEOUT, 1u);
    mu_assert("a timed-out acquire: retried",
              import_by_rule(&imp, NULL, &attempts) == VMAFX_OK && attempts == 2u);
    vmafx_test_fail_imports(VMAFX_E_BUSY, 2u);
    mu_assert("two: fail",
              import_by_rule(&imp, &error, &attempts) == VMAFX_E_BUSY && attempts == 2u && error);
    const char *msg = vmafx_error_message(error);
    mu_assert("named", contains(msg, "main: backend cpu device 0, memory HOST, nv12 8-bit") &&
                           contains(msg, "modifiers 0x0 0x0") && contains(msg, "2 attempts") &&
                           contains(msg, "no host copy"));
    vmafx_error_free(error);
    error = NULL;
    vmafx_test_fail_imports(VMAFX_OK, 0u);
    imp.plane[1].modifier = 0x0100000000000002ull;
    mu_assert("not transient: no retry",
              import_by_rule(&imp, &error, &attempts) == VMAFX_E_NOTSUP && attempts == 1u &&
                  contains(vmafx_error_message(error), "1 attempt;") &&
                  contains(vmafx_error_message(error), "0x100000000000002") &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.plane[1].modifier", VMAFX_SUBJECT_PLANE));
    return NULL;
}

/* ---- The import rule's wait (context option) ------------------------------------------- */

static uint64_t real_ns(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

/* What one run of the import rule on a never-signalled acquire fence took. */
typedef struct WaitRun {
    VmafxStatus status;
    uint64_t attempts;
    uint64_t allowed; /* the wait the rule allowed itself (hook) */
    uint64_t virtual; /* virtual time it waited */
    uint64_t real;    /* wall time it took */
} WaitRun;

/* Run the import rule with `import_retry_wait_ns` = `option` on the
 * virtual test clock, against an acquire fence nobody signals. */
static bool run_wait(uint64_t option, WaitRun *run)
{
    static uint8_t data[FRAME_BYTES];
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.import_retry_wait_ns = option;
    VmafxContext *context = NULL;
    VmafxFrameImport imp = vt_import_planar(&d, data);
    if (vmafx_context_create(&config, &context, NULL) != VMAFX_OK ||
        vmafx_fence_create(NULL, VMAFX_FENCE_HOST, &imp.acquire, NULL) != VMAFX_OK) {
        return false;
    }
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    const uint64_t attempts = vmafx_test_import_attempts();
    const uint64_t virtual = vmafx_test_clock_now_ns();
    const uint64_t real = real_ns();
    run->status = vmafx_context_import_frame(context, NULL, &imp, "main", &frame, &error);
    run->real = real_ns() - real;
    run->virtual = vmafx_test_clock_now_ns() - virtual;
    run->attempts = vmafx_test_import_attempts() - attempts;
    run->allowed = vmafx_test_last_retry_wait_ns();
    vmafx_error_free(error);
    return vmafx_fence_destroy(&imp.acquire, NULL) == VMAFX_OK &&
           vmafx_context_destroy(context, NULL) == VMAFX_OK;
}

/* The rule waited `expected` (the poll rounds stop within one step after
 * it), retried once and failed. */
static bool waited(const WaitRun *run, uint64_t expected)
{
    return run->status == VMAFX_E_BUSY && run->attempts == 2u && run->allowed == expected &&
           run->virtual >= expected && run->virtual <= expected + VMAFX_FENCE_POLL_NS;
}

static char *test_import_rule_wait_option(void)
{
    const uint64_t default_ns = 10000000000ull;
    WaitRun runs[3];
    vmafx_test_set_virtual_clock(true);
    const bool ran =
        run_wait(0u, &runs[0]) && run_wait(20000000u, &runs[1]) && run_wait(1u, &runs[2]);
    vmafx_test_set_virtual_clock(false);
    mu_assert("runs", ran);
    mu_assert("0: the default holds 10 s", waited(&runs[0], default_ns));
    mu_assert("20 ms option: fails after 20 ms", waited(&runs[1], 20000000u));
    mu_assert("shorter fails sooner", runs[1].virtual < runs[0].virtual);
    mu_assert("1 ns (the smallest): one poll step", waited(&runs[2], 1u));
    mu_assert("the test clock, not a real 10 s sleep", runs[0].real < 5000000000ull);
    return NULL;
}

/* ---- Admission ------------------------------------------------------------------------------------ */

static char *test_admission_names_extractors(void)
{
    static uint8_t data[W * H * 3 / 2];
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    const VmafxFrameImport imp = vt_import_planar(&d, data);
    VmafxContext *const context = feature_context("psnr", "float_ssim");
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    mu_assert("context", context != NULL);
    vmafx_test_set_import_residency(VMAFX_BACKEND_CUDA);
    const VmafxStatus status =
        vmafx_context_import_frame(context, NULL, &imp, "reference", &frame, &error);
    vmafx_test_set_import_residency(VMAFX_TEST_RESIDENCY_OFF);
    const char *msg = vmafx_error_message(error);
    mu_assert("refused, every extractor named",
              status == VMAFX_E_NOTSUP && frame == NULL &&
                  contains(msg, "refused by 2 extractors") && contains(msg, "psnr (cpu") &&
                  contains(msg, "float_ssim (cpu") && contains(msg, "host copy of device memory") &&
                  contains(msg, "reference: backend cpu") &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "psnr", VMAFX_SUBJECT_EXTRACTOR));
    /* The same frame on the host is admitted everywhere. */
    mu_assert("host frame", vmafx_context_import_frame(context, NULL, &imp, "reference", &frame,
                                                       NULL) == VMAFX_OK);
    vmafx_frame_unref(frame);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* A frame in device memory of a device the context does not score on. */
static char *test_admission_device(void)
{
    static uint8_t data[W * H * 3 / 2];
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    const VmafxFrameImport imp = vt_import_planar(&d, data);
    VmafxContext *bare = NULL;
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    mu_assert("context", vmafx_context_create(NULL, &bare, NULL) == VMAFX_OK);
    vmafx_test_set_import_residency(VMAFX_BACKEND_SYCL);
    const VmafxStatus imported = vmafx_frame_import(NULL, &imp, &frame, NULL);
    vmafx_test_set_import_residency(VMAFX_TEST_RESIDENCY_OFF);
    mu_assert("imported", imported == VMAFX_OK);
    mu_assert("other device", vmafx_context_admit(bare, frame, &error) == VMAFX_E_NOTSUP &&
                                  vt_failed(&error, VMAFX_E_NOTSUP, "frame", VMAFX_SUBJECT_DEVICE));
    VmafxFrame *host = NULL;
    mu_assert("host", vmafx_frame_import(NULL, &imp, &host, NULL) == VMAFX_OK);
    mu_assert("mixed inputs",
              vmafx_submit(bare, host, frame, 0, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "distorted", VMAFX_SUBJECT_FRAME));
    mu_assert("destroy", vmafx_context_destroy(bare, NULL) == VMAFX_OK);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_acquire_ordering),        MU_TEST(test_release_canary),
        MU_TEST(test_host_copy_counter),       MU_TEST(test_import_rule),
        MU_TEST(test_import_rule_wait_option), MU_TEST(test_admission_names_extractors),
        MU_TEST(test_admission_device),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
