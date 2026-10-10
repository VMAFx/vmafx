/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Fence ordering of Vulkan frames on a device of the lane (RC4 WP3 Vulkan
 * lane, ADR-1929). Each arm is run with the real fences and with the planted
 * defect the arm exists to catch; the planted arm must score frames wrong and
 * the real one none (0 bad of N).
 *
 * - Acquire: every frame first holds a canary, then the producer writes the
 *   real frame behind a gate a helper thread opens a few milliseconds after
 *   the frame was imported and submitted. CUDA waits for the write on the
 *   device (a VULKAN_SEMAPHORE acquire fence); SYCL and HIP take the write's
 *   sync_file, which the import rule waits on before its retry. With the
 *   planted skipped wait (VMAFX_TEST_SKIP_ACQUIRE_WAIT) the readers see the
 *   canary where every import ran ahead of its write (CUDA always). An
 *   import that returned before its write finished may still read the
 *   finished frame (the write can land before the copy runs), which is no
 *   failure by itself: on the gfx1036 the import normally waits for the
 *   writer and now and then returns ahead of one or two writes.
 * - Release: the producer queues a canary write into the frame that waits on
 *   its own timeline for the release value, and the frame's readers are held
 *   back (CUDA: a further acquire fence on a delay timeline the helper opens
 *   late; SYCL / HIP: an acquire event behind work that holds the producer's
 *   queue or stream). Each frame is scored by two contexts, the second
 *   submitted once the first submit returned and the canary had its chance
 *   to land: a release is due only after the second context's readers.
 *   CUDA signals the release value on the device
 *   (vmafx_frame_signal_on_release()); on SYCL and HIP a helper thread waits
 *   on the HOST release fence and signals the value from the host, the
 *   documented pattern for a device that cannot signal a Vulkan semaphore.
 *   With the planted early release (VMAFX_TEST_EARLY_RELEASE, signalled when
 *   a submit returns) the canary lands before the second context reads. On
 *   SYCL and HIP every reader copies the producer's memory, so every frame
 *   is bad (a SYCL submit returns only after its own reads: one context
 *   alone saw the planted release on 1 or 2 of 8 frames). CUDA copies the
 *   memory once, at the import, so only frames whose copy is still held
 *   behind the delay timeline see it (2 to 5 of 8 measured).
 *
 * Built once per lane (VMAFX_VK_LANE). Needs the lane's device and a Vulkan
 * driver on its GPU (77 without one). On the gfx1036 a real arm that differs
 * is repeated (up to 3 times, printed): the platform drops commands now and
 * then (T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_vulkan_test_util.h"

#if VMAFX_VK_LANE == 2
#include "vmafx_sycl_producer.h"
#elif VMAFX_VK_LANE == 4
#include <hip/hip_runtime_api.h>

#include "hip/hip_handle.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 1920u
#define H 1080u
#define N_FRAMES 8u
/* The helper opens each gate this long after the previous one. */
#define GATE_MS 8u
/* How long the readers of a release-arm frame are held back. */
#define HOLD_US 20000u
/* How long the release arm waits, between a frame's two submits, for the
 * canary an early release lets through (the real release never does). */
#define CANARY_WAIT_NS 100000000ull
/* Frames the planted early release must make bad (see the release arm). */
#define RELEASE_PLANTED_MIN (VK_DEVICE_SEMAPHORES ? 1u : N_FRAMES)
/* The release arm opens frame i's delay GATE_MS after its submission, or
 * this long after the previous one at most. */
#define OPEN_WAIT_MS 2000u
/* Repeats of a real arm that differs (dropped commands on the gfx1036). */
#define MAX_RERUNS 3u
/* SYCL / HIP: 64 MiB scratch copies the producer queues before each write,
 * so the write is still running when the import is made (a host-signalled
 * gate would make the sync_file export wait for it, Mesa's threaded submit). */
#define BUSY_COPIES 24u

static VkGpu gpu;
static bool have_gpu;
static uint8_t (*ref_data)[W * H * 3u / 2u];
static uint8_t (*dist_data)[W * H * 3u / 2u];
static VmafxFrameDesc desc;
static uint8_t *canary;
static unsigned reruns;
/* Imports of the last acquire arm that returned before the producer's write
 * had finished (0: the platform waited for the writer at import). */
static unsigned imports_ahead;

static const VcCell psnr = {"psnr", "psnr", NULL, 0u};

static void sleep_ms(unsigned ms)
{
    const struct timespec t = {.tv_sec = 0, .tv_nsec = (long)ms * 1000000L};
    (void)nanosleep(&t, NULL);
}

/* ---- The gate helper ---------------------------------------------------------------- */

typedef struct Opener {
    pthread_t thread;
    uint64_t from; /* gate values from+1 .. from+n */
    unsigned n;
    VkpFrame *delay;       /* its timeline signalled to i + 1 with gate i, or NULL */
    atomic_uint submitted; /* frames the test thread submitted */
    bool after_submit;     /* open gate i GATE_MS after frame i - 1 was submitted */
    bool running;
} Opener;

/* Wait until frame `i` was submitted, at most OPEN_WAIT_MS (an engine that
 * waits for an earlier frame inside the submit is never stuck behind a
 * gate). */
static void wait_submitted(Opener *o, unsigned i)
{
    for (unsigned ms = 0; atomic_load(&o->submitted) <= i && ms < OPEN_WAIT_MS; ms++) {
        sleep_ms(1u);
    }
}

static void *opener_main(void *arg)
{
    Opener *const o = arg;
    for (unsigned i = 1; i <= o->n; i++) {
        if (o->after_submit) {
            wait_submitted(o, i - 1u);
        }
        sleep_ms(GATE_MS);
        (void)vkp_gate_signal(gpu.vk, o->from + i);
        if (o->delay) {
            (void)vkp_frame_signal_host(o->delay, i + 1u);
        }
    }
    return NULL;
}

static void opener_start(Opener *o, uint64_t from, unsigned n, VkpFrame *delay)
{
    o->from = from;
    o->n = n;
    o->delay = delay;
    atomic_init(&o->submitted, 0u);
    o->after_submit = delay != NULL;
    o->running = pthread_create(&o->thread, NULL, opener_main, o) == 0;
}

static void opener_join(Opener *o)
{
    if (o->running) {
        (void)pthread_join(o->thread, NULL);
    }
    o->running = false;
}

/* The gate's next free value (each arm uses values past the last one). */
static uint64_t gate_base;

/* ---- Frames ----------------------------------------------------------------------------- */

/* A frame holding the canary (written and finished), its timeline at 1. */
static bool canary_frame(VkFrame *f)
{
    const VkpFrameDesc fd = vk_frame_desc(&desc, VMAFX_PIXEL_FORMAT_NV12, &vk_layouts[0]);
    f->sync = -1;
    f->f = vkp_frame_new(gpu.vk, &fd);
    const VkpWrite w = {.packed = canary, .wait_gate = 0u, .wait_self = 0u, .signal = 1u};
    const bool ok = f->f && vkp_frame_write(f->f, &w) == 0 && vkp_frame_wait(f->f, 1u, ~0ull) == 1;
    if (ok && !VK_DEVICE_SEMAPHORES) {
        const int stale = vkp_frame_sync_file(f->f);
        if (stale >= 0) {
            (void)close(stale);
        }
    }
    return ok;
}

/* Queue the write of `planar`, signalling 2: behind gate `gate` (0: none)
 * and `busy` scratch copies. */
static bool gated_write(VkFrame *f, const uint8_t *planar, uint64_t gate, uint32_t busy)
{
    uint8_t *const packed = vk_pack(&desc, planar, VMAFX_PIXEL_FORMAT_NV12);
    const VkpWrite w = {
        .packed = packed, .wait_gate = gate, .wait_self = 0u, .signal = 2u, .busy = busy};
    const bool ok = packed && vkp_frame_write(f->f, &w) == 0;
    free(packed);
    if (ok && !VK_DEVICE_SEMAPHORES) {
        f->sync = vkp_frame_sync_file(f->f);
    }
    return ok && (VK_DEVICE_SEMAPHORES || f->sync >= 0);
}

/* Import a frame whose last write signalled `value`. With `further` (CUDA)
 * the timeline is handed over as the last further acquire fence instead, so
 * a lane that skips `acquire_more` reads unfinished frames. */
static VmafxFrame *import_at(VmafxContext *context, VkFrame *f, uint64_t value, bool further)
{
    VmafxFrameImport imp;
    if (!vk_describe(f, value, &imp)) {
        return NULL;
    }
    if (further && VK_DEVICE_SEMAPHORES) {
        imp.acquire_more[1] = imp.acquire;
        imp.acquire.kind = VMAFX_FENCE_NONE;
        imp.acquire.fd = -1;
    }
    VmafxFrame *frame = NULL;
    const VmafxStatus status =
        vmafx_context_import_frame(context, gpu.device, &imp, "main", &frame, NULL);
    if (!VK_DEVICE_SEMAPHORES) {
        imp.acquire.kind = VMAFX_FENCE_NONE;
    }
    if (imp.acquire_more[1].kind == VMAFX_FENCE_VULKAN_SEMAPHORE) {
        (void)close(imp.acquire_more[1].fd);
    }
    vkp_import_close(&imp);
    return status == VMAFX_OK ? frame : NULL;
}

/* ---- Scores ------------------------------------------------------------------------------ */

static VmafxContext *run_host(void)
{
    VmafxContext *const context = vc_cell_context(gpu.device, &psnr);
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = vmafx_submit(context, vt_wrap_frame(&desc, ref_data[i], NULL),
                          vt_wrap_frame(&desc, dist_data[i], NULL), i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* Frames of `run` whose psnr differs from the host session's; N_FRAMES + 1
 * when a session failed. */
static unsigned bad_frames(VmafxContext *run)
{
    VmafxContext *const host = run ? run_host() : NULL;
    unsigned bad = host ? 0u : N_FRAMES + 1u;
    for (unsigned i = 0; i < N_FRAMES && host; i++) {
        VmafxScore a = VMAFX_SCORE_INIT;
        VmafxScore b = VMAFX_SCORE_INIT;
        const bool ok = vmafx_feature_score(host, "psnr_y", i, &a, NULL) == VMAFX_OK &&
                        vmafx_feature_score(run, "psnr_y", i, &b, NULL) == VMAFX_OK;
        bad += !ok || !vt_same_bits(a.value, b.value);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    return bad;
}

/* ---- Acquire ------------------------------------------------------------------------------- */

typedef struct Pairs {
    VkFrame ref[N_FRAMES];
    VkFrame dist[N_FRAMES];
} Pairs;

static void free_pairs(Pairs *p)
{
    for (unsigned i = 0; i < N_FRAMES; i++) {
        if (p->ref[i].f) {
            vk_frame_free(&p->ref[i]);
        }
        if (p->dist[i].f) {
            vk_frame_free(&p->dist[i]);
        }
    }
}

static bool make_pairs(Pairs *p)
{
    memset(p, 0, sizeof(*p));
    bool ok = true;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = canary_frame(&p->ref[i]) && canary_frame(&p->dist[i]);
    }
    return ok;
}

/* Queue the writes of frame i: CUDA behind gate value base + i + 1, which
 * the helper opens after the frame was imported; SYCL / HIP behind busy
 * work, the write's sync_file waited on by the import rule. */
static bool write_frame(Pairs *p, uint64_t base, unsigned i)
{
    const uint32_t busy = VK_DEVICE_SEMAPHORES ? 0u : BUSY_COPIES;
    const uint64_t gate = VK_DEVICE_SEMAPHORES ? base + i + 1u : 0u;
    return gated_write(&p->ref[i], ref_data[i], gate, busy) &&
           gated_write(&p->dist[i], dist_data[i], gate, busy);
}

/* The fenced session: CUDA queues every write first, SYCL / HIP queue each
 * frame's writes right before its import (a write queued behind earlier
 * busy work would land long after its own import is due). */
static VmafxContext *run_gated(Pairs *p, uint64_t base)
{
    VmafxContext *const context = vc_cell_context(gpu.device, &psnr);
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok && VK_DEVICE_SEMAPHORES; i++) {
        ok = write_frame(p, base, i);
    }
    Opener opener;
    opener_start(&opener, base, N_FRAMES, NULL);
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = VK_DEVICE_SEMAPHORES || write_frame(p, base, i);
        /* The reference is imported first: its copy is queued before any
         * wait of the distorted frame could cover it. */
        VmafxFrame *const r = ok ? import_at(context, &p->ref[i], 2u, true) : NULL;
        VmafxFrame *const d = r ? import_at(context, &p->dist[i], 2u, false) : NULL;
        imports_ahead += r && vkp_frame_value(p->ref[i].f) < 2u;
        imports_ahead += d && vkp_frame_value(p->dist[i].f) < 2u;
        ok = r && d && vmafx_submit(context, r, d, i, NULL) == VMAFX_OK;
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    opener_join(&opener);
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    return ok ? context : NULL;
}

static unsigned acquire_arm(uint32_t switches)
{
    imports_ahead = 0u;
    Pairs p;
    const bool made = make_pairs(&p);
    const uint64_t base = gate_base;
    gate_base += N_FRAMES;
    vmafx_test_set_switches(switches);
    VmafxContext *const run = made ? run_gated(&p, base) : NULL;
    vmafx_test_set_switches(0u);
    (void)vkp_gate_signal(gpu.vk, gate_base); /* every write of the arm may finish */
    const unsigned bad = bad_frames(run);
    if (run) {
        (void)vmafx_context_destroy(run, NULL);
    }
    free_pairs(&p);
    return bad;
}

/* A real arm, repeated on the gfx1036 while it differs (printed). */
static unsigned real_arm(unsigned (*arm)(uint32_t))
{
    unsigned bad = arm(0u);
    for (unsigned r = 0; r < MAX_RERUNS && bad != 0u && VMAFX_VK_LANE == 4; r++) {
        (void)fprintf(stderr, "[real arm: %u bad, repeated (dropped commands?)] ", bad);
        reruns++;
        bad = arm(0u);
    }
    return bad;
}

static char *test_acquire_order(void)
{
    if (!have_gpu) {
        return NULL;
    }
    const unsigned planted = acquire_arm(VMAFX_TEST_SKIP_ACQUIRE_WAIT);
    const unsigned ahead = imports_ahead;
    const unsigned real = real_arm(acquire_arm);
    (void)fprintf(stderr,
                  "[acquire: skipped wait %u bad (%u imports ahead of their write), with the "
                  "wait %u bad of %u] ",
                  planted, ahead, real, N_FRAMES);
    /* The skipped wait must be seen where no import waited for its writer:
     * CUDA, whose import never waits, and any arm whose every import ran
     * ahead of its write. Where the platform ordered every import behind its
     * writer (the gfx1036 under ROCm 10.1,
     * T-HIP-DMABUF-IMPORT-WAITS-FOR-WRITER-2026-10-06) no frame can read
     * early. Between the two, a frame imported ahead of its write may or may
     * not read the canary: printed, not judged. */
    if (VK_DEVICE_SEMAPHORES || ahead == 2u * N_FRAMES) {
        mu_assert("the skipped wait is seen", planted > 0u && planted <= N_FRAMES);
    } else if (ahead == 0u) {
        (void)fprintf(stderr, "[the platform waited for every writer at import] ");
        mu_assert("no frame read early", planted == 0u);
    } else {
        (void)fprintf(stderr, "[%u of %u imports ran ahead of their write] ", ahead, 2u * N_FRAMES);
    }
    mu_assert("with the wait every frame scores as the host frame", real == 0u);
    return NULL;
}

/* ---- Release ------------------------------------------------------------------------------ */

#if VMAFX_VK_LANE == 2
static VsProducer *holder;
#elif VMAFX_VK_LANE == 4
static hipStream_t holder; /* the HIP device's library stream (external[0]) */

static void hold_stream(void *arg)
{
    (void)arg;
    const struct timespec t = {.tv_sec = 0, .tv_nsec = (long)HOLD_US * 1000L};
    (void)nanosleep(&t, NULL);
}
#endif

/* The fence that holds the readers of frame i back. CUDA: a further acquire
 * fence on the delay timeline `delay` (value i + 2, its gated writes opened
 * by the helper); SYCL / HIP: an event behind work holding the producer's
 * queue or stream. Frame 0 is not held: the engine's first submission waits
 * for its frame on the host (setting up its device state). */
static bool hold_fence(VkFrame *delay, unsigned i, VmafxFence *out)
{
    const VmafxFence init = VMAFX_FENCE_INIT;
    *out = init;
    out->fd = -1;
    if (i == 0u) {
        return true;
    }
#if VMAFX_VK_LANE == 1
    return vkp_frame_fence(delay->f, i + 2u, out) == 0;
#elif VMAFX_VK_LANE == 2
    (void)delay;
    (void)i;
    out->kind = VMAFX_FENCE_SYCL_EVENT;
    out->handle = vs_hold_device(holder, HOLD_US) == 0 ? vs_last_event(holder) : 0u;
    return out->handle != 0u;
#else
    /* HIP: the readers are held where they run instead (hold_readers()): a
     * HIP import synchronises the device, so a hold queued before it would
     * be over by the time the frame is submitted (measured). */
    (void)delay;
    (void)i;
    return true;
#endif
}

static void free_hold(VmafxFence *f)
{
    if (f->kind == VMAFX_FENCE_NONE) {
        return;
    }
#if VMAFX_VK_LANE == 1
    if (f->fd >= 0) {
        (void)close(f->fd);
    }
#elif VMAFX_VK_LANE == 2
    vs_event_free(f->handle);
#endif
}

/* HIP: hold the library stream (the device was made from the test's
 * stream) before frame i's submit, so the twins' copies, which the submit
 * enqueues there, wait. hipStreamQuery() submits the hold (the gfx1036 runs
 * unsubmitted work out of order, T-HIP-GFX1036-UNFLUSHED-STREAM-ORDER). */
static bool hold_readers(unsigned i)
{
#if VMAFX_VK_LANE == 4
    return i == 0u || (hipLaunchHostFunc(holder, hold_stream, NULL) == hipSuccess &&
                       hipStreamQuery(holder) != hipErrorInvalidHandle);
#else
    (void)i;
    return true;
#endif
}

/* The producer's side of a release: the frame's release signals its
 * timeline to 3 (CUDA: on the device; SYCL / HIP: a host thread waits on the
 * HOST release fence and signals), and a canary write waits for 3. */
typedef struct Releaser {
    pthread_t thread;
    VkFrame *frames[2u * N_FRAMES];
    VmafxFence host[2u * N_FRAMES];
    unsigned n;                 /* the test thread's count of armed host fences */
    atomic_uint published;      /* `n` as the releaser may read it */
    VmafxFence holds[N_FRAMES]; /* kept until the frames are read: a stream or
                                 * queue may still wait on them */
    bool running;
} Releaser;

/* Wait until host fence `k` was armed (published), at most OPEN_WAIT_MS. */
static bool armed(Releaser *r, unsigned k)
{
    for (unsigned ms = 0; atomic_load(&r->published) <= k && ms < OPEN_WAIT_MS; ms++) {
        sleep_ms(1u);
    }
    return atomic_load(&r->published) > k;
}

/* The producer's host thread: as each frame's HOST release fence is
 * signalled, signal its timeline to 3 so its canary write may run. It runs
 * while the frames are submitted (an early release is acted on at once). */
static void *releaser_main(void *arg)
{
    Releaser *const r = arg;
    for (unsigned k = 0; k < 2u * N_FRAMES && armed(r, k); k++) {
        if (vmafx_fence_wait(&r->host[k], ~0ull, NULL) == VMAFX_OK) {
            (void)vkp_frame_signal_host(r->frames[k]->f, 3u);
        }
    }
    return NULL;
}

/* Arm the release of `frame` (Vulkan frame `f`, timeline at 2) and queue the
 * canary write behind it. */
static bool arm_release(Releaser *r, VmafxFrame *frame, VkFrame *f)
{
    bool ok = true;
    if (VK_DEVICE_SEMAPHORES) {
        VmafxFence sem = VMAFX_FENCE_INIT;
        ok = vkp_frame_fence(f->f, 3u, &sem) == 0 &&
             vmafx_frame_signal_on_release(frame, &sem, NULL) == VMAFX_OK;
        if (sem.fd >= 0) {
            (void)close(sem.fd);
        }
    } else {
        const VmafxFence init = VMAFX_FENCE_INIT;
        r->frames[r->n] = f;
        r->host[r->n] = init;
        ok = vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &r->host[r->n], NULL) == VMAFX_OK;
        r->n += ok ? 1u : 0u;
        atomic_store(&r->published, r->n);
    }
    const VkpWrite w = {.packed = canary, .wait_gate = 0u, .wait_self = 3u, .signal = 4u};
    return ok && vkp_frame_write(f->f, &w) == 0;
}

/* Every frame written for real and finished (timelines at 2), before any
 * canary write is queued: the canaries wait for releases, and a write queued
 * behind them on the producer's queue would wait for them too. */
static bool write_pairs(Pairs *p)
{
    bool ok = true;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = gated_write(&p->ref[i], ref_data[i], 0u, 0u) &&
             gated_write(&p->dist[i], dist_data[i], 0u, 0u) &&
             vkp_frame_wait(p->ref[i].f, 2u, ~0ull) == 1 &&
             vkp_frame_wait(p->dist[i].f, 2u, ~0ull) == 1;
    }
    return ok;
}

