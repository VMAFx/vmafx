/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The frame's planes, uploaded once per frame into buffers the VmafContext
 *  owns and read by every HIP twin (ADR-1408). shared_frame.h has the
 *  contract.
 */

#include <stddef.h>

#include "shared_frame.h"

/* `hip/meson.build` compiles this file whenever enable_hip is on. Without
 * enable_hipcc (HAVE_HIPCC) no extractor has a device kernel to feed, so
 * the entry points below share nothing and report -ENOSYS where a twin
 * would have uploaded. */
#ifdef HAVE_HIPCC
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <hip/hip_runtime_api.h>

#include "common.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Frames alternate between two slots; see shared_frame.h for why two are
 * enough. */
#define SHARED_SLOTS 2u
/* Reference and distorted picture. */
#define SHARED_PICS 2u
/* Y, U, V. */
#define SHARED_PLANES 3u

/* One packed device plane of a slot. */
typedef struct SharedPlane {
    void *dev;
    size_t row_bytes;
    size_t rows;
    /* Holds the announced frame's plane. */
    bool uploaded;
} SharedPlane;

/* One host plane of the announced frame, as the twins will name it. */
typedef struct HostPlane {
    const void *data;
    ptrdiff_t stride;
} HostPlane;

struct VmafHipSharedFrame {
    SharedPlane plane[SHARED_SLOTS][SHARED_PICS][SHARED_PLANES];
    HostPlane host[SHARED_PICS][SHARED_PLANES];
    /* The announced pictures; NULL outside begin() / end(). */
    const VmafPicture *pic[SHARED_PICS];
    /* Planes a twin asked for in this frame, and in the frame before. */
    bool wanted[SHARED_PICS][SHARED_PLANES];
    bool wanted_before[SHARED_PICS][SHARED_PLANES];
    /* Twins whose latest acquire was served from the slot. */
    unsigned readers[SHARED_SLOTS];
    unsigned slot;
    /* Between begin() and end(): the announced pictures are readable. */
    bool active;
    /* The current slot was checked for readers before its first write of
     * this frame. */
    bool fenced;
    uint64_t uploads;
};

static size_t plane_bytes(const VmafHipPlaneUpload *p)
{
    if (p->row_bytes == 0u || p->rows == 0u || p->rows > SIZE_MAX / p->row_bytes)
        return 0u;
    return p->rows * p->row_bytes;
}

/* Which announced picture `p` names, or SHARED_PICS when it names neither. */
static unsigned announced_picture(const VmafHipSharedFrame *f, const VmafHipPlaneUpload *p)
{
    if (p->pic == NULL || p->plane >= SHARED_PLANES)
        return SHARED_PICS;
    for (unsigned i = 0u; i < SHARED_PICS; i++) {
        const HostPlane *h = &f->host[i][p->plane];
        if (h->data != NULL && h->data == p->pic->data[p->plane] &&
            h->stride == p->pic->stride[p->plane])
            return i;
    }
    return SHARED_PICS;
}

/* Whether `p` asks for the whole plane, packed: the one layout the shared
 * frame holds a plane in. */
static bool plane_is_whole(const VmafHipPlaneUpload *p)
{
    const size_t sample_bytes = (p->pic->bpc > 8u) ? 2u : 1u;
    return p->rows == p->pic->h[p->plane] &&
           p->row_bytes == (size_t)p->pic->w[p->plane] * sample_bytes;
}

/* Whether the announced frame can serve every entry: each names one of its
 * planes, whole and packed. */
static bool frame_serves(const VmafHipSharedFrame *f, const VmafHipPlaneUpload *planes,
                         unsigned n_planes)
{
    if (f == NULL || !f->active)
        return false;
    for (unsigned i = 0u; i < n_planes; i++) {
        if (announced_picture(f, &planes[i]) == SHARED_PICS || plane_bytes(&planes[i]) == 0u ||
            !plane_is_whole(&planes[i]))
            return false;
    }
    return true;
}

/* Before the first write into the current slot of a frame: a twin that still
 * holds the slot skipped the frame in between and may have kernels reading
 * it, so wait for the device to go idle. Without such a twin this is one
 * comparison. */
static int frame_fence(VmafHipSharedFrame *f)
{
    if (f->fenced)
        return 0;
    f->fenced = true;
    if (f->readers[f->slot] == 0u)
        return 0;
    return vmaf_hip_rc_to_errno(hipDeviceSynchronize());
}

