/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Imported frames on a SYCL device (RC4 WP3, ADR-1852 design section 2.7,
 * ADR-1929, ADR-2091).
 *
 * USM pointers of the device's context and linear dma-buf planes are bound
 * where they are, for any address and pitch: the engine's readers copy rows
 * of a device picture on the device (vmaf_sycl_picture_read_plane()), so no
 * reader assumes an alignment. NV12 / P010 / P016 are planarised on the
 * device into planes of the frame's own (vmafx_sycl_rt.cpp: a de-interleave
 * and the P010 shift, nothing else), Intel-tiled dma-buf planes are
 * de-tiled there (detile.h, the shift fused), so an imported frame scores
 * bit for bit as the same frame uploaded from the host. Nothing is ever
 * copied through the host (the planted VMAFX_TEST_FORCE_HOST_COPY defect
 * does, and counts it). VMAFX_IMPORT_ALLOW_COPY changes nothing on SYCL: no
 * layout needs a copy.
 *
 * Every conversion runs on the device's library queue behind the acquire
 * fence (a SYCL event becomes a barrier there); the frame's ready event is
 * the last of them, and every reader waits on it. HOST, SYNC_FILE and
 * GL_SYNC acquire fences are checked on the host: signalled, or VMAFX_E_BUSY
 * for the D8 retry.
 */

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "common.h"
#include "log.h"
#include "picture.h"
#include "ref.h"
#include "vmafx/error_internal.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/internal.h"
#include "vmafx/sync_object.h"
#include "vmafx/vmafx.h"
#include "vmafx_sycl.h"
#include "vmafx_sycl_internal.h"
#include "vmafx_sycl_rt.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The memory kinds a SYCL device binds besides USM, for messages. */
#ifdef __linux__
#define VMAFX_SYCL_LINUX_MEMORY ", DMABUF and GL_TEXTURE memory"
#else
#define VMAFX_SYCL_LINUX_MEMORY ""
#endif

/* Longest host wait on the implicit fences an EGL export of GL textures
 * leaves on the dma-buf (1 s). */
#define VMAFX_SYCL_GL_EXPORT_WAIT_NS 1000000000ull

/* Row alignment of the planes an import allocates. */
#define VMAFX_SYCL_PITCH_ALIGN 64u

/* Where each plane of the frame comes from. */
typedef struct SyclPlan {
    unsigned pw[3]; /* planar geometry of the frame */
    unsigned ph[3];
    uint32_t n_out;   /* planes of the frame: 1 (YUV400P) or 3 */
    unsigned bytes;   /* per sample */
    bool owned[3];    /* plane i is in the frame's own allocation */
    bool staged[3];   /* ... the planted host copy of a bound plane */
    size_t offset[3]; /* its offset there */
    size_t pitch[3];  /* its pitch */
    size_t staging;   /* tiled interleaved chroma de-tiled here; SIZE_MAX: none */
    size_t staging_pitch;
    size_t total; /* bytes of the allocation */
} SyclPlan;

const char *vmafx_sycl_refusal(const char *extractor)
{
    /* Every SYCL twin reads a SYCL device picture: the shared luma and
     * chroma planes and the planes a twin stages itself are copied on the
     * device from it (ADR-2091 item 3, an audit of every twin). */
    (void)extractor;
    return NULL;
}

/* ---- Frame state --------------------------------------------------------------- */

VmafxSyclFrame *vmafx_sycl_frame_state_new(VmafxSyclDevice *dev)
{
    assert(dev != NULL && dev->rt != NULL);
    VmafxSyclFrame *const sf = calloc(1, sizeof(*sf));
    if (!sf) {
        return NULL;
    }
    if (pthread_mutex_init(&sf->lock, NULL) != 0) {
        free(sf);
        return NULL;
    }
    sf->rt = vmafx_sycl_rt_frame_new(dev->rt);
    if (!sf->rt) {
        (void)pthread_mutex_destroy(&sf->lock);
        free(sf);
        return NULL;
    }
    sf->dev = dev;
    for (unsigned i = 0; i < 3u; i++) {
        sf->fds[i] = -1;
    }
    return sf;
}

