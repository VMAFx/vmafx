/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Fence ordering of the VMAFx HIP lane under concurrent device load (RC4
 * WP3, ADR-1929, ADR-2092), modelled on the ADR-1199 harness
 * (scripts/test/repro-cuda-ffmpeg-nondeterminism.sh: a producer the library
 * cannot see, another stream keeping the device busy):
 *
 * - HIP_EVENT acquire: the producer writes every frame on its own stream
 *   behind a host function that holds that stream for a few milliseconds,
 *   then records the acquire event; the import is made at once. With the
 *   library's wait on its stream every frame scores as the host frame; with
 *   the planted skipped wait (VMAFX_TEST_SKIP_ACQUIRE_WAIT) the twins read
 *   the planes before the producer wrote them and frames score wrong.
 * - SYNC_FILE acquire: the producer writes every frame into a new dma-buf
 *   through its GBM mapping, which the driver lands with a GPU copy behind a
 *   kernel fence, and hands over the dma-buf's sync_file. Through the import
 *   rule (a host wait on the sync_file, then the import) every frame scores
 *   as the host frame; with the planted skipped wait the twins read the
 *   buffer before the copy landed.
 * - Release: the library stream is held behind a host function while the
 *   frame's readers queue up; the frame's release callback makes the
 *   producer's stream wait on the HIP_EVENT release fence and write a canary
 *   into the frame (no host wait). With the real release the canary lands
 *   after the readers; with the planted early release
 *   (VMAFX_TEST_EARLY_RELEASE: the event recorded and the callback run when
 *   the submit returns) the readers see the canary.
 * - A HOST release fence stays pending while the device holds the frame.
 * - The import rule on HIP: an unsignalled HOST acquire fence is waited on by
 *   vmafx_context_import_frame() and the import retried once.
 * - HIP_EVENT fences: created on the device, polled, waited on, destroyed.
 *
 * The gfx1036 this lane is verified on now and then never runs a run of a
 * stream's commands (T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01, host
 * frames included): an arm that must score every frame right is run again
 * when a frame differs, up to ARM_ATTEMPTS times, and each such attempt is
 * reported. A missing wait makes most frames wrong in every attempt.
 *
 * Needs a HIP device (77 without one); the SYNC_FILE case needs libgbm and
 * the device's render node.
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
#include "vmafx_hip_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 1920u
#define H 1080u
#define N_FRAMES 16u
/* How long a host function holds the producer's or the library's stream. */
#define HOLD_MS 30u
/* Bytes the load stream keeps setting while a test runs. */
#define LOAD_BYTES (64u << 20)
/* Bytes of each dma-buf of the SYNC_FILE arms. */
#define SLOW_DMABUF_BYTES (48u << 20)
/* Attempts of an arm that must score every frame right (the platform
 * defect above). */
#define ARM_ATTEMPTS 3u

static VhGpu gpu;
static bool have_gpu;
#ifdef VMAFX_TEST_HAVE_GBM
static VhGbm gbm;
static bool have_gbm;
#endif
static unsigned arm_reruns;

static void hold_stream(void *arg)
{
    (void)arg;
    const struct timespec t = {.tv_sec = 0, .tv_nsec = (long)HOLD_MS * 1000000L};
    (void)nanosleep(&t, NULL);
}

/* ---- Device load (the ADR-1199 harness's concurrent work) -------------------------- */

typedef struct Load {
    hipStream_t stream;
    void *buf;
    pthread_t thread;
    volatile int stop;
    bool running;
} Load;

static void *load_main(void *arg)
{
    Load *const load = arg;
    (void)hipSetDevice(0);
    for (unsigned i = 0; i < 100000u && !load->stop; i++) {
        (void)hipMemsetAsync(load->buf, (int)(i & 0xffu), LOAD_BYTES, load->stream);
        (void)hipStreamSynchronize(load->stream);
    }
    return NULL;
}

static bool load_start(Load *load)
{
    memset(load, 0, sizeof(*load));
    const bool ok = hipStreamCreateWithFlags(&load->stream, hipStreamNonBlocking) == hipSuccess &&
                    hipMalloc(&load->buf, LOAD_BYTES) == hipSuccess;
    load->running = ok && pthread_create(&load->thread, NULL, load_main, load) == 0;
    return load->running;
}