/* Submit frame pair i to both contexts: the second once the first submit
 * returned and an early release, if any, let the canaries land. Each submit
 * takes one reference of each frame (it releases them on every path). */
static bool submit_twice(VmafxContext *const contexts[2], VmafxFrame *const frames[2],
                         VkFrame *const fs[2], unsigned i)
{
    (void)vmafx_frame_ref(frames[0]);
    (void)vmafx_frame_ref(frames[1]);
    const bool first = vmafx_submit(contexts[0], frames[0], frames[1], i, NULL) == VMAFX_OK;
    if (vkp_frame_wait(fs[0]->f, 4u, CANARY_WAIT_NS) == 1) {
        (void)vkp_frame_wait(fs[1]->f, 4u, CANARY_WAIT_NS);
    }
    return vmafx_submit(contexts[1], frames[0], frames[1], i, NULL) == VMAFX_OK && first;
}

/* Frame i (written): imported with its readers held, its release armed. */
static bool release_frame(VmafxContext *const contexts[2], Pairs *p, VkFrame *delay, Releaser *r,
                          unsigned i)
{
    VmafxFence *const hold_slot = &r->holds[i];
    VkFrame *const fs[2] = {&p->ref[i], &p->dist[i]};
    VmafxFrame *frames[2] = {NULL, NULL};
    bool ok = hold_fence(delay, i, hold_slot);
    const VmafxFence hold = *hold_slot;
    for (unsigned s = 0; s < 2u && ok; s++) {
        VmafxFrameImport imp;
        ok = vk_describe(fs[s], 2u, &imp);
        if (ok) {
            imp.acquire_more[0] = hold; /* CUDA */
            if (!VK_DEVICE_SEMAPHORES) {
                imp.acquire = hold; /* SYCL / HIP: the event replaces the sync_file */
                imp.acquire_more[0].kind = VMAFX_FENCE_NONE;
            }
            ok = vmafx_frame_import(gpu.device, &imp, &frames[s], NULL) == VMAFX_OK;
            imp.acquire.kind = VK_DEVICE_SEMAPHORES ? imp.acquire.kind : VMAFX_FENCE_NONE;
            vkp_import_close(&imp);
        }
        ok = ok && arm_release(r, frames[s], fs[s]);
    }
    if (!ok || !hold_readers(i)) {
        vmafx_frame_unref(frames[0]);
        vmafx_frame_unref(frames[1]);
        return false;
    }
    return submit_twice(contexts, frames, fs, i);
}

