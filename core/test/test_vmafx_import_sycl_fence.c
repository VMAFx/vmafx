/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Fence ordering of the VMAFx SYCL lane under concurrent device load (RC4
 * WP3, ADR-1929, ADR-2091), modelled on the ADR-1199 harness
 * (scripts/test/repro-cuda-ffmpeg-nondeterminism.sh: a producer the library
 * cannot see, another queue keeping the device busy):
 *
 * - Acquire: the producer writes every frame on its own queue behind a host
 *   task that holds the queue for a few milliseconds (so its copy reaches the
 *   device after the readers when they are not ordered), and hands the copy's
 *   event over as the SYCL_EVENT acquire fence; the import is made at once.
 *   With the frame's ready event (the barrier on the library queue every
 *   reader waits on) every frame scores as the host frame (0 bad of N); with
 *   the planted skipped wait (VMAFX_TEST_SKIP_ACQUIRE_WAIT) the readers copy
 *   the planes before the producer wrote them and frames score wrong.
 * - Release: a second producer queue holds the frame's acquire event with a
 *   kernel spinning on the compute engine (a host-task hold would make the
 *   submit wait on the host: the SYCL scheduler resolves a dependency on a
 *   host task on the submitting thread), so the readers queue up behind it.
 *   The frame's release callback waits for the SYCL_EVENT release fence and
 *   writes a canary into the frame (shared USM) from the host: a device-side
 *   canary cannot pass the readers on this GPU, whose compute engine runs the
 *   commands of every queue in submission order. With the real release the
 *   canary lands after the readers; with the planted early release
 *   (VMAFX_TEST_EARLY_RELEASE: the event opened and the callback run when the
 *   submit returns) it lands while the readers still wait, and they see it.
 * - A HOST release fence is pending while the device still reads the frame.
 * - Pool frames (written and finished by the caller before the submit) score
 *   as host frames under load, cycling through a pool of 3.
 * - The import rule on SYCL: an unsignalled HOST acquire fence is waited on by
 *   vmafx_context_import_frame() and the import retried once.
 * - SYCL_EVENT fences: created, stored into, polled, waited on, destroyed.
 *
 * Needs a Level Zero GPU (77 without one).
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_sycl_cells.h"
#include "vmafx_sycl_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 1920u
#define H 1080u
#define N_FRAMES 16u
/* How long a host task holds a producer queue (microseconds). */
#define HOLD_US 6000u
/* The release tests' hold: a kernel spinning this many microseconds on the
 * compute engine, long enough that an early canary, a copy on the copy
 * engine, lands first. */
#define RELEASE_HOLD_US 30000u
/* Busy kernels the load keeps queued. */
#define LOAD_IN_FLIGHT 4u

/* The psnr and adm cells of vmafx_sycl_cells.h. */
#define CELL_PSNR (&vs_cells[17])
#define CELL_ADM (&vs_cells[0])

static VsGpu gpu;
static bool have_gpu;

/* ---- Frames ------------------------------------------------------------------------- */

typedef struct Clip {
    VmafxFrameDesc desc;
    uint8_t *ref;
    uint8_t *dist;
    VsPlanes src[2u * N_FRAMES]; /* the frames, uploaded once (the producer's source) */
    size_t bytes;                /* bytes one frame's planes span */
} Clip;

static bool clip_open(Clip *c)
{
    memset(c, 0, sizeof(*c));
    c->desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    const size_t frame = vt_frame_bytes(&c->desc);
    c->ref = malloc(frame * N_FRAMES);
    c->dist = malloc(frame * N_FRAMES);
    bool ok = c->ref && c->dist;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        vt_fill(&c->desc, c->ref + i * frame, i);
        vt_fill(&c->desc, c->dist + i * frame, 3u * i + 1u);
        ok = vs_upload(&gpu, &c->desc, c->ref + i * frame, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, 0u,
                       &c->src[(size_t)2u * i]) &&
             vs_upload(&gpu, &c->desc, c->dist + i * frame, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, 0u,
                       &c->src[(size_t)2u * i + 1u]);
    }
    if (ok) {
        const VsPlanes *const p = &c->src[0];
        c->bytes = (size_t)(p->offset[2] + p->pitch[2] * ((H + 1u) / 2u));
    }
    return ok;
}