static void load_stop(Load *load)
{
    load->stop = 1;
    if (load->running) {
        (void)pthread_join(load->thread, NULL);
    }
    (void)hipStreamSynchronize(load->stream);
    (void)hipFree(load->buf);
    (void)hipStreamDestroy(load->stream);
}

/* ---- Frames ------------------------------------------------------------------------- */

typedef struct Clip {
    VmafxFrameDesc desc;
    uint8_t *ref;
    uint8_t *dist;
    VhPlanes src[2u * N_FRAMES]; /* the frames, uploaded once (the producer's source) */
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
        ok = vh_upload(&gpu, &c->desc, c->ref + i * frame, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u,
                       &c->src[(size_t)2u * i]) &&
             vh_upload(&gpu, &c->desc, c->dist + i * frame, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u,
                       &c->src[(size_t)2u * i + 1u]);
    }
    return ok;
}

static void clip_close(Clip *c)
{
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vh_free(&gpu, &c->src[k]);
    }
    free(c->ref);
    free(c->dist);
}

/* Bytes of one uploaded frame's buffer. */
static size_t frame_buffer_bytes(const Clip *c, const VhPlanes *src)
{
    return (size_t)(src->layout.offset[2] + src->layout.pitch[2] * ((c->desc.h + 1u) / 2u));
}

/* A frame buffer of the producer: zeroed, then written on the producer's
 * stream from `src` behind a host function that holds the stream, then the
 * acquire event recorded. */
static bool produce(const Clip *c, const VhPlanes *src, VhPlanes *dst, hipEvent_t *acquire)
{
    *dst = *src;
    dst->base = NULL;
    const size_t bytes = frame_buffer_bytes(c, src);
    return hipMalloc((void **)&dst->base, bytes) == hipSuccess &&
           hipMemsetAsync(dst->base, 0, bytes, gpu.producer) == hipSuccess &&
           hipStreamSynchronize(gpu.producer) == hipSuccess &&
           hipLaunchHostFunc(gpu.producer, hold_stream, NULL) == hipSuccess &&
           hipMemcpyAsync(dst->base, src->base, bytes, hipMemcpyDeviceToDevice, gpu.producer) ==
               hipSuccess &&
           hipEventCreateWithFlags(acquire, hipEventDisableTiming) == hipSuccess &&
           hipEventRecord(*acquire, gpu.producer) == hipSuccess;
}

/* ---- Sessions ------------------------------------------------------------------------ */

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

/* Bad frames of `imported` against a host session of `cell`; -1 when a
 * session failed. Destroys `imported`. */
static int score_against_host(const Clip *c, const VcCell *cell, VmafxContext *imported,
                              const char *feature)
{
    VmafxContext *const host = imported ? run_host(c, cell) : NULL;
    const int bad = imported && host ? (int)bad_frames(host, imported, feature) : -1;
    if (imported) {
        (void)vmafx_context_destroy(imported, NULL);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    return bad;
}

/* An arm with the real wait: 0 bad frames in one of ARM_ATTEMPTS attempts
 * (each attempt that is not is reported). */
static int clean_arm(int (*arm)(const Clip *c, const VcCell *cell, const char *feature,
                                uint32_t switches),
                     const Clip *c, const VcCell *cell, const char *feature)
{
    int bad = -1;
    for (unsigned a = 0; a < ARM_ATTEMPTS && bad != 0; a++) {
        bad = arm(c, cell, feature, 0u);
        if (bad > 0) {
            arm_reruns++;
            (void)fprintf(stderr, "[%s, attempt %u: %d bad of %u, run again] ", cell->name, a + 1u,
                          bad, N_FRAMES);
        }
    }
    return bad;
}

/* ---- HIP_EVENT acquire ------------------------------------------------------------- */

/* The fenced session: every frame produced and imported at once. */
static VmafxContext *run_fenced(const Clip *c, const VcCell *cell, VhPlanes *bufs,
                                hipEvent_t *events)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        for (unsigned s = 0; s < 2u && ok; s++) {
            const unsigned k = 2u * i + s;
            ok = produce(c, &c->src[k], &bufs[k], &events[k]);
            VmafxFrameImport imp =
                vh_import_desc(&c->desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &bufs[k]);
            imp.acquire.kind = VMAFX_FENCE_HIP_EVENT;
            imp.acquire.handle = vmaf_hip_event_bits(events[k]);
            ok = ok && vmafx_frame_import(gpu.device, &imp, &pair[s], NULL) == VMAFX_OK;
        }
        if (!ok) {
            vmafx_frame_unref(pair[0]);
            vmafx_frame_unref(pair[1]);
            break;
        }
        ok = vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK;
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    return ok ? context : NULL;
}

