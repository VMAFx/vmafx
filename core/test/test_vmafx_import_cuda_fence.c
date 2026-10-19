/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Fence ordering of the VMAFx CUDA lane under concurrent device load (RC4
 * WP3, ADR-1929, ADR-2023), modelled on the ADR-1199 harness
 * (scripts/test/repro-cuda-ffmpeg-nondeterminism.sh: a producer the library
 * cannot see, another stream keeping the device busy):
 *
 * - Acquire: the producer writes every frame on its own stream behind a host
 *   function that holds that stream, then records the acquire event; the
 *   import is made at once. With the library's wait on its stream every frame
 *   scores as the host frame (0 bad of N; the hold lasts a few
 *   milliseconds). The planted skipped wait (VMAFX_TEST_SKIP_ACQUIRE_WAIT)
 *   runs with a late producer: from frame LATE_FIRST on, the hold before each
 *   distorted frame's write lasts until the library has run the frame's
 *   readers (its CUDA_EVENT release fence, which the test waits on), so the
 *   kernels read the zeroed planes and every such frame scores wrong whatever
 *   the device's timing. (A timed hold left the control to that timing:
 *   adm's readers sometimes ran after a 6 ms hold and the skipped wait went
 *   unseen.) The ADR-1199 barrier is not what saves the fenced frames: it is
 *   skipped for them, which the skipped-wait arm shows; anything that ordered
 *   the readers after the producer would hold them until the gate gives up,
 *   which the arm reports.
 * - Release: the library stream is held behind a host function while the
 *   frame's readers queue up; the frame's release callback makes the
 *   producer's stream wait on the CUDA_EVENT release fence and write a canary
 *   into the frame (no host wait). With the real release the canary lands
 *   after the readers (scores unchanged, no canary written before its frame
 *   was read); with the planted early release (VMAFX_TEST_EARLY_RELEASE: the
 *   event recorded and the callback run when the submit returns, not behind
 *   the readers) the readers see the canary.
 * - The import rule on CUDA: an unsignalled HOST acquire fence is waited on
 *   by vmafx_context_import_frame() and the import retried once.
 * - CUDA_EVENT fences: created on the device, polled, waited on, destroyed.
 *
 * Needs a CUDA device (77 without one).
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
#include "vmafx_device_cells.h"
#include "vmafx_cuda_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 1920u
#define H 1080u
#define N_FRAMES 16u
/* How long a host function holds the producer's or the library's stream. */
#define HOLD_MS 6u
/* Bytes the load stream keeps setting while a test runs. */
#define LOAD_BYTES (256u << 20)
/* The late producer: how long its gate holds at most, how many wake-ups its
 * wait takes before it gives up, and how long the test waits for a frame's
 * readers (longer than the gate, so a gate that gives up is what shows). */
#define GATE_TIMEOUT_S 2
#define GATE_WAKEUPS 1000u
#define READERS_TIMEOUT_NS 5000000000ull
/* The late producer holds the distorted frames from this one on. Frame 0 is
 * read after the extractors' first-frame setup (module loads, allocations),
 * which waits for the producer's stream (measured: its gate gave up every
 * time), so it keeps the timed hold and is not counted. */
#define LATE_FIRST 1u

static VcGpu gpu;
static bool have_gpu;

static void CUDAAPI hold_stream(void *arg)
{
    (void)arg;
    const struct timespec t = {.tv_sec = 0, .tv_nsec = (long)HOLD_MS * 1000000L};
    (void)nanosleep(&t, NULL);
}

/* The late producer's gate: the test opens it frame by frame. */
typedef struct Gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned opened;   /* frames below it may be written */
    unsigned timeouts; /* holds that gave up before their frame was opened */
} Gate;

typedef struct GateTicket {
    Gate *gate;
    unsigned frame;
} GateTicket;

/* Host function: hold the producer's stream until the ticket's frame is
 * opened, GATE_TIMEOUT_S at most. No CUDA call. */
static void CUDAAPI hold_until_open(void *arg)
{
    const GateTicket *const t = arg;
    Gate *const g = t->gate;
    struct timespec deadline = {0, 0};
    int err = clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += GATE_TIMEOUT_S;
    if (pthread_mutex_lock(&g->lock) != 0) {
        return;
    }
    for (unsigned n = 0; n < GATE_WAKEUPS && g->opened <= t->frame && err == 0; n++) {
        err = pthread_cond_timedwait(&g->cond, &g->lock, &deadline);
    }
    g->timeouts += g->opened <= t->frame;
    (void)pthread_mutex_unlock(&g->lock);
}