static void clip_close(Clip *c)
{
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vs_free_planes(&gpu, &c->src[k]);
    }
    free(c->ref);
    free(c->dist);
}

/* A frame buffer of the producer: zeroed and finished, then written on the
 * producer's queue from `src` behind a host task that holds the queue; the
 * copy's event is the acquire fence (`*event`, freed by the caller). */
static bool produce(const Clip *c, const VsPlanes *src, VsPlanes *dst, uintptr_t *event)
{
    *dst = *src;
    dst->base = vs_alloc(gpu.producer, c->bytes);
    bool ok = dst->base && vs_fill(gpu.producer, dst->base, 0u, c->bytes) == 0 &&
              vs_finish(gpu.producer) == 0 && vs_hold(gpu.producer, HOLD_US) == 0 &&
              vs_copy_2d(gpu.producer, dst->base, c->bytes, src->base, c->bytes, c->bytes, 1u) == 0;
    *event = ok ? vs_last_event(gpu.producer) : 0u;
    return ok && *event != 0u;
}

/* ---- Sessions ------------------------------------------------------------------------- */

static VmafxContext *run_host(const Clip *c, const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    const size_t frame = vt_frame_bytes(&c->desc);
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        VmafxFrame *ref = vt_wrap_frame(&c->desc, c->ref + i * frame, NULL);
        VmafxFrame *dist = vt_wrap_frame(&c->desc, c->dist + i * frame, NULL);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* Frames of `other` whose `feature` differs from `host`'s. */
static unsigned bad_frames(VmafxContext *host, VmafxContext *other, const char *feature)
{
    unsigned bad = 0;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        VmafxScore a = VMAFX_SCORE_INIT;
        VmafxScore b = VMAFX_SCORE_INIT;
        const bool ok = vmafx_feature_score(host, feature, i, &a, NULL) == VMAFX_OK &&
                        vmafx_feature_score(other, feature, i, &b, NULL) == VMAFX_OK;
        bad += !ok || !vt_same_bits(a.value, b.value);
    }
    return bad;
}

static void destroy(VmafxContext *context)
{
    if (context) {
        (void)vmafx_context_destroy(context, NULL);
    }
}

/* ---- Acquire ordering ---------------------------------------------------------------- */

/* The fenced session: every frame produced and imported at once. */
static VmafxContext *run_fenced(const Clip *c, const VcCell *cell, VsPlanes *bufs,
                                uintptr_t *events)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        for (unsigned s = 0; s < 2u && ok; s++) {
            const unsigned k = 2u * i + s;
            ok = produce(c, &c->src[k], &bufs[k], &events[k]);
            VmafxFrameImport imp =
                vs_import_desc(&c->desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &bufs[k]);
            imp.acquire = vs_event_fence(events[k]);
            ok = ok && vmafx_frame_import(gpu.device, &imp, &pair[s], NULL) == VMAFX_OK;
        }
        ok = ok && vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK;
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    return ok ? context : NULL;
}

static void free_run(VsPlanes *bufs, const uintptr_t *events)
{
    (void)vs_finish(gpu.producer);
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vs_free_planes(&gpu, &bufs[k]);
        if (events[k]) {
            vs_event_free(events[k]);
        }
    }
}

/* One arm: bad frames of `feature` under load, with the planted switches. */
static int acquire_arm(const Clip *c, const VcCell *cell, const char *feature, uint32_t switches)
{
    VsPlanes bufs[2u * N_FRAMES];
    uintptr_t events[2u * N_FRAMES];
    memset(bufs, 0, sizeof(bufs));
    memset(events, 0, sizeof(events));
    VsLoad *const load = vs_load_start(LOAD_IN_FLIGHT);
    vmafx_test_set_switches(switches);
    VmafxContext *const fenced = load ? run_fenced(c, cell, bufs, events) : NULL;
    vmafx_test_set_switches(0u);
    vs_load_stop(load);
    VmafxContext *const host = fenced ? run_host(c, cell) : NULL;
    const int bad = fenced && host ? (int)bad_frames(host, fenced, feature) : -1;
    destroy(fenced);
    destroy(host);
    free_run(bufs, events);
    return bad;
}