/* Close the frame's duplicated dma-buf descriptors. */
static void close_fds(VmafxSyclFrame *sf)
{
    for (unsigned i = 0; i < 3u; i++) {
#ifndef _WIN32
        if (sf->fds[i] >= 0) {
            (void)close(sf->fds[i]);
        }
#endif
        sf->fds[i] = -1;
    }
}

/* The runtime's host task signals a HOST release fence through this. */
static void signal_host_fence(void *arg)
{
    vmafx_host_fence_signal_unref(arg);
}

/* What the runtime frees behind a frame's release. */
static VmafxSyclRelease release_of(const VmafxSyclFrame *sf, VmafxHostFence *fence)
{
    VmafxSyclRelease r = {.signal = fence ? signal_host_fence : NULL,
                          .arg = fence,
                          .slot = sf->release_slot,
                          .owned = sf->owned,
                          .imports = {sf->imports[0], sf->imports[1], sf->imports[2]}};
    return r;
}

void vmafx_sycl_frame_state_discard(VmafxSyclFrame *sf)
{
    if (!sf) {
        return;
    }
    const VmafxSyclRelease r = release_of(sf, NULL);
    vmafx_sycl_rt_frame_discard(sf->rt, &r);
    close_fds(sf);
    (void)pthread_mutex_destroy(&sf->lock);
    free(sf);
}

void vmafx_sycl_picture_attach(VmafPicture *pic, VmafxSyclFrame *sf)
{
    VmafPicturePrivate *const priv = pic->priv;
    assert(priv != NULL && sf != NULL && sf->rt != NULL);
    priv->buf_type = VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE;
    priv->sycl.frame = sf->rt;
}

int vmafx_sycl_frame_release(VmafxFrame *frame, VmafPicture *pic)
{
    VmafxSyclFrame *const sf = frame->lane;
    assert(sf != NULL && sf->rt != NULL && pic != NULL);
    VmafxHostFence *const fence = atomic_exchange(&frame->released, (VmafxHostFence *)NULL);
    frame->lane = NULL;
    VmafPicturePrivate *const priv = pic->priv;
    if (priv) {
        priv->sycl.frame = NULL;
    }
    (void)pthread_mutex_lock(&sf->lock);
    VmafxSyclRelease r = release_of(sf, fence);
    r.idle_slot = frame->pool ? vmafx_sycl_pool_idle_slot(frame) : 0u;
    const uint32_t slot = sf->release_slot;
    sf->release_slot = 0;
    (void)pthread_mutex_unlock(&sf->lock);
    /* Records the release event into the slot, signals the host fence and
     * frees the planes behind the last reader; frees the runtime frame. */
    const int err = vmafx_sycl_rt_frame_release(sf->rt, &r);
    sf->rt = NULL;
    if (slot) {
        vmafx_sycl_rt_slot_unref(slot); /* the frame's reference */
    }
    close_fds(sf);
    (void)pthread_mutex_destroy(&sf->lock);
    free(sf);
    return err;
}

/* ---- Checks ------------------------------------------------------------------- */

static VmafxStatus check_sycl_memory(const VmafxReport *report, const VmafxFrameImport *d,
                                     const VmafxImportLayout *layout)
{
    switch (d->memory) {
    case VMAFX_MEMORY_DEVICE_POINTER:
#ifdef __linux__
    case VMAFX_MEMORY_DMABUF:
    case VMAFX_MEMORY_GL_TEXTURE:
#endif
        return VMAFX_OK;
    case VMAFX_MEMORY_WIN32_SHARED:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend sycl, memory WIN32_SHARED, pixel format %s: Windows shared "
                          "textures are not imported by this build (ADR-2091: the Level Zero "
                          "external-memory path is prepared, not shipped, until a Windows "
                          "device runs its tests); never copied through the host",
                          layout->name);
    default:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend sycl, memory %s, pixel format %s: a SYCL device binds "
                          "DEVICE_POINTER (USM) memory" VMAFX_SYCL_LINUX_MEMORY " (host frames "
                          "go to vmafx_frame_wrap_host(); a host copy is never made)",
                          vmafx_memory_kind_name(d->memory), layout->name);
    }
}