/* Let the producer write every frame up to `frame`. */
static void gate_open(Gate *g, unsigned frame)
{
    if (pthread_mutex_lock(&g->lock) == 0) {
        g->opened = frame + 1u > g->opened ? frame + 1u : g->opened;
        (void)pthread_cond_broadcast(&g->cond);
        (void)pthread_mutex_unlock(&g->lock);
    }
}

/* ---- Device load (the ADR-1199 harness's concurrent CUDA work) ---------------------- */

typedef struct Load {
    CUstream stream;
    CUdeviceptr buf;
    pthread_t thread;
    volatile int stop;
    bool running;
} Load;

static void *load_main(void *arg)
{
    Load *const load = arg;
    (void)vc_push(&gpu);
    for (unsigned i = 0; i < 100000u && !load->stop; i++) {
        (void)gpu.f->cuMemsetD8Async(load->buf, (unsigned char)i, LOAD_BYTES, load->stream);
        (void)gpu.f->cuStreamSynchronize(load->stream);
    }
    vc_pop(&gpu);
    return NULL;
}

static bool load_start(Load *load)
{
    memset(load, 0, sizeof(*load));
    bool ok = vc_push(&gpu) &&
              gpu.f->cuStreamCreate(&load->stream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS &&
              gpu.f->cuMemAlloc(&load->buf, LOAD_BYTES) == CUDA_SUCCESS;
    vc_pop(&gpu);
    load->running = ok && pthread_create(&load->thread, NULL, load_main, load) == 0;
    return load->running;
}

static void load_stop(Load *load)
{
    load->stop = 1;
    if (load->running) {
        (void)pthread_join(load->thread, NULL);
    }
    if (vc_push(&gpu)) {
        (void)gpu.f->cuStreamSynchronize(load->stream);
        (void)gpu.f->cuMemFree(load->buf);
        (void)gpu.f->cuStreamDestroy(load->stream);
        vc_pop(&gpu);
    }
}

/* ---- Frames ------------------------------------------------------------------------- */

typedef struct Clip {
    VmafxFrameDesc desc;
    uint8_t *ref;
    uint8_t *dist;
    VcPlanes src[2u * N_FRAMES]; /* the frames, uploaded once (the producer's source) */
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
        ok = vc_upload(&gpu, &c->desc, c->ref + i * frame, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u,
                       &c->src[(size_t)2u * i]) &&
             vc_upload(&gpu, &c->desc, c->dist + i * frame, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u,
                       &c->src[(size_t)2u * i + 1u]);
    }
    return ok && vc_push(&gpu) && gpu.f->cuStreamSynchronize(gpu.producer) == CUDA_SUCCESS &&
           (vc_pop(&gpu), true);
}

static void clip_close(Clip *c)
{
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vc_free(&gpu, &c->src[k]);
    }
    free(c->ref);
    free(c->dist);
}

/* A frame buffer of the producer: zeroed, then written on the producer's
 * stream from `src` behind a host function that holds the stream (HOLD_MS, or
 * until `gate` opens its frame), then the acquire event recorded. */
static bool produce(const Clip *c, const VcPlanes *src, VcPlanes *dst, CUevent *acquire,
                    GateTicket *gate)
{
    *dst = *src;
    const size_t bytes = (size_t)(src->offset[2] + src->pitch[2] * ((c->desc.h + 1u) / 2u));
    bool ok = vc_push(&gpu) && gpu.f->cuMemAlloc(&dst->base, bytes) == CUDA_SUCCESS &&
              gpu.f->cuMemsetD8Async(dst->base, 0, bytes, gpu.producer) == CUDA_SUCCESS &&
              gpu.f->cuStreamSynchronize(gpu.producer) == CUDA_SUCCESS &&
              gpu.f->cuLaunchHostFunc(gpu.producer, gate ? hold_until_open : hold_stream, gate) ==
                  CUDA_SUCCESS &&
              gpu.f->cuMemcpyDtoDAsync(dst->base, src->base, bytes, gpu.producer) == CUDA_SUCCESS &&
              gpu.f->cuEventCreate(acquire, CU_EVENT_DISABLE_TIMING) == CUDA_SUCCESS &&
              gpu.f->cuEventRecord(*acquire, gpu.producer) == CUDA_SUCCESS;
    vc_pop(&gpu);
    return ok;
}