static char *test_acquire_order_under_load(void)
{
    Clip c;
    if (!clip_open(&c)) {
        clip_close(&c);
        return "clip";
    }
    const VcCell *const cells[] = {CELL_PSNR, CELL_ADM};
    static const char *const features[] = {"psnr_y", "VMAF_integer_feature_adm2_score"};
    char *msg = NULL;
    for (unsigned k = 0; k < 2u && !msg; k++) {
        const int waited = acquire_arm(&c, cells[k], features[k], 0u);
        const int skipped = acquire_arm(&c, cells[k], features[k], VMAFX_TEST_SKIP_ACQUIRE_WAIT);
        (void)fprintf(stderr, "[%s: %d bad of %u with the wait, %d without] ", cells[k]->name,
                      waited, N_FRAMES, skipped);
        msg = waited != 0  ? "a fenced frame scored wrong" :
              skipped <= 0 ? "the planted skipped wait went unseen" :
                             NULL;
    }
    clip_close(&c);
    return msg;
}

/* ---- Release ordering ------------------------------------------------------------------- */

typedef struct ReleaseRun ReleaseRun;

/* What the release callback of one distorted frame needs. */
typedef struct Pending {
    ReleaseRun *run;
    unsigned i;
    VmafxFence release; /* its SYCL_EVENT release fence */
} Pending;

struct ReleaseRun {
    VsProducer *holder; /* holds the frames' acquire events */
    VsPlanes bufs[2u * N_FRAMES];
    uintptr_t held[N_FRAMES]; /* each frame's acquire event */
    Pending pending[N_FRAMES];
    unsigned callbacks;      /* release callbacks run */
    unsigned early_canaries; /* canaries written while their frame was not yet read */
};

/* The frame's release callback (VmafxFrameImport.release): the producer
 * waits for the SYCL_EVENT release fence, then writes a canary over the
 * frame's luma, which lives in shared USM, from the host. A canary written
 * while the frame's acquire event is still held was written before the
 * readers ran. */
static void on_release(void *user)
{
    Pending *const p = user;
    ReleaseRun *const r = p->run;
    const VsPlanes *const buf = &r->bufs[2u * p->i + 1u];
    r->callbacks++;
    if (vmafx_fence_wait(&p->release, 10000000000ull, NULL) != VMAFX_OK) {
        return;
    }
    memset(buf->base + buf->offset[0], 0xff, (size_t)buf->pitch[0] * H);
    r->early_canaries += vs_event_done(r->held[p->i]) == 0;
}

/* Frame i (uploaded before the run), imported behind the holder's held
 * event (the distorted frame with the release callback and a SYCL_EVENT
 * release fence), submitted. */
static bool release_frame(ReleaseRun *r, const Clip *c, VmafxContext *context, unsigned i)
{
    VmafxFrame *pair[2] = {NULL, NULL};
    Pending *const p = &r->pending[i];
    p->run = r;
    p->i = i;
    p->release = (VmafxFence)VMAFX_FENCE_INIT;
    bool ok = vs_hold_device(r->holder, RELEASE_HOLD_US) == 0;
    r->held[i] = ok ? vs_last_event(r->holder) : 0u;
    for (unsigned s = 0; s < 2u && ok; s++) {
        const unsigned k = 2u * i + s;
        VmafxFrameImport imp =
            vs_import_desc(&c->desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &r->bufs[k]);
        imp.acquire = vs_event_fence(r->held[i]);
        imp.release = s ? on_release : NULL;
        imp.user = s ? p : NULL;
        ok = ok && vmafx_frame_import(gpu.device, &imp, &pair[s], NULL) == VMAFX_OK;
    }
    ok = ok &&
         vmafx_frame_release_fence(pair[1], VMAFX_FENCE_SYCL_EVENT, &p->release, NULL) == VMAFX_OK;
    return ok && vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK;
}