static void free_run(VhPlanes *bufs, hipEvent_t *events)
{
    (void)hipDeviceSynchronize();
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vh_free(&gpu, &bufs[k]);
        if (events[k]) {
            (void)hipEventDestroy(events[k]);
        }
    }
}

/* One arm: bad frames of `feature` under load, with the planted switches. */
static int acquire_arm(const Clip *c, const VcCell *cell, const char *feature, uint32_t switches)
{
    VhPlanes bufs[2u * N_FRAMES];
    hipEvent_t events[2u * N_FRAMES];
    memset(bufs, 0, sizeof(bufs));
    memset((void *)events, 0, sizeof(events));
    Load load;
    const bool loaded = load_start(&load);
    vmafx_test_set_switches(switches);
    VmafxContext *const fenced = loaded ? run_fenced(c, cell, bufs, events) : NULL;
    vmafx_test_set_switches(0u);
    load_stop(&load);
    const int bad = score_against_host(c, cell, fenced, feature);
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
    /* psnr and vif: twins that make no host wait of their own per frame
     * (adm_hip's parameter upload does, which hides a missing acquire). */
    static const VcCell *const cells[] = {&vc_cells[17], &vc_cells[23]};
    static const char *const features[] = {"psnr_y", "VMAF_integer_feature_vif_scale0_score"};
    char *msg = NULL;
    for (unsigned k = 0; k < 2u && !msg; k++) {
        const int waited = clean_arm(acquire_arm, &c, cells[k], features[k]);
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

/* ---- SYNC_FILE acquire -------------------------------------------------------------- */

#ifdef VMAFX_TEST_HAVE_GBM

/* Frames whose sync_file was pending when they were imported; of them, those
 * the import rule waited for (two import attempts) and those imported at
 * once (one attempt: the planted skipped wait). */
static unsigned sf_pending;
static unsigned sf_waited;
static unsigned sf_passed_unsignalled;

/* The dma-buf session: every frame written through its GBM mapping and
 * imported with its sync_file at once, through the import rule (a host wait
 * on the sync_file, then the import); the planted skipped wait passes the
 * unsignalled sync_file. */
static VmafxContext *run_dmabuf(const Clip *c, const VcCell *cell, VhDmabuf *bufs)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    const size_t frame = vt_frame_bytes(&c->desc);
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        for (unsigned s = 0; s < 2u && ok; s++) {
            const unsigned k = 2u * i + s;
            ok = vh_dmabuf_write(&gbm, &c->desc, (s ? c->dist : c->ref) + i * frame,
                                 VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &bufs[k]);
            VmafxFrameImport imp =
                vh_dmabuf_desc(&c->desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &bufs[k]);
            imp.acquire.kind = VMAFX_FENCE_SYNC_FILE;
            imp.acquire.fd = ok ? vh_dmabuf_sync_file(&bufs[k]) : -1;
            const bool pending =
                imp.acquire.fd >= 0 && vmafx_fence_wait(&imp.acquire, 0u, NULL) == VMAFX_PENDING;
            const uint64_t attempts = vmafx_test_import_attempts();
            ok = ok && imp.acquire.fd >= 0 &&
                 vmafx_context_import_frame(context, gpu.device, &imp, "main", &pair[s], NULL) ==
                     VMAFX_OK;
            sf_pending += pending;
            sf_waited += ok && pending && vmafx_test_import_attempts() - attempts == 2u;
            sf_passed_unsignalled += ok && pending && vmafx_test_import_attempts() - attempts == 1u;
            if (imp.acquire.fd >= 0) {
                (void)close(imp.acquire.fd);
            }
        }
        if (!ok) {
            vmafx_frame_unref(pair[0]);
            vmafx_frame_unref(pair[1]);
            break;
        }
        ok = vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK;
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    return ok ? context : NULL;
}

static int sync_file_arm(const Clip *c, const VcCell *cell, const char *feature, uint32_t switches)
{
    VhDmabuf bufs[2u * N_FRAMES];
    memset(bufs, 0, sizeof(bufs));
    Load load;
    const bool loaded = load_start(&load);
    vmafx_test_set_switches(switches);
    VmafxContext *const imported = loaded ? run_dmabuf(c, cell, bufs) : NULL;
    vmafx_test_set_switches(0u);
    load_stop(&load);
    const int bad = score_against_host(c, cell, imported, feature);
    (void)hipDeviceSynchronize();
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vh_dmabuf_free(&bufs[k]);
    }
    return bad;
}