/* Plane `i` of USM memory: linear, inside the address space, and a USM
 * allocation of the device's context. */
static VmafxStatus check_pointer_plane(const VmafxReport *report, VmafxSyclDevice *dev,
                                       const VmafxImportPlane *p, uint32_t i, uint64_t row,
                                       uint64_t rows)
{
    const VmafxStatus status = vmafx_import_check_linear_plane(report, p, i, row, rows, "SYCL USM");
    if (status != VMAFX_OK) {
        return status;
    }
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): a USM pointer crosses the ABI as uintptr_t (VmafxImportPlane.handle, ADR-1929). */
    const void *const ptr = (const void *)p->handle;
    if (vmafx_sycl_rt_pointer_kind(dev->rt, ptr) != VMAFX_SYCL_POINTER_UNKNOWN) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PLANE,
                      vmafx_import_plane_field(i, "handle"),
                      "plane %u: 0x%llx is no USM allocation of the device's SYCL context "
                      "(allocate it on a queue of that context, or create the device from the "
                      "producer's queue)",
                      (unsigned)i, (unsigned long long)p->handle);
}

static VmafxStatus check_pointer_planes(const VmafxReport *report, VmafxSyclDevice *dev,
                                        const VmafxFrameImport *d, const VmafxImportLayout *layout)
{
    unsigned pw[3];
    unsigned ph[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, pw, ph);
    for (uint32_t i = 0; i < layout->n_planes; i++) {
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(layout, d->bpc, i, pw, ph, &row, &rows);
        const VmafxStatus status = check_pointer_plane(report, dev, &d->plane[i], i, row, rows);
        if (status != VMAFX_OK) {
            return status;
        }
    }
    return VMAFX_OK;
}

/* A HOST acquire fence: signalled (or the planted skipped wait), else
 * VMAFX_E_BUSY for the D8 retry. */
static VmafxStatus host_acquire(const VmafxReport *report, const VmafxFence *acquire)
{
    VmafxHostFence *host = NULL;
    const VmafxStatus status = vmafx_host_fence_of(report, acquire, "desc.acquire.handle", &host);
    if (status != VMAFX_OK || vmafx_host_fence_signalled(host) ||
        vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT)) {
        return status;
    }
    return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "desc.acquire",
                      "the producer has not signalled the HOST acquire fence; the SYCL device "
                      "waits on SYCL_EVENT fences on its queue (vmafx_context_import_frame() "
                      "waits and retries once)");
}

/* The acquire fence is one the SYCL device honours: a SYCL event (a barrier
 * on the library queue), a host fence, sync_file or GL sync already
 * signalled, or none. */
static VmafxStatus check_sycl_acquire(const VmafxReport *report, const VmafxFrameImport *d)
{
    const VmafxFence *const acquire = &d->acquire;
    switch (acquire->kind) {
    case VMAFX_FENCE_NONE:
        return VMAFX_OK;
    case VMAFX_FENCE_SYCL_EVENT:
        return acquire->handle ? VMAFX_OK :
                                 VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE,
                                            "desc.acquire.handle",
                                            "backend sycl: a SYCL_EVENT acquire fence without "
                                            "its event");
    case VMAFX_FENCE_HOST:
        return host_acquire(report, acquire);
    case VMAFX_FENCE_SYNC_FILE:
        return vmafx_sync_file_acquire(report, acquire, "sycl");
    case VMAFX_FENCE_GL_SYNC:
        if (d->memory != VMAFX_MEMORY_GL_TEXTURE) {
            return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "desc.acquire.kind",
                              "backend sycl: a GL sync orders GL_TEXTURE imports only");
        }
        return vmafx_gl_sync_acquire(report, acquire, "sycl");
    default:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "desc.acquire.kind",
                          "backend sycl: an acquire fence of kind %u; the SYCL device waits on "
                          "NONE, HOST, SYCL_EVENT, SYNC_FILE and GL_SYNC fences",
                          (unsigned)acquire->kind);
    }
}