/* Every frame of the clip into shared USM (the producer writes the canary
 * from the host). */
static bool release_upload(ReleaseRun *r, const Clip *c)
{
    bool ok = true;
    for (unsigned k = 0; k < 2u * N_FRAMES && ok; k++) {
        const uint8_t *const src =
            (k & 1u ? c->dist : c->ref) + (k / 2u) * vt_frame_bytes(&c->desc);
        size_t rows[3];
        size_t row_bytes[3];
        vs_layout(&c->desc, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &r->bufs[k], rows, row_bytes);
        r->bufs[k].base = vs_alloc_shared(gpu.producer, vs_span(&r->bufs[k], rows));
        ok = r->bufs[k].base && vs_write(&gpu, &c->desc, src, VMAFX_PIXEL_FORMAT_YUV420P, 0u,
                                         &r->bufs[k], rows, row_bytes);
    }
    return ok;
}

static VmafxContext *run_release(ReleaseRun *r, const Clip *c)
{
    VmafxContext *const context = vc_cell_context(gpu.device, CELL_PSNR);
    bool ok = context != NULL && release_upload(r, c);
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = release_frame(r, c, context, i);
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    return ok ? context : NULL;
}

static void release_run_free(ReleaseRun *r)
{
    (void)vs_finish(gpu.producer);
    (void)vs_finish(r->holder);
    for (unsigned i = 0; i < N_FRAMES; i++) {
        (void)vmafx_fence_destroy(&r->pending[i].release, NULL);
        if (r->held[i]) {
            vs_event_free(r->held[i]);
        }
    }
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vs_free_planes(&gpu, &r->bufs[k]);
    }
    vs_producer_close(r->holder);
}

/* One arm: bad frames of psnr_y, and canaries that landed before their
 * frame was read. */
static int release_arm(const Clip *c, uint32_t switches, unsigned *early)
{
    static ReleaseRun r;
    memset(&r, 0, sizeof(r));
    r.holder = vs_producer_open();
    vmafx_test_set_switches(switches);
    VmafxContext *const imported = r.holder ? run_release(&r, c) : NULL;
    vmafx_test_set_switches(0u);
    VmafxContext *const host = imported ? run_host(c, CELL_PSNR) : NULL;
    const int bad = imported && host ? (int)bad_frames(host, imported, "psnr_y") : -1;
    *early = r.early_canaries;
    if (r.callbacks != N_FRAMES) {
        (void)fprintf(stderr, "[%u release callbacks of %u] ", r.callbacks, N_FRAMES);
    }
    destroy(imported);
    destroy(host);
    release_run_free(&r);
    return bad;
}

static char *test_release_canary(void)
{
    Clip c;
    if (!clip_open(&c)) {
        clip_close(&c);
        return "clip";
    }
    unsigned early_real = 0;
    unsigned early_planted = 0;
    const int real = release_arm(&c, 0u, &early_real);
    const int planted = release_arm(&c, VMAFX_TEST_EARLY_RELEASE, &early_planted);
    clip_close(&c);
    (void)fprintf(stderr,
                  "[release: %d bad of %u, %u early canaries; planted early release: %d bad, "
                  "%u early canaries] ",
                  real, N_FRAMES, early_real, planted, early_planted);
    mu_assert("released after the readers", real == 0 && early_real == 0u);
    mu_assert("the planted early release was seen", planted > 0 || early_planted > 0u);
    return NULL;
}

/* A frame's HOST release fence is signalled when the device has run its
 * readers, not when its last reference is dropped: with the readers held
 * behind the acquire event, the fence is pending right after the release and
 * signalled once the hold ends. */