/* SYNC_FILE acquire. The driver lands each write of the slow producer at
 * unmap with a copy behind a kernel fence, and hands its sync_file over
 * while it is pending. The import rule waits for it on the host (two import
 * attempts) and the frames score as the host frames; with the planted
 * skipped wait the import takes the pending sync_file at once. The AMD
 * runtime's import of a dma-buf as external memory itself waits for the
 * buffer's fences (measured: about 2 ms longer, the fence signalled by the
 * time it returns), so the planted arm still scores right; it is the import
 * taking an unsignalled sync_file that the gate shows. */
static char *test_sync_file_acquire_under_load(void)
{
    if (!have_gbm) {
        (void)fprintf(stderr, "[no render node: skipped] ");
        return NULL;
    }
    Clip c;
    if (!clip_open(&c)) {
        clip_close(&c);
        return "clip";
    }
    gbm.min_bytes = SLOW_DMABUF_BYTES;
    sf_pending = sf_waited = sf_passed_unsignalled = 0u;
    const int waited = clean_arm(sync_file_arm, &c, &vc_cells[17], "psnr_y");
    const unsigned real_pending = sf_pending;
    const unsigned real_waited = sf_waited;
    const unsigned real_passed = sf_passed_unsignalled;
    sf_pending = sf_waited = sf_passed_unsignalled = 0u;
    const int skipped = sync_file_arm(&c, &vc_cells[17], "psnr_y", VMAFX_TEST_SKIP_ACQUIRE_WAIT);
    gbm.min_bytes = 0u;
    clip_close(&c);
    (void)fprintf(stderr,
                  "[sync_file: %d bad of %u with the wait (%u pending at import, %u waited for, %u "
                  "taken unsignalled); planted skip: %d bad, %u pending, %u taken unsignalled] ",
                  waited, N_FRAMES, real_pending, real_waited, real_passed, skipped, sf_pending,
                  sf_passed_unsignalled);
    mu_assert("a frame behind its sync_file scored wrong", waited == 0);
    mu_assert("the import rule waited for pending sync_files",
              real_waited > 0u && real_passed == 0u);
    mu_assert("the planted skipped wait went unseen", sf_passed_unsignalled > 0u);
    return NULL;
}

#else

static char *test_sync_file_acquire_under_load(void)
{
    (void)fprintf(stderr, "[built without libgbm: skipped] ");
    return NULL;
}

#endif /* VMAFX_TEST_HAVE_GBM */

/* ---- Release ordering ------------------------------------------------------------------- */

/* A device whose library stream is the test's, so the test can hold it. */
static VmafxDevice *held_device(hipStream_t *stream)
{
    VmafxDevice *device = NULL;
    if (hipStreamCreateWithFlags(stream, hipStreamNonBlocking) != hipSuccess) {
        return NULL;
    }
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_HIP;
    desc.external[0] = vmaf_hip_stream_bits(*stream);
    return vmafx_device_create(&desc, &device, NULL) == VMAFX_OK ? device : NULL;
}

typedef struct ReleaseRun ReleaseRun;

/* What the release callback of one distorted frame needs. */
typedef struct Pending {
    ReleaseRun *run;
    unsigned i;
    VmafxFence release; /* its HIP_EVENT release fence */
} Pending;

struct ReleaseRun {
    VmafxDevice *device;
    hipStream_t stream;
    VhPlanes bufs[2u * N_FRAMES];
    hipEvent_t landed[N_FRAMES];
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
    const VhPlanes *const buf = &r->bufs[2u * p->i + 1u];
    const size_t luma = (size_t)buf->layout.pitch[0] * H;
    r->callbacks++;
    (void)hipStreamWaitEvent(gpu.producer, vh_event(&p->release), 0u);
    (void)hipMemsetAsync(buf->base + buf->layout.offset[0], 0xff, luma, gpu.producer);
    if (hipEventCreateWithFlags(&r->landed[p->i], hipEventDisableTiming) == hipSuccess) {
        (void)hipEventRecord(r->landed[p->i], gpu.producer);
    }
}