/* ---- Planning ----------------------------------------------------------------- */

static size_t pitch_of(unsigned samples, unsigned bytes)
{
    const size_t row = (size_t)samples * bytes;
    return (row + VMAFX_SYCL_PITCH_ALIGN - 1u) & ~(size_t)(VMAFX_SYCL_PITCH_ALIGN - 1u);
}

/* Producer plane that frame plane `i` comes from. */
static uint32_t source_plane(const VmafxImportLayout *layout, uint32_t i)
{
    return layout->interleaved && i > 0u ? 1u : i;
}

/* Which planes the frame owns: converted (semi-planar chroma, the P010
 * luma), de-tiled, or staged through the host (planted), the rest bound. */
static void plan_planes(const VmafxFrameImport *d, const VmafxImportLayout *layout,
                        const VmafxSyclSource src[3], bool force_copy, SyclPlan *plan)
{
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, plan->pw,
                               plan->ph);
    plan->n_out = layout->planar_fmt == VMAFX_PIXEL_FORMAT_YUV400P ? 1u : 3u;
    plan->bytes = d->bpc > 8u ? 2u : 1u;
    plan->total = 0;
    for (uint32_t i = 0; i < 3u; i++) {
        const bool out = i < plan->n_out;
        const bool converted = layout->shift != 0u || (i > 0u && layout->interleaved);
        const bool tiled = src[source_plane(layout, i)].tiled;
        plan->staged[i] = out && force_copy && !converted && !tiled;
        plan->owned[i] = out && (converted || tiled || plan->staged[i]);
        plan->pitch[i] = plan->owned[i] ? pitch_of(plan->pw[i], plan->bytes) : 0u;
        plan->offset[i] = plan->total;
        plan->total += plan->pitch[i] * plan->ph[i];
    }
    plan->staging = SIZE_MAX;
    plan->staging_pitch = 0;
    if (layout->interleaved && src[1].tiled) {
        plan->staging = plan->total;
        plan->staging_pitch = pitch_of(2u * plan->pw[1], plan->bytes);
        plan->total += plan->staging_pitch * plan->ph[1];
    }
}

/* ---- Device work ---------------------------------------------------------------- */

static uint8_t *source_address(const VmafxSyclSource *s)
{
    return s->base + s->offset;
}

/* The de-tile of producer plane `s` (`samples` per row) into `dst`. */
static int detile(VmafxSyclFrame *sf, const VmafxSyclSource *s, unsigned samples, unsigned rows,
                  unsigned bytes, unsigned shift, uint8_t *dst, size_t dst_pitch)
{
    const VmafxSyclPlaneOp op = {.src = source_address(s),
                                 .src_pitch = (size_t)s->pitch,
                                 .dst0 = dst,
                                 .dst1 = NULL,
                                 .dst_pitch = dst_pitch,
                                 .w = samples,
                                 .rows = rows,
                                 .bytes = bytes,
                                 .shift = shift};
    return vmafx_sycl_rt_frame_detile(sf->rt, &op, s->tiling);
}

/* Cb and Cr of the interleaved producer plane into frame planes 1, 2 (via
 * the staging rows when it is tiled). */