static char *test_host_release_after_device(void)
{
    VsProducer *const holder = vs_producer_open();
    VsPlanes p;
    memset(&p, 0, sizeof(p));
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    uint8_t *const data = malloc(vt_frame_bytes(&d));
    bool ok = holder && data;
    if (ok) {
        vt_fill(&d, data, 3u);
        ok = vs_upload(&gpu, &d, data, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, 0u, &p);
    }
    const uintptr_t held =
        ok && vs_hold_device(holder, RELEASE_HOLD_US) == 0 ? vs_last_event(holder) : 0u;
    VmafxFrameImport imp = vs_import_desc(&d, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    imp.acquire = vs_event_fence(held);
    VmafxFrame *frame = NULL;
    VmafxFence fence = VMAFX_FENCE_INIT;
    ok = ok && held && vmafx_frame_import(gpu.device, &imp, &frame, NULL) == VMAFX_OK &&
         vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_OK;
    vmafx_frame_unref(frame); /* the last reference: the release is enqueued */
    const VmafxStatus at_release = ok ? vmafx_fence_wait(&fence, 0u, NULL) : VMAFX_E_INVALID;
    const VmafxStatus later = ok ? vmafx_fence_wait(&fence, 5000000000ull, NULL) : VMAFX_E_INVALID;
    (void)vmafx_fence_destroy(&fence, NULL);
    (void)vs_finish(holder);
    if (held) {
        vs_event_free(held);
    }
    vs_free_planes(&gpu, &p);
    free(data);
    vs_producer_close(holder);
    mu_assert("set up", ok);
    mu_assert("pending while the device still holds the frame", at_release == VMAFX_PENDING);
    mu_assert("signalled after the device ran on", later == VMAFX_OK);
    return NULL;
}

/* ---- Pool frames ------------------------------------------------------------------------ */

/* The caller writes a pool frame on its queue from the uploaded source `src`
 * and finishes the write before the submit (a SYCL pool frame carries no
 * acquire fence, ADR-2091). */
static bool produce_into_pool(const Clip *c, const VsPlanes *src, VmafxFrame *frame)
{
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(&c->desc, w, h, row);
    bool ok = vmafx_frame_planes(frame, &planes, NULL) == VMAFX_OK;
    for (uint32_t i = 0; i < planes.n_planes && i < 3u && ok; i++) {
        ok = vs_copy_2d(gpu.producer, planes.data[i], (size_t)planes.stride[i],
                        src->base + src->offset[i], (size_t)src->pitch[i], row[i], h[i]) == 0;
    }
    return ok && vs_finish(gpu.producer) == 0;
}

static VmafxContext *run_pool(const Clip *c, VmafxFramePool *pool, const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        for (unsigned s = 0; s < 2u && ok; s++) {
            ok = vmafx_frame_pool_acquire(pool, &pair[s], NULL) == VMAFX_OK &&
                 produce_into_pool(c, &c->src[(size_t)2u * i + s], pair[s]);
        }
        ok = ok && vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK;
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    return ok ? context : NULL;
}

/* Pool frames handed out again only after their previous readers ran: a
 * pool of 4 cycled through 32 frames under load scores as the host frames. */
static char *test_pool_frames(void)
{
    Clip c;
    if (!clip_open(&c)) {
        clip_close(&c);
        return "clip";
    }
    VmafxFramePool *pool = NULL;
    const bool made = vmafx_frame_pool_create(gpu.device, &c.desc, 4u, &pool, NULL) == VMAFX_OK;
    VsLoad *const load = made ? vs_load_start(LOAD_IN_FLIGHT) : NULL;
    VmafxContext *const pooled = load ? run_pool(&c, pool, CELL_PSNR) : NULL;
    vs_load_stop(load);
    VmafxContext *const host = pooled ? run_host(&c, CELL_PSNR) : NULL;
    const int bad = pooled && host ? (int)bad_frames(host, pooled, "psnr_y") : -1;
    destroy(pooled);
    destroy(host);
    vmafx_frame_pool_destroy(pool);
    clip_close(&c);
    (void)fprintf(stderr, "[pool frames: %d bad of %u] ", bad, N_FRAMES);
    mu_assert("pool frames score as host frames", bad == 0);
    return NULL;
}

/* ---- The import rule and SYCL_EVENT fences ------------------------------------------- */

static void *signal_later(void *arg)
{
    const struct timespec t = {.tv_sec = 0, .tv_nsec = 10000000L};
    (void)nanosleep(&t, NULL);
    (void)vmafx_fence_signal(arg, NULL);
    return NULL;
}

static char *test_import_rule_host_acquire(void)
{
    Clip c;
    if (!clip_open(&c)) {
        clip_close(&c);
        return "clip";
    }
    VmafxContext *const context = vc_cell_context(gpu.device, CELL_PSNR);
    VmafxFrameImport imp = vs_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &c.src[0]);
    const bool fenced =
        vmafx_fence_create(gpu.device, VMAFX_FENCE_HOST, &imp.acquire, NULL) == VMAFX_OK;
    pthread_t producer;
    const bool started = fenced && pthread_create(&producer, NULL, signal_later, &imp.acquire) == 0;
    const uint64_t before = vmafx_test_import_attempts();
    VmafxFrame *frame = NULL;
    const VmafxStatus status =
        context && started ?
            vmafx_context_import_frame(context, gpu.device, &imp, "main", &frame, NULL) :
            VMAFX_E_INVALID;
    const uint64_t attempts = vmafx_test_import_attempts() - before;
    if (started) {
        (void)pthread_join(producer, NULL);
    }
    vmafx_frame_unref(frame);
    (void)vmafx_fence_destroy(&imp.acquire, NULL);
    destroy(context);
    clip_close(&c);
    mu_assert("retried once after the host wait", status == VMAFX_OK && attempts == 2u);
    return NULL;
}