/* Frame i: uploaded, imported (the distorted frame with the release
 * callback and a HIP_EVENT release fence), the library stream held so the
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
        ok = vh_upload(&gpu, &c->desc, (s ? c->dist : c->ref) + i * vt_frame_bytes(&c->desc),
                       VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &r->bufs[k]);
        VmafxFrameImport imp =
            vh_import_desc(&c->desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &r->bufs[k]);
        imp.release = s ? on_release : NULL;
        imp.user = s ? p : NULL;
        ok = ok && vmafx_frame_import(r->device, &imp, &pair[s], NULL) == VMAFX_OK;
    }
    ok = ok &&
         vmafx_frame_release_fence(pair[1], VMAFX_FENCE_HIP_EVENT, &p->release, NULL) == VMAFX_OK;
    ok = ok && hipLaunchHostFunc(r->stream, hold_stream, NULL) == hipSuccess;
    if (!ok || !pair[0] || !pair[1]) {
        vmafx_frame_unref(pair[0]);
        vmafx_frame_unref(pair[1]);
        return false;
    }
    /* The producer keeps its reference across the submit, as a decoder
     * holding its output does: the HIP twins copy a frame's planes when it is
     * submitted, and the engine would otherwise drop the last reference
     * inside the submit. */
    VmafxFrame *const held = vmafx_frame_ref(pair[1]);
    ok = vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK;
    /* The readers of frame i wait behind the held library stream: a canary
     * that has landed now was written before they read the frame. */
    r->early_canaries += ok && r->landed[i] && hipEventQuery(r->landed[i]) == hipSuccess;
    vmafx_frame_unref(held); /* the last reference: the release is enqueued */
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
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    return ok ? context : NULL;
}

static void release_run_free(ReleaseRun *r)
{
    (void)hipStreamSynchronize(gpu.producer);
    for (unsigned i = 0; i < N_FRAMES; i++) {
        if (r->landed[i]) {
            (void)hipEventDestroy(r->landed[i]);
        }
        (void)vmafx_fence_destroy(&r->pending[i].release, NULL);
    }
    for (unsigned k = 0; k < 2u * N_FRAMES; k++) {
        vh_free(&gpu, &r->bufs[k]);
    }
    vmafx_device_unref(r->device);
    if (r->stream) {
        (void)hipStreamDestroy(r->stream);
    }
}

/* One arm: bad frames of psnr_y, and canaries that landed before their
 * frame was released (`*early`). */
static int release_arm_with(const Clip *c, uint32_t switches, unsigned *early)
{
    static ReleaseRun r;
    memset(&r, 0, sizeof(r));
    r.device = held_device(&r.stream);
    vmafx_test_set_switches(switches);
    VmafxContext *const imported = r.device ? run_release(&r, c, &vc_cells[17]) : NULL;
    vmafx_test_set_switches(0u);
    const int bad = score_against_host(c, &vc_cells[17], imported, "psnr_y");
    *early = r.early_canaries;
    if (r.callbacks != N_FRAMES) {
        (void)fprintf(stderr, "[%u release callbacks of %u] ", r.callbacks, N_FRAMES);
    }
    release_run_free(&r);
    return bad;
}

static unsigned early_canaries;

/* The real release as an arm of clean_arm(): bad frames, early canaries
 * counted as bad. */
static int release_arm(const Clip *c, const VcCell *cell, const char *feature, uint32_t switches)
{
    (void)cell;
    (void)feature;
    unsigned early = 0;
    const int bad = release_arm_with(c, switches, &early);
    early_canaries = early;
    return bad < 0 ? bad : bad + (int)early;
}