static int fill_chroma_pair(VmafxSyclFrame *sf, const VmafxImportLayout *layout,
                            const VmafxSyclSource *s, const SyclPlan *plan, uint8_t *base)
{
    const uint8_t *src = source_address(s);
    size_t src_pitch = (size_t)s->pitch;
    if (s->tiled) {
        const int err = detile(sf, s, 2u * plan->pw[1], plan->ph[1], plan->bytes, 0u,
                               base + plan->staging, plan->staging_pitch);
        if (err) {
            return err;
        }
        src = base + plan->staging;
        src_pitch = plan->staging_pitch;
    }
    assert(plan->pitch[1] == plan->pitch[2]);
    const VmafxSyclPlaneOp op = {.src = src,
                                 .src_pitch = src_pitch,
                                 .dst0 = base + plan->offset[1],
                                 .dst1 = base + plan->offset[2],
                                 .dst_pitch = plan->pitch[1],
                                 .w = plan->pw[1],
                                 .rows = plan->ph[1],
                                 .bytes = plan->bytes,
                                 .shift = layout->shift};
    return vmafx_sycl_rt_frame_deinterleave(sf->rt, &op);
}

/* An owned plane that is not interleaved chroma: shifted (P010 luma),
 * de-tiled, or staged through the host (planted). */
static int fill_owned_plane(VmafxSyclFrame *sf, const VmafxImportLayout *layout,
                            const VmafxSyclSource *s, const SyclPlan *plan, uint8_t *base,
                            uint32_t i)
{
    uint8_t *const dst = base + plan->offset[i];
    if (s->tiled) {
        return detile(sf, s, plan->pw[i], plan->ph[i], plan->bytes, layout->shift, dst,
                      plan->pitch[i]);
    }
    const VmafxSyclPlaneOp op = {.src = source_address(s),
                                 .src_pitch = (size_t)s->pitch,
                                 .dst0 = dst,
                                 .dst1 = NULL,
                                 .dst_pitch = plan->pitch[i],
                                 .w = plan->pw[i],
                                 .rows = plan->ph[i],
                                 .bytes = plan->bytes,
                                 .shift = layout->shift};
    if (layout->shift != 0u) {
        return vmafx_sycl_rt_frame_shift(sf->rt, &op);
    }
    assert(plan->staged[i]);
    const int err = vmafx_sycl_rt_frame_stage_host(sf->rt, &op);
    vmafx_count_host_copy((uint64_t)plan->pw[i] * plan->bytes * plan->ph[i]);
    return err;
}

/* Frame plane `i`: bound, converted, de-tiled or (planted) staged. */
static int fill_plane(VmafxSyclFrame *sf, const VmafxImportLayout *layout,
                      const VmafxSyclSource src[3], const SyclPlan *plan, uint32_t i, void *data[3],
                      ptrdiff_t stride[3])
{
    uint8_t *const base = sf->owned;
    const VmafxSyclSource *const s = &src[source_plane(layout, i)];
    const bool pair = layout->interleaved && i > 0u;
    if (!plan->owned[i]) {
        data[i] = source_address(s);
        stride[i] = (ptrdiff_t)s->pitch;
        return 0;
    }
    data[i] = base + plan->offset[i];
    stride[i] = (ptrdiff_t)plan->pitch[i];
    if (pair) {
        return i == 1u ? fill_chroma_pair(sf, layout, s, plan, base) : 0;
    }
    return fill_owned_plane(sf, layout, s, plan, base, i);
}

/* Every plane of the frame into `data` / `stride`, the device work on the
 * library queue behind the acquire barrier. */
static VmafxStatus fill_planes(const VmafxReport *report, VmafxSyclFrame *sf,
                               const VmafxFrameImport *d, const VmafxImportLayout *layout,
                               const VmafxSyclSource src[3], void *data[3], ptrdiff_t stride[3])
{
    SyclPlan plan;
    plan_planes(d, layout, src, vmafx_test_switch(VMAFX_TEST_FORCE_HOST_COPY), &plan);
    if (plan.total) {
        sf->owned = vmafx_sycl_rt_alloc(sf->dev->rt, plan.total);
        if (!sf->owned) {
            return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "frame",
                              "backend sycl: cannot allocate %zu bytes of converted planes",
                              plan.total);
        }
    }
    int err = 0;
    for (uint32_t i = 0; i < plan.n_out && !err; i++) {
        err = fill_plane(sf, layout, src, &plan, i, data, stride);
    }
    if (!err) {
        err = vmafx_sycl_rt_frame_ready(sf->rt);
    }
    if (err) {
        return VMAFX_FAIL(report, err == -ENOMEM ? VMAFX_E_NOMEM : VMAFX_E_DEVICE, err,
                          VMAFX_SUBJECT_FRAME, "frame",
                          "backend sycl: cannot enqueue the planes of the import (%d)", err);
    }
    const bool tiled = src[0].tiled || src[1].tiled || src[2].tiled;
    if (layout->interleaved || layout->shift != 0u || tiled) {
        vmafx_count_conversion();
    }
    return VMAFX_OK;
}