/* CUDA's delay timeline: an exportable timeline the helper signals from the
 * host (value i + 2 for frame i), with no queue submission that the canary
 * writes would queue behind. */
static bool delay_timeline(VkFrame *delay)
{
    if (!VK_DEVICE_SEMAPHORES) {
        return true;
    }
    const VmafxFrameDesc small = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, 16u, 16u);
    const VkpFrameDesc fd = vk_frame_desc(&small, VMAFX_PIXEL_FORMAT_NV12, &vk_layouts[0]);
    delay->sync = -1;
    delay->f = vkp_frame_new(gpu.vk, &fd);
    return delay->f != NULL;
}

static unsigned release_arm(uint32_t switches)
{
    Pairs p;
    VkFrame delay;
    memset(&delay, 0, sizeof(delay));
    Releaser r;
    memset(&r, 0, sizeof(r));
    const uint64_t base = gate_base;
    gate_base += N_FRAMES;
    VmafxContext *const contexts[2] = {vc_cell_context(gpu.device, &psnr),
                                       vc_cell_context(gpu.device, &psnr)};
    bool ok =
        contexts[0] && contexts[1] && make_pairs(&p) && write_pairs(&p) && delay_timeline(&delay);
    Opener opener;
    opener_start(&opener, base, N_FRAMES, VK_DEVICE_SEMAPHORES ? delay.f : NULL);
    atomic_init(&r.published, 0u);
    r.running =
        ok && !VK_DEVICE_SEMAPHORES && pthread_create(&r.thread, NULL, releaser_main, &r) == 0;
    vmafx_test_set_switches(switches);
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = release_frame(contexts, &p, &delay, &r, i);
        atomic_store(&opener.submitted, i + 1u);
    }
    atomic_store(&opener.submitted, N_FRAMES);
    ok = ok && vmafx_flush(contexts[0], NULL) == VMAFX_OK &&
         vmafx_flush(contexts[1], NULL) == VMAFX_OK;
    vmafx_test_set_switches(0u);
    opener_join(&opener);
    (void)vkp_gate_signal(gpu.vk, gate_base);
    const unsigned bad_first = ok ? bad_frames(contexts[0]) : N_FRAMES + 1u;
    const unsigned bad_second = ok ? bad_frames(contexts[1]) : N_FRAMES + 1u;
    const unsigned bad = bad_first > bad_second ? bad_first : bad_second;
    for (unsigned c = 0; c < 2u; c++) {
        if (contexts[c]) {
            (void)vmafx_context_destroy(contexts[c], NULL); /* releases every frame */
        }
    }
    if (r.running) {
        (void)pthread_join(r.thread, NULL);
    }
    for (unsigned k = 0; k < r.n; k++) {
        (void)vmafx_fence_destroy(&r.host[k], NULL);
    }
    for (unsigned i = 0; i < N_FRAMES; i++) {
        free_hold(&r.holds[i]);
    }
    if (delay.f) {
        vk_frame_free(&delay);
    }
    free_pairs(&p);
    return bad;
}