static char *test_release_canary(void)
{
    Clip c;
    if (!clip_open(&c)) {
        clip_close(&c);
        return "clip";
    }
    const int real = clean_arm(release_arm, &c, &vc_cells[17], "psnr_y");
    const unsigned early_real = early_canaries;
    unsigned early_planted = 0;
    const int planted = release_arm_with(&c, VMAFX_TEST_EARLY_RELEASE, &early_planted);
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
 * readers, not when its last reference is dropped: with the library stream
 * held, the fence is pending right after the release and signalled once the
 * stream runs on. */
static char *test_host_release_after_device(void)
{
    hipStream_t stream = NULL;
    VmafxDevice *const device = held_device(&stream);
    VhPlanes p;
    memset(&p, 0, sizeof(p));
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    uint8_t *const data = malloc(vt_frame_bytes(&d));
    bool ok = device && data;
    if (ok) {
        vt_fill(&d, data, 3u);
        ok = vh_upload(&gpu, &d, data, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &p);
    }
    const VmafxFrameImport imp = vh_import_desc(&d, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxFrame *frame = NULL;
    VmafxFence fence = VMAFX_FENCE_INIT;
    ok = ok && vmafx_frame_import(device, &imp, &frame, NULL) == VMAFX_OK &&
         vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_OK &&
         hipLaunchHostFunc(stream, hold_stream, NULL) == hipSuccess;
    vmafx_frame_unref(frame); /* the last reference: the release is enqueued */
    const VmafxStatus at_release = ok ? vmafx_fence_wait(&fence, 0u, NULL) : VMAFX_E_INVALID;
    const VmafxStatus later = ok ? vmafx_fence_wait(&fence, 5000000000ull, NULL) : VMAFX_E_INVALID;
    (void)vmafx_fence_destroy(&fence, NULL);
    vh_free(&gpu, &p);
    free(data);
    vmafx_device_unref(device);
    if (stream) {
        (void)hipStreamDestroy(stream);
    }
    mu_assert("set up", ok);
    mu_assert("pending while the device still holds the frame", at_release == VMAFX_PENDING);
    mu_assert("signalled after the device ran on", later == VMAFX_OK);
    return NULL;
}

/* ---- The import rule and HIP_EVENT fences --------------------------------------------- */

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
    VmafxFrameImport imp = vh_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &c.src[0]);
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

static char *test_hip_event_fences(void)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("create",
              vmafx_fence_create(gpu.device, VMAFX_FENCE_HIP_EVENT, &fence, NULL) == VMAFX_OK &&
                  fence.kind == VMAFX_FENCE_HIP_EVENT && fence.handle != 0u);
    const bool ok = hipLaunchHostFunc(gpu.producer, hold_stream, NULL) == hipSuccess &&
                    hipEventRecord(vh_event(&fence), gpu.producer) == hipSuccess;
    mu_assert("recorded behind a held stream", ok);
    mu_assert("poll", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_PENDING);
    mu_assert("wait", vmafx_fence_wait(&fence, 5000000000ull, NULL) == VMAFX_OK);
    VmafxError *error = NULL;
    mu_assert("the host signals no event",
              vmafx_fence_signal(&fence, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "fence.kind", VMAFX_SUBJECT_FENCE));
    mu_assert("destroy", vmafx_fence_destroy(&fence, NULL) == VMAFX_OK);
    return NULL;
}

/* The library makes no GL sync and no sync_file (a producer's kinds). */
static char *test_no_producer_kinds(void)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    VmafxError *error = NULL;
    mu_assert("no GL sync from the library",
              vmafx_fence_create(gpu.device, VMAFX_FENCE_GL_SYNC, &fence, &error) ==
                      VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "kind", VMAFX_SUBJECT_FENCE));
    mu_assert("no sync_file from the library",
              vmafx_fence_create(gpu.device, VMAFX_FENCE_SYNC_FILE, &fence, &error) ==
                      VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "kind", VMAFX_SUBJECT_FENCE));
    return NULL;
}

static char *test_reruns(void)
{
    (void)fprintf(stderr, "[%u arm attempts run again (platform defect)] ", arm_reruns);
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
GUARDED(test_sync_file_acquire_under_load)
GUARDED(test_release_canary)
GUARDED(test_host_release_after_device)
GUARDED(test_import_rule_host_acquire)
GUARDED(test_hip_event_fences)
GUARDED(test_no_producer_kinds)
GUARDED(test_reruns)

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vh_open(&gpu);
#ifdef VMAFX_TEST_HAVE_GBM
    have_gbm = have_gpu && vh_gbm_open(&gpu, &gbm);
#endif
    static const MuTest tests[] = {
        MU_TEST(test_acquire_order_under_load_g), MU_TEST(test_sync_file_acquire_under_load_g),
        MU_TEST(test_release_canary_g),           MU_TEST(test_host_release_after_device_g),
        MU_TEST(test_import_rule_host_acquire_g), MU_TEST(test_hip_event_fences_g),
        MU_TEST(test_no_producer_kinds_g),        MU_TEST(test_reruns_g),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
#ifdef VMAFX_TEST_HAVE_GBM
    if (have_gbm) {
        vh_gbm_close(&gbm);
    }
#endif
    vh_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