/* Size `sp` for `p`. The device buffer is reallocated when the geometry
 * changed; the caller fenced the slot first. */
static int plane_reserve(SharedPlane *sp, const VmafHipPlaneUpload *p)
{
    if (sp->dev != NULL && sp->row_bytes == p->row_bytes && sp->rows == p->rows)
        return 0;
    if (sp->dev != NULL) {
        (void)hipFree(sp->dev);
        sp->dev = NULL;
    }
    const hipError_t rc = hipMalloc(&sp->dev, plane_bytes(p));
    if (rc != hipSuccess) {
        sp->dev = NULL;
        return vmaf_hip_rc_to_errno(rc);
    }
    sp->row_bytes = p->row_bytes;
    sp->rows = p->rows;
    return 0;
}

/* The planes of the current slot waiting for one upload call. */
typedef struct UploadBatch {
    VmafHipPlaneUpload todo[VMAF_HIP_SOURCE_MAX_PLANES];
    SharedPlane *pending[VMAF_HIP_SOURCE_MAX_PLANES];
    unsigned count;
} UploadBatch;

static bool batch_holds(const UploadBatch *b, const SharedPlane *sp)
{
    for (unsigned k = 0u; k < b->count; k++) {
        if (b->pending[k] == sp)
            return true;
    }
    return false;
}

/* Queue `p` for upload into `sp`, sizing the device plane first. */
static int batch_add(UploadBatch *b, SharedPlane *sp, const VmafHipPlaneUpload *p)
{
    const int err = plane_reserve(sp, p);
    if (err != 0)
        return err;
    b->todo[b->count] = *p;
    b->todo[b->count].dst = sp->dev;
    b->todo[b->count].dst_pitch = p->row_bytes;
    b->pending[b->count] = sp;
    b->count++;
    return 0;
}

/* An upload takes along every plane a twin asked for in the frame before and
 * the slot does not hold yet: the same twins are about to ask for them, and
 * one upload call is one host wait where each twin that found a plane missing
 * would wait on its own. A plane nobody asks for after all costs one upload;
 * it is not expected again in the next frame. */
static int batch_add_expected(VmafHipSharedFrame *f, UploadBatch *b)
{
    int err = 0;
    for (unsigned i = 0u; i < SHARED_PICS && err == 0; i++) {
        const VmafPicture *pic = f->pic[i];
        for (unsigned p = 0u; p < SHARED_PLANES && err == 0; p++) {
            SharedPlane *sp = &f->plane[f->slot][i][p];
            if (!f->wanted_before[i][p] || sp->uploaded || batch_holds(b, sp))
                continue;
            const VmafHipPlaneUpload whole = {
                .pic = pic,
                .plane = p,
                .row_bytes = (size_t)pic->w[p] * ((pic->bpc > 8u) ? 2u : 1u),
                .rows = pic->h[p],
            };
            if (f->host[i][p].data != NULL && plane_bytes(&whole) != 0u)
                err = batch_add(b, sp, &whole);
        }
    }
    return err;
}

/* Upload the entries the current slot does not hold yet, in one call, and
 * point device[] at the slot's planes. The upload returns once the copies
 * have read the pictures (vmaf_hip_picture_upload()). */
static int frame_upload(VmafHipSharedFrame *f, const VmafHipPlaneUpload *planes, unsigned n_planes,
                        uintptr_t stream, void **device)
{
    UploadBatch batch = {.count = 0u};
    int err = 0;
    for (unsigned i = 0u; i < n_planes && err == 0; i++) {
        const unsigned pic = announced_picture(f, &planes[i]);
        SharedPlane *sp = &f->plane[f->slot][pic][planes[i].plane];
        f->wanted[pic][planes[i].plane] = true;
        if (!sp->uploaded && !batch_holds(&batch, sp)) {
            err = frame_fence(f);
            if (err == 0)
                err = batch_add(&batch, sp, &planes[i]);
        }
        device[i] = sp->dev;
    }
    if (err == 0 && batch.count != 0u)
        err = batch_add_expected(f, &batch);
    if (err != 0 || batch.count == 0u)
        return err;
    err = vmaf_hip_picture_upload(batch.todo, batch.count, stream);
    if (err != 0)
        return err;
    for (unsigned k = 0u; k < batch.count; k++)
        batch.pending[k]->uploaded = true;
    f->uploads += batch.count;
    return 0;
}