/* ---- Binding ------------------------------------------------------------------ */

/* The producer planes of a USM import, as the device reads them. */
static void pointer_sources(const VmafxFrameImport *d, VmafxSyclSource src[3])
{
    for (uint32_t i = 0; i < 3u; i++) {
        const bool given = i < d->n_planes;
        /* NOLINTNEXTLINE(performance-no-int-to-ptr): a USM pointer crosses the ABI as uintptr_t (VmafxImportPlane.handle, ADR-1929). */
        src[i] = (VmafxSyclSource){.base = given ? (uint8_t *)d->plane[i].handle : NULL,
                                   .offset = given ? d->plane[i].offset : 0u,
                                   .pitch = given ? d->plane[i].pitch : 0u,
                                   .tiled = false,
                                   .tiling = VMAFX_SYCL_TILING_Y};
    }
}

/* The acquire barrier (none under the planted defect) and the planes. */
static VmafxStatus enqueue_import(const VmafxReport *report, VmafxSyclFrame *sf,
                                  const VmafxFrameImport *d, const VmafxImportLayout *layout,
                                  const VmafxSyclSource src[3], void *data[3], ptrdiff_t stride[3])
{
    if (d->acquire.kind == VMAFX_FENCE_SYCL_EVENT &&
        !vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT)) {
        const int err = vmafx_sycl_rt_frame_after_event(sf->rt, d->acquire.handle);
        if (err) {
            return VMAFX_FAIL(report, VMAFX_E_INVALID, err, VMAFX_SUBJECT_FENCE,
                              "desc.acquire.handle",
                              "backend sycl: cannot wait on the acquire event 0x%llx (%d); is "
                              "it an event of the device's context?",
                              (unsigned long long)d->acquire.handle, err);
        }
    }
    return fill_planes(report, sf, d, layout, src, data, stride);
}

/* The VmafxFrame over the planes, a SYCL device picture. */
static VmafxStatus bind_picture(const VmafxReport *report, VmafxSyclFrame *sf,
                                const VmafxFrameImport *d, const VmafxImportLayout *layout,
                                void *data[3], ptrdiff_t stride[3], VmafxFrame *frame)
{
    VmafxFrameDesc planar = VMAFX_FRAME_DESC_INIT;
    planar.pix_fmt = layout->planar_fmt;
    planar.bpc = d->bpc;
    planar.w = d->w;
    planar.h = d->h;
    const int err = vmafx_frame_bind(frame, &planar, data, stride);
    if (err) {
        return VMAFX_FAIL(report, err == -ENOMEM ? VMAFX_E_NOMEM : VMAFX_E_DEVICE, err,
                          VMAFX_SUBJECT_FRAME, "frame", "backend sycl: cannot bind the frame (%d)",
                          err);
    }
    vmafx_sycl_picture_attach(&frame->pic, sf);
    return VMAFX_OK;
}

/* The producer planes as the device reads them: USM pointers, or the
 * dma-bufs of a DMABUF or GL_TEXTURE descriptor imported into `sf`. */