static char *test_release_canary(void)
{
    if (!have_gpu) {
        return NULL;
    }
    const unsigned planted = release_arm(VMAFX_TEST_EARLY_RELEASE);
    const unsigned real = real_arm(release_arm);
    (void)fprintf(stderr, "[release: early %u bad, real %u bad of %u] ", planted, real, N_FRAMES);
    mu_assert("the early release is seen", planted >= RELEASE_PLANTED_MIN && planted <= N_FRAMES);
    mu_assert("with the real release every frame scores as the host frame", real == 0u);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    return NULL;
}

/* ---- Setup ----------------------------------------------------------------------------------- */

static bool open_holder(void)
{
#if VMAFX_VK_LANE == 2
    holder = vs_producer_open();
    return holder != NULL;
#elif VMAFX_VK_LANE == 4
    /* The device is made again from the test's stream, so the test can hold
     * its library stream. */
    VmafxDeviceDesc d = VMAFX_DEVICE_DESC_INIT;
    d.backend = VMAFX_BACKEND_HIP;
    if (hipSetDevice(0) != hipSuccess || hipStreamCreate(&holder) != hipSuccess) {
        return false;
    }
    d.external[0] = vmaf_hip_stream_bits(holder);
    vmafx_device_unref(gpu.device);
    gpu.device = NULL;
    return vmafx_device_create(&d, &gpu.device, NULL) == VMAFX_OK;
#else
    return true;
#endif
}