int vmaf_hip_shared_frame_create(VmafHipSharedFrame **out)
{
    if (out == NULL)
        return -EINVAL;
    *out = calloc(1, sizeof(**out));
    return (*out != NULL) ? 0 : -ENOMEM;
}

void vmaf_hip_shared_frame_destroy(VmafHipSharedFrame **frame)
{
    if (frame == NULL || *frame == NULL)
        return;
    VmafHipSharedFrame *f = *frame;
    for (unsigned s = 0u; s < SHARED_SLOTS; s++) {
        for (unsigned i = 0u; i < SHARED_PICS; i++) {
            for (unsigned p = 0u; p < SHARED_PLANES; p++) {
                if (f->plane[s][i][p].dev != NULL)
                    (void)hipFree(f->plane[s][i][p].dev);
            }
        }
    }
    free(f);
    *frame = NULL;
}

int vmaf_hip_shared_frame_begin(VmafHipSharedFrame *frame, const VmafPicture *ref,
                                const VmafPicture *dist)
{
    if (frame == NULL)
        return 0;
    if (ref == NULL || dist == NULL)
        return -EINVAL;
    frame->slot = (frame->slot + 1u) % SHARED_SLOTS;
    frame->pic[0] = ref;
    frame->pic[1] = dist;
    for (unsigned i = 0u; i < SHARED_PICS; i++) {
        for (unsigned p = 0u; p < SHARED_PLANES; p++) {
            frame->plane[frame->slot][i][p].uploaded = false;
            frame->host[i][p].data = frame->pic[i]->data[p];
            frame->host[i][p].stride = frame->pic[i]->stride[p];
            frame->wanted_before[i][p] = frame->wanted[i][p];
            frame->wanted[i][p] = false;
        }
    }
    frame->active = true;
    frame->fenced = false;
    return 0;
}

void vmaf_hip_shared_frame_end(VmafHipSharedFrame *frame)
{
    if (frame == NULL)
        return;
    frame->active = false;
    for (unsigned i = 0u; i < SHARED_PICS; i++) {
        frame->pic[i] = NULL;
        for (unsigned p = 0u; p < SHARED_PLANES; p++)
            frame->host[i][p].data = NULL;
    }
}

uint64_t vmaf_hip_shared_frame_upload_count(const VmafHipSharedFrame *frame)
{
    return (frame != NULL) ? frame->uploads : 0u;
}

/* Let go of the slot `src` holds, if any. */
static void source_release(VmafHipPlaneSource *src)
{
    VmafHipSharedFrame *f = src->held_frame;
    if (f == NULL)
        return;
    if (src->held_slot < SHARED_SLOTS && f->readers[src->held_slot] > 0u)
        f->readers[src->held_slot]--;
    src->held_frame = NULL;
}

/* The twin's own buffers: allocate what the geometry needs and upload on the
 * twin's stream, waiting until the pictures are read. */
static int source_upload_private(VmafHipPlaneSource *src, const VmafHipPlaneUpload *planes,
                                 unsigned n_planes, uintptr_t stream, void **device)
{
    VmafHipPlaneUpload own[VMAF_HIP_SOURCE_MAX_PLANES];
    for (unsigned i = 0u; i < n_planes; i++) {
        const size_t bytes = plane_bytes(&planes[i]);
        if (bytes == 0u)
            return -EINVAL;
        if (src->private_bytes[i] != bytes) {
            if (src->private_dev[i] != NULL)
                (void)hipFree(src->private_dev[i]);
            src->private_dev[i] = NULL;
            src->private_bytes[i] = 0u;
            const hipError_t rc = hipMalloc(&src->private_dev[i], bytes);
            if (rc != hipSuccess)
                return vmaf_hip_rc_to_errno(rc);
            src->private_bytes[i] = bytes;
        }
        own[i] = planes[i];
        own[i].dst = src->private_dev[i];
        own[i].dst_pitch = planes[i].row_bytes;
        device[i] = src->private_dev[i];
    }
    return vmaf_hip_picture_upload(own, n_planes, stream);
}