/* ---- Acquire ordering ---------------------------------------------------------------- */

/* One fenced session's producer. */
typedef struct FencedRun {
    VcPlanes bufs[2u * N_FRAMES];
    CUevent events[2u * N_FRAMES];
    bool late;                     /* the late producer (the planted skipped wait's arm) */
    Gate gate;                     /* late: the holds before the distorted frames' writes */
    GateTicket tickets[N_FRAMES];  /* late: one per distorted frame */
    VmafxFence released[N_FRAMES]; /* late: the distorted frames' release fences */
    unsigned unread;               /* late: frames whose readers had not run in time */
} FencedRun;

static bool fenced_run_init(FencedRun *r, bool late)
{
    memset(r, 0, sizeof(*r));
    r->late = late;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        r->released[i] = (VmafxFence)VMAFX_FENCE_INIT;
        r->tickets[i] = (GateTicket){.gate = &r->gate, .frame = i};
    }
    if (pthread_mutex_init(&r->gate.lock, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&r->gate.cond, NULL) != 0) {
        (void)pthread_mutex_destroy(&r->gate.lock);
        return false;
    }
    return true;
}

/* Frame i produced, imported at once and submitted. The late producer opens
 * the distorted frame's gate once the library has run the frame's readers,
 * and on every other path, so its stream never waits on a failed frame. */
static bool fenced_frame(FencedRun *r, const Clip *c, VmafxContext *context, unsigned i)
{
    VmafxFrame *pair[2] = {NULL, NULL};
    bool ok = true;
    const bool gated = r->late && i >= LATE_FIRST;
    for (unsigned s = 0; s < 2u && ok; s++) {
        const unsigned k = 2u * i + s;
        GateTicket *const gate = gated && s ? &r->tickets[i] : NULL;
        ok = produce(c, &c->src[k], &r->bufs[k], &r->events[k], gate);
        VmafxFrameImport imp =
            vc_import_desc(&c->desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &r->bufs[k]);
        imp.acquire.kind = VMAFX_FENCE_CUDA_EVENT;
        imp.acquire.handle = (uintptr_t)r->events[k];
        ok = ok && vmafx_frame_import(gpu.device, &imp, &pair[s], NULL) == VMAFX_OK;
    }
    ok = ok && (!gated || vmafx_frame_release_fence(pair[1], VMAFX_FENCE_CUDA_EVENT,
                                                    &r->released[i], NULL) == VMAFX_OK);
    ok = ok && vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK;
    if (gated) {
        r->unread += ok && vmafx_fence_wait(&r->released[i], READERS_TIMEOUT_NS, NULL) != VMAFX_OK;
        gate_open(&r->gate, i);
    }
    return ok;
}

/* The fenced session: every frame produced and imported at once. */
static VmafxContext *run_fenced(FencedRun *r, const Clip *c, const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = fenced_frame(r, c, context, i);
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    return ok ? context : NULL;
}

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

/* Frames of `fenced` from `first` on whose `feature` differs from `host`'s. */
static unsigned bad_frames(VmafxContext *host, VmafxContext *fenced, const char *feature,
                           unsigned first)
{
    unsigned bad = 0;
    for (unsigned i = first; i < N_FRAMES; i++) {
        VmafxScore a = VMAFX_SCORE_INIT;
        VmafxScore b = VMAFX_SCORE_INIT;
        const bool ok = vmafx_feature_score(host, feature, i, &a, NULL) == VMAFX_OK &&
                        vmafx_feature_score(fenced, feature, i, &b, NULL) == VMAFX_OK;
        bad += !ok || !vt_same_bits(a.value, b.value);
    }
    return bad;
}

/* Free the run once the producer's stream has drained: no hold reads the
 * gate any more. */
static void fenced_run_free(FencedRun *r)
{
    gate_open(&r->gate, N_FRAMES);
    if (vc_push(&gpu)) {
        (void)gpu.f->cuStreamSynchronize(gpu.producer);
        vc_pop(&gpu);
    }
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vc_free(&gpu, &r->bufs[k]);
        if (r->events[k]) {
            (void)gpu.f->cuEventDestroy(r->events[k]);
        }
    }
    for (unsigned i = 0; i < N_FRAMES; i++) {
        (void)vmafx_fence_destroy(&r->released[i], NULL);
    }
    (void)pthread_cond_destroy(&r->gate.cond);
    (void)pthread_mutex_destroy(&r->gate.lock);
}