/* What the library refuses about its SYCL_EVENT fences; destroys `fence`. */
static char *sycl_event_refusals(VmafxFence *fence)
{
    VmafxError *error = NULL;
    mu_assert("the host signals no event",
              vmafx_fence_signal(fence, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "fence.kind", VMAFX_SUBJECT_FENCE));
    mu_assert("destroy", vmafx_fence_destroy(fence, NULL) == VMAFX_OK);
    mu_assert("a destroyed fence is not the library's",
              vmafx_fence_destroy(fence, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "fence.handle", VMAFX_SUBJECT_FENCE));
    mu_assert("no GL sync from the library",
              vmafx_fence_create(gpu.device, VMAFX_FENCE_GL_SYNC, fence, &error) ==
                      VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "kind", VMAFX_SUBJECT_FENCE));
    return NULL;
}

/* A SYCL_EVENT fence of the library: complete when created, pending behind a
 * held queue once a command is stored in it, then complete. */
static char *test_sycl_event_fences(void)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("create",
              vmafx_fence_create(gpu.device, VMAFX_FENCE_SYCL_EVENT, &fence, NULL) == VMAFX_OK &&
                  fence.kind == VMAFX_FENCE_SYCL_EVENT && fence.handle != 0u);
    mu_assert("a created event is complete until a command is stored in it",
              vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_OK);
    const bool ok = vs_hold_device(gpu.producer, HOLD_US) == 0 &&
                    vs_store_last_event(gpu.producer, fence.handle) == 0;
    mu_assert("stored behind a held queue", ok);
    mu_assert("poll", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_PENDING);
    mu_assert("wait", vmafx_fence_wait(&fence, 5000000000ull, NULL) == VMAFX_OK);
    return sycl_event_refusals(&fence);
}

static char *guard(char *(*test)(void))
{
    if (!have_gpu) {
        mu_skipped = 1;
        return NULL;
    }
    return test();
}

#define GUARDED(name)                                                                              \
    static char *name##_g(void)                                                                    \
    {                                                                                              \
        return guard(name);                                                                        \
    }

GUARDED(test_acquire_order_under_load)
GUARDED(test_release_canary)
GUARDED(test_host_release_after_device)
GUARDED(test_pool_frames)
GUARDED(test_import_rule_host_acquire)
GUARDED(test_sycl_event_fences)

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vs_open_gpu(&gpu, false);
    static const MuTest tests[] = {
        MU_TEST(test_acquire_order_under_load_g),  MU_TEST(test_release_canary_g),
        MU_TEST(test_host_release_after_device_g), MU_TEST(test_pool_frames_g),
        MU_TEST(test_import_rule_host_acquire_g),  MU_TEST(test_sycl_event_fences_g),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    vs_close_gpu(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