int vmaf_hip_plane_source_acquire(VmafHipPlaneSource *src, VmafHipSharedFrame *frame,
                                  const VmafHipPlaneUpload *planes, unsigned n_planes,
                                  uintptr_t stream, void **device)
{
    if (src == NULL || planes == NULL || device == NULL || n_planes == 0u ||
        n_planes > VMAF_HIP_SOURCE_MAX_PLANES)
        return -EINVAL;
    /* The twin collected its previous frame before this submit(), so its
     * kernels no longer read the slot it held. Releasing first also keeps
     * the twin from fencing the slot against itself. */
    source_release(src);

    if (!frame_serves(frame, planes, n_planes))
        return source_upload_private(src, planes, n_planes, stream, device);

    const int err = frame_upload(frame, planes, n_planes, stream, device);
    if (err != 0)
        return err;
    frame->readers[frame->slot]++;
    src->held_frame = frame;
    src->held_slot = frame->slot;
    return 0;
}

int vmaf_hip_plane_source_acquire_luma(VmafHipPlaneSource *src, VmafHipSharedFrame *frame,
                                       const VmafPicture *ref, const VmafPicture *dist,
                                       uintptr_t stream, void **ref_dev, void **dis_dev)
{
    if (ref == NULL || ref_dev == NULL || (dist != NULL && dis_dev == NULL))
        return -EINVAL;
    const size_t row_bytes = (size_t)ref->w[0] * ((ref->bpc > 8u) ? 2u : 1u);
    const VmafHipPlaneUpload planes[2] = {
        {.pic = ref, .plane = 0u, .row_bytes = row_bytes, .rows = ref->h[0]},
        {.pic = dist, .plane = 0u, .row_bytes = row_bytes, .rows = ref->h[0]},
    };
    void *device[2] = {NULL, NULL};
    const int err =
        vmaf_hip_plane_source_acquire(src, frame, planes, (dist != NULL) ? 2u : 1u, stream, device);
    if (err != 0)
        return err;
    *ref_dev = device[0];
    if (dist != NULL)
        *dis_dev = device[1];
    return 0;
}

void vmaf_hip_plane_source_close(VmafHipPlaneSource *src)
{
    if (src == NULL)
        return;
    source_release(src);
    for (unsigned i = 0u; i < VMAF_HIP_SOURCE_MAX_PLANES; i++) {
        if (src->private_dev[i] != NULL)
            (void)hipFree(src->private_dev[i]);
        src->private_dev[i] = NULL;
        src->private_bytes[i] = 0u;
    }
}

/* NOLINTEND(modernize-use-nullptr) */

#else /* !HAVE_HIPCC — compile without device kernels */
#include <errno.h>

int vmaf_hip_shared_frame_create(VmafHipSharedFrame **out)
{
    (void)out;
    return -ENOSYS;
}

void vmaf_hip_shared_frame_destroy(VmafHipSharedFrame **frame)
{
    (void)frame;
}

int vmaf_hip_shared_frame_begin(VmafHipSharedFrame *frame, const VmafPicture *ref,
                                const VmafPicture *dist)
{
    (void)frame;
    (void)ref;
    (void)dist;
    return 0;
}

void vmaf_hip_shared_frame_end(VmafHipSharedFrame *frame)
{
    (void)frame;
}

uint64_t vmaf_hip_shared_frame_upload_count(const VmafHipSharedFrame *frame)
{
    (void)frame;
    return 0u;
}

int vmaf_hip_plane_source_acquire(VmafHipPlaneSource *src, VmafHipSharedFrame *frame,
                                  const VmafHipPlaneUpload *planes, unsigned n_planes,
                                  uintptr_t stream, void **device)
{
    (void)src;
    (void)frame;
    (void)planes;
    (void)n_planes;
    (void)stream;
    (void)device;
    return -ENOSYS;
}

int vmaf_hip_plane_source_acquire_luma(VmafHipPlaneSource *src, VmafHipSharedFrame *frame,
                                       const VmafPicture *ref, const VmafPicture *dist,
                                       uintptr_t stream, void **ref_dev, void **dis_dev)
{
    (void)src;
    (void)frame;
    (void)ref;
    (void)dist;
    (void)stream;
    (void)ref_dev;
    (void)dis_dev;
    return -ENOSYS;
}

void vmaf_hip_plane_source_close(VmafHipPlaneSource *src)
{
    (void)src;
}

#endif /* HAVE_HIPCC */