/* What one arm saw: bad frames of the feature (-1: the run failed) and, with
 * the late producer, frames whose readers had not run in time and holds that
 * gave up before their frame was opened. */
typedef struct ArmResult {
    int bad;
    unsigned unread;
    unsigned timeouts;
} ArmResult;

/* One arm: bad frames of `feature` under load, with the planted switches;
 * `late` runs the late producer. */
static ArmResult acquire_arm(const Clip *c, const VcCell *cell, const char *feature,
                             uint32_t switches, bool late)
{
    static FencedRun r;
    ArmResult res = {.bad = -1, .unread = 0u, .timeouts = 0u};
    if (!fenced_run_init(&r, late)) {
        return res;
    }
    Load load;
    const bool loaded = load_start(&load);
    vmafx_test_set_switches(switches);
    VmafxContext *const fenced = loaded ? run_fenced(&r, c, cell) : NULL;
    vmafx_test_set_switches(0u);
    load_stop(&load);
    VmafxContext *const host = fenced ? run_host(c, cell) : NULL;
    res.bad = fenced && host ? (int)bad_frames(host, fenced, feature, late ? LATE_FIRST : 0u) : -1;
    if (fenced) {
        (void)vmafx_context_destroy(fenced, NULL);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    fenced_run_free(&r);
    res.unread = r.unread;
    res.timeouts = r.gate.timeouts;
    return res;
}

/* The verdict on one cell: every frame right with the wait; without it,
 * every late frame wrong, each read before the late producer wrote it. */
static char *acquire_verdict(const ArmResult *waited, const ArmResult *skipped)
{
    if (waited->bad != 0) {
        return "a fenced frame scored wrong";
    }
    if (skipped->timeouts != 0u) {
        return "the readers waited for the late producer: something besides the acquire wait "
               "orders them";
    }
    if (skipped->unread != 0u) {
        return "a frame's readers had not run when the late producer wrote it";
    }
    return skipped->bad != (int)(N_FRAMES - LATE_FIRST) ? "the planted skipped wait went unseen" :
                                                          NULL;
}

static char *test_acquire_order_under_load(void)
{
    Clip c;
    if (!clip_open(&c)) {
        clip_close(&c);
        return "clip";
    }
    static const VcCell *const cells[] = {&vc_cells[17], &vc_cells[0]}; /* psnr, adm */
    static const char *const features[] = {"psnr_y", "VMAF_integer_feature_adm2_score"};
    char *msg = NULL;
    for (unsigned k = 0; k < 2u && !msg; k++) {
        const ArmResult waited = acquire_arm(&c, cells[k], features[k], 0u, false);
        const ArmResult skipped =
            acquire_arm(&c, cells[k], features[k], VMAFX_TEST_SKIP_ACQUIRE_WAIT, true);
        (void)fprintf(stderr,
                      "[%s: %d bad of %u with the wait; without, %d bad of %u late frames, %u "
                      "unread in time, %u holds gave up] ",
                      cells[k]->name, waited.bad, N_FRAMES, skipped.bad, N_FRAMES - LATE_FIRST,
                      skipped.unread, skipped.timeouts);
        msg = acquire_verdict(&waited, &skipped);
    }
    clip_close(&c);
    return msg;
}

/* ---- Release ordering ------------------------------------------------------------------- */

/* A device whose library stream is the test's, so the test can hold it. */
static VmafxDevice *held_device(CUstream *stream)
{
    VmafxDevice *device = NULL;
    if (!vc_push(&gpu) || gpu.f->cuStreamCreate(stream, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS) {
        vc_pop(&gpu);
        return NULL;
    }
    vc_pop(&gpu);
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_CUDA;
    desc.external[0] = (uintptr_t)gpu.ctx;
    desc.external[1] = (uintptr_t)*stream;
    return vmafx_device_create(&desc, &device, NULL) == VMAFX_OK ? device : NULL;
}

typedef struct ReleaseRun ReleaseRun;

/* What the release callback of one distorted frame needs. */
typedef struct Pending {
    ReleaseRun *run;
    unsigned i;
    VmafxFence release; /* its CUDA_EVENT release fence */
} Pending;

struct ReleaseRun {
    VmafxDevice *device;
    CUstream stream;
    VcPlanes bufs[2u * N_FRAMES];
    CUevent landed[N_FRAMES];
    Pending pending[N_FRAMES];
    unsigned callbacks;      /* release callbacks run */
    unsigned early_canaries; /* canaries written before their frame was read */
};

/* The frame's release callback (VmafxFrameImport.release): the producer's
 * stream waits on the release event, then writes a canary over the frame's
 * luma; `landed` is recorded behind it. No host wait. */
static void on_release(void *user)
{
    Pending *const p = user;
    ReleaseRun *const r = p->run;
    const VcPlanes *const buf = &r->bufs[2u * p->i + 1u];
    const size_t luma = (size_t)buf->pitch[0] * H;
    r->callbacks++;
    if (!vc_push(&gpu)) {
        return;
    }
    (void)gpu.f->cuStreamWaitEvent(gpu.producer, vc_event(&p->release), 0);
    (void)gpu.f->cuMemsetD8Async(buf->base + buf->offset[0], 0xff, luma, gpu.producer);
    if (gpu.f->cuEventCreate(&r->landed[p->i], CU_EVENT_DISABLE_TIMING) == CUDA_SUCCESS) {
        (void)gpu.f->cuEventRecord(r->landed[p->i], gpu.producer);
    }
    vc_pop(&gpu);
}

/* Frame i: uploaded, imported (the distorted frame with the release
 * callback and a CUDA_EVENT release fence), the library stream held so the
 * readers queue up, submitted. */
static bool release_frame(ReleaseRun *r, const Clip *c, VmafxContext *context, unsigned i)
{
    VmafxFrame *pair[2] = {NULL, NULL};
    Pending *const p = &r->pending[i];
    p->run = r;
    p->i = i;
    p->release = (VmafxFence)VMAFX_FENCE_INIT;
    bool ok = true;
    for (unsigned s = 0; s < 2u && ok; s++) {
        const unsigned k = 2u * i + s;
        ok = vc_upload(&gpu, &c->desc, (s ? c->dist : c->ref) + i * vt_frame_bytes(&c->desc),
                       VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &r->bufs[k]) &&
             vc_push(&gpu) && gpu.f->cuStreamSynchronize(gpu.producer) == CUDA_SUCCESS &&
             (vc_pop(&gpu), true);
        VmafxFrameImport imp =
            vc_import_desc(&c->desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &r->bufs[k]);
        imp.release = s ? on_release : NULL;
        imp.user = s ? p : NULL;
        ok = ok && vmafx_frame_import(r->device, &imp, &pair[s], NULL) == VMAFX_OK;
    }
    ok = ok &&
         vmafx_frame_release_fence(pair[1], VMAFX_FENCE_CUDA_EVENT, &p->release, NULL) == VMAFX_OK;
    ok = ok && vc_push(&gpu) &&
         gpu.f->cuLaunchHostFunc(r->stream, hold_stream, NULL) == CUDA_SUCCESS &&
         (vc_pop(&gpu), true);
    ok = ok && vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK;
    /* The readers of frame i wait behind the held library stream: a canary
     * that has landed now was written before they read the frame. */
    r->early_canaries += ok && r->landed[i] && gpu.f->cuEventQuery(r->landed[i]) == CUDA_SUCCESS;
    return ok;
}

static VmafxContext *run_release(ReleaseRun *r, const Clip *c, const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(r->device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = release_frame(r, c, context, i);
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    return ok ? context : NULL;
}

static void release_run_free(ReleaseRun *r)
{
    if (vc_push(&gpu)) {
        (void)gpu.f->cuStreamSynchronize(gpu.producer);
        for (unsigned i = 0; i < N_FRAMES; i++) {
            if (r->landed[i]) {
                (void)gpu.f->cuEventDestroy(r->landed[i]);
            }
        }
        vc_pop(&gpu);
    }
    for (unsigned i = 0; i < N_FRAMES; i++) {
        (void)vmafx_fence_destroy(&r->pending[i].release, NULL);
    }
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vc_free(&gpu, &r->bufs[k]);
    }
    vmafx_device_unref(r->device);
    if (r->stream && vc_push(&gpu)) {
        (void)gpu.f->cuStreamDestroy(r->stream);
        vc_pop(&gpu);
    }
}

/* One arm: bad frames of psnr_y, and canaries that landed before their
 * frame was released. */
static int release_arm(const Clip *c, uint32_t switches, unsigned *early)
{
    static ReleaseRun r;
    memset(&r, 0, sizeof(r));
    r.device = held_device(&r.stream);
    vmafx_test_set_switches(switches);
    VmafxContext *const imported = r.device ? run_release(&r, c, &vc_cells[17]) : NULL;
    vmafx_test_set_switches(0u);
    VmafxContext *const host = imported ? run_host(c, &vc_cells[17]) : NULL;
    const int bad = imported && host ? (int)bad_frames(host, imported, "psnr_y", 0u) : -1;
    *early = r.early_canaries;
    if (r.callbacks != N_FRAMES) {
        (void)fprintf(stderr, "[%u release callbacks of %u] ", r.callbacks, N_FRAMES);
    }
    if (imported) {
        (void)vmafx_context_destroy(imported, NULL);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
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

/* ---- Frames without a fence: the ADR-1199 barrier ------------------------------------ */

/* The producer writes a pool frame on its stream behind a held host
 * function, from the uploaded source `src`, and submits it without a fence. */
static bool produce_into_pool(const Clip *c, const VcPlanes *src, VmafxFrame *frame)
{
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(&c->desc, w, h, row);
    bool ok = vmafx_frame_planes(frame, &planes, NULL) == VMAFX_OK && vc_push(&gpu);
    for (uint32_t i = 0; i < planes.n_planes && i < 3u && ok; i++) {
        CUdeviceptr dst = 0;
        memcpy((void *)&dst, (const void *)&planes.data[i], sizeof(dst));
        ok = gpu.f->cuMemsetD8Async(dst, 0, (size_t)planes.stride[i] * h[i], gpu.producer) ==
             CUDA_SUCCESS;
    }
    ok = ok && gpu.f->cuStreamSynchronize(gpu.producer) == CUDA_SUCCESS &&
         gpu.f->cuLaunchHostFunc(gpu.producer, hold_stream, NULL) == CUDA_SUCCESS;
    for (uint32_t i = 0; i < planes.n_planes && i < 3u && ok; i++) {
        CUDA_MEMCPY2D m = {.srcMemoryType = CU_MEMORYTYPE_DEVICE,
                           .srcDevice = src->base + src->offset[i],
                           .srcPitch = (size_t)src->pitch[i],
                           .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                           .dstPitch = (size_t)planes.stride[i],
                           .WidthInBytes = row[i],
                           .Height = h[i]};
        memcpy((void *)&m.dstDevice, (const void *)&planes.data[i], sizeof(m.dstDevice));
        ok = gpu.f->cuMemcpy2DAsync(&m, gpu.producer) == CUDA_SUCCESS;
    }
    vc_pop(&gpu);
    return ok;
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

/* Pool frames carry no fence: the engine's once-per-frame synchronisation of
 * the context (ADR-1199) orders the producer's writes, which the producer
 * makes on a stream the library does not know. Every frame scores as the host
 * frame. */
static char *test_pool_frames_ordered_by_barrier(void)
{
    Clip c;
    if (!clip_open(&c)) {
        clip_close(&c);
        return "clip";
    }
    VmafxFramePool *pool = NULL;
    const bool made = vmafx_frame_pool_create(gpu.device, &c.desc, 6u, &pool, NULL) == VMAFX_OK;
    Load load;
    const bool loaded = made && load_start(&load);
    VmafxContext *const pooled = loaded ? run_pool(&c, pool, &vc_cells[17]) : NULL;
    if (loaded) {
        load_stop(&load);
    }
    VmafxContext *const host = pooled ? run_host(&c, &vc_cells[17]) : NULL;
    const int bad = pooled && host ? (int)bad_frames(host, pooled, "psnr_y", 0u) : -1;
    if (pooled) {
        (void)vmafx_context_destroy(pooled, NULL);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    vmafx_frame_pool_destroy(pool);
    clip_close(&c);
    (void)fprintf(stderr, "[pool frames without a fence: %d bad of %u] ", bad, N_FRAMES);
    mu_assert("frames without a fence are ordered by the barrier", bad == 0);
    return NULL;
}

/* A frame's HOST release fence is signalled when the device has run its
 * readers, not when its last reference is dropped: with the library stream
 * held, the fence is pending right after the release and signalled once the
 * stream runs on. */
static char *test_host_release_after_device(void)
{
    CUstream stream = NULL;
    VmafxDevice *const device = held_device(&stream);
    VcPlanes p;
    memset(&p, 0, sizeof(p));
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    uint8_t *const data = malloc(vt_frame_bytes(&d));
    bool ok = device && data;
    if (ok) {
        vt_fill(&d, data, 3u);
        ok = vc_upload(&gpu, &d, data, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &p) && vc_push(&gpu) &&
             gpu.f->cuStreamSynchronize(gpu.producer) == CUDA_SUCCESS && (vc_pop(&gpu), true);
    }
    const VmafxFrameImport imp = vc_import_desc(&d, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxFrame *frame = NULL;
    VmafxFence fence = VMAFX_FENCE_INIT;
    ok = ok && vmafx_frame_import(device, &imp, &frame, NULL) == VMAFX_OK &&
         vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_OK &&
         vc_push(&gpu) && gpu.f->cuLaunchHostFunc(stream, hold_stream, NULL) == CUDA_SUCCESS &&
         (vc_pop(&gpu), true);
    vmafx_frame_unref(frame); /* the last reference: the release is enqueued */
    const VmafxStatus at_release = ok ? vmafx_fence_wait(&fence, 0u, NULL) : VMAFX_E_INVALID;
    const VmafxStatus later = ok ? vmafx_fence_wait(&fence, 5000000000ull, NULL) : VMAFX_E_INVALID;
    (void)vmafx_fence_destroy(&fence, NULL);
    vc_free(&gpu, &p);
    free(data);
    vmafx_device_unref(device);
    if (stream && vc_push(&gpu)) {
        (void)gpu.f->cuStreamDestroy(stream);
        vc_pop(&gpu);
    }
    mu_assert("set up", ok);
    mu_assert("pending while the device still holds the frame", at_release == VMAFX_PENDING);
    mu_assert("signalled after the device ran on", later == VMAFX_OK);
    return NULL;
}

/* ---- The import rule and CUDA_EVENT fences ------------------------------------------- */

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
    VmafxContext *const context = vc_cell_context(gpu.device, &vc_cells[17]);
    VmafxFrameImport imp = vc_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &c.src[0]);
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
    if (context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    clip_close(&c);
    mu_assert("retried once after the host wait", status == VMAFX_OK && attempts == 2u);
    return NULL;
}

static char *test_cuda_event_fences(void)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("create",
              vmafx_fence_create(gpu.device, VMAFX_FENCE_CUDA_EVENT, &fence, NULL) == VMAFX_OK &&
                  fence.kind == VMAFX_FENCE_CUDA_EVENT && fence.handle != 0u);
    bool ok = vc_push(&gpu) &&
              gpu.f->cuLaunchHostFunc(gpu.producer, hold_stream, NULL) == CUDA_SUCCESS &&
              gpu.f->cuEventRecord(vc_event(&fence), gpu.producer) == CUDA_SUCCESS;
    vc_pop(&gpu);
    mu_assert("recorded behind a held stream", ok);
    mu_assert("poll", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_PENDING);
    mu_assert("wait", vmafx_fence_wait(&fence, 5000000000ull, NULL) == VMAFX_OK);
    VmafxError *error = NULL;
    mu_assert("the host signals no event",
              vmafx_fence_signal(&fence, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "fence.kind", VMAFX_SUBJECT_FENCE));
    mu_assert("destroy", vmafx_fence_destroy(&fence, NULL) == VMAFX_OK);
    mu_assert("no GL sync from the library",
              vmafx_fence_create(gpu.device, VMAFX_FENCE_GL_SYNC, &fence, &error) ==
                      VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "kind", VMAFX_SUBJECT_FENCE));
    return NULL;
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
GUARDED(test_pool_frames_ordered_by_barrier)
GUARDED(test_import_rule_host_acquire)
GUARDED(test_cuda_event_fences)

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vc_open(&gpu);
    static const MuTest tests[] = {
        MU_TEST(test_acquire_order_under_load_g),  MU_TEST(test_release_canary_g),
        MU_TEST(test_host_release_after_device_g), MU_TEST(test_pool_frames_ordered_by_barrier_g),
        MU_TEST(test_import_rule_host_acquire_g),  MU_TEST(test_cuda_event_fences_g),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    vc_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