static void close_holder(void)
{
#if VMAFX_VK_LANE == 2
    vs_producer_close(holder);
#elif VMAFX_VK_LANE == 4
    vmafx_device_unref(gpu.device); /* before the stream it was made from */
    gpu.device = NULL;
    (void)hipStreamDestroy(holder);
#endif
}

char *run_tests(void)
{
    desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    ref_data = malloc(sizeof(*ref_data) * N_FRAMES);
    dist_data = malloc(sizeof(*dist_data) * N_FRAMES);
    canary = malloc(vt_frame_bytes(&desc));
    if (!ref_data || !dist_data || !canary) {
        return "no memory";
    }
    for (unsigned i = 0; i < N_FRAMES; i++) {
        vt_fill(&desc, ref_data[i], 2u * i + 1u);
        vt_fill(&desc, dist_data[i], 7u * i + 5u);
    }
    memset(canary, 0x5a, vt_frame_bytes(&desc));
    vmafx_test_reset_counters();
    have_gpu = vk_open(&gpu) && open_holder();
    gate_base = 0u;
    mu_skipped = !have_gpu; /* no case runs, so none reports a pass (Q-346) */
    static const MuTest tests[] = {
        MU_TEST(test_acquire_order),
        MU_TEST(test_release_canary),
    };
    char *const msg = have_gpu ? mu_run_table(tests, MU_TABLE_LEN(tests)) : NULL;
    if (have_gpu) {
        (void)fprintf(stderr, "[%u repeated arms] ", reruns);
    }
    if (have_gpu) {
        close_holder();
    }
    vk_close(&gpu);
    free(ref_data);
    free(dist_data);
    free(canary);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