static VmafxStatus resolve_sources(const VmafxReport *report, VmafxSyclFrame *sf,
                                   const VmafxFrameImport *desc, const VmafxImportLayout *layout,
                                   VmafxSyclSource src[3])
{
    if (desc->memory == VMAFX_MEMORY_DEVICE_POINTER) {
        pointer_sources(desc, src);
        return VMAFX_OK;
    }
    VmafxFrameImport d = *desc;
    int exported[3] = {-1, -1, -1};
    VmafxStatus status = d.memory == VMAFX_MEMORY_GL_TEXTURE ?
                             vmafx_sycl_gl_export(report, sf, &d, exported) :
                             VMAFX_OK;
    if (status == VMAFX_OK) {
        /* A caller's dma-buf: a poll (a pending writer is VMAFX_E_BUSY, which
         * the D8 helper waits on). A GL export: the GL driver's own work on
         * the texture for the export (about 2 ms measured), behind a GL sync
         * the caller declared signalled; waited for. */
        const uint64_t wait_ns =
            desc->memory == VMAFX_MEMORY_GL_TEXTURE ? VMAFX_SYCL_GL_EXPORT_WAIT_NS : 0u;
        status = vmafx_sycl_dmabuf_planes(report, sf, &d, layout, wait_ns, src);
    }
    /* The import holds duplicates of the exported descriptors. */
    for (unsigned i = 0; i < 3u; i++) {
#ifndef _WIN32
        if (exported[i] >= 0) {
            (void)close(exported[i]);
        }
#endif
    }
    return status;
}

/* Bind a checked descriptor: lane state, planes, picture. */
static VmafxStatus bind_sycl_frame(const VmafxReport *report, VmafxDevice *device,
                                   const VmafxFrameImport *desc, const VmafxImportLayout *layout,
                                   VmafxFrame **out)
{
    VmafxSyclDevice *const dev = vmafx_sycl_dev(device);
    vmafx_sycl_rt_collect(dev->rt);
    VmafxSyclFrame *const sf = vmafx_sycl_frame_state_new(dev);
    VmafxFrame *const frame = sf ? calloc(1, sizeof(*frame)) : NULL;
    if (!frame) {
        vmafx_sycl_frame_state_discard(sf);
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "frame",
                          "backend sycl: cannot start the import");
    }
    VmafxSyclSource src[3];
    void *data[3] = {NULL, NULL, NULL};
    ptrdiff_t stride[3] = {0, 0, 0};
    VmafxStatus status = resolve_sources(report, sf, desc, layout, src);
    if (status == VMAFX_OK) {
        status = enqueue_import(report, sf, desc, layout, src, data, stride);
    }
    if (status == VMAFX_OK) {
        status = bind_picture(report, sf, desc, layout, data, stride, frame);
    }
    if (status != VMAFX_OK) {
        vmafx_sycl_frame_state_discard(sf);
        free(frame);
        return status;
    }
    assert(frame->pic.priv != NULL && frame->pic.ref != NULL);
    frame->lane = sf;
    frame->lane_release = vmafx_sycl_frame_release;
    frame->release = desc->release;
    frame->user = desc->user;
    const uint32_t residency = vmafx_test_import_residency();
    frame->residency = residency == VMAFX_TEST_RESIDENCY_OFF ? VMAFX_BACKEND_SYCL : residency;
    frame->device = vmafx_device_ref(device);
    *out = frame;
    return VMAFX_OK;
}

VmafxStatus vmafx_sycl_frame_import(const VmafxReport *report, VmafxDevice *device,
                                    const VmafxFrameImport *desc, const VmafxImportLayout *layout,
                                    VmafxFrame **out)
{
    VmafxStatus status = check_sycl_memory(report, desc, layout);
    if (status == VMAFX_OK && desc->memory == VMAFX_MEMORY_DEVICE_POINTER) {
        status = check_pointer_planes(report, vmafx_sycl_dev(device), desc, layout);
    }
    if (status == VMAFX_OK) {
        status = check_sycl_acquire(report, desc);
    }
    return status == VMAFX_OK ? bind_sycl_frame(report, device, desc, layout, out) : status;
}

/* NOLINTEND(modernize-use-nullptr) */
