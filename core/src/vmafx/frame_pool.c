/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Frame pools of the VMAFx API (RC4 WP3 common lane, ADR-1852 design section
 * 2.2): `count` frames of one geometry allocated once and reused, the
 * successor of vmaf_preallocate_pictures(). This lane pools host frames on
 * the CPU device; the backend lanes pool device memory behind the same
 * functions.
 *
 * A pool frame follows the frame-reference rule (ADR-1906 item 4): each
 * acquire arms the frame's picture with a fresh reference count, and where
 * the last count is dropped (a caller's unref, or the engine's after frame
 * n-2) pool_frame_release() signals the frame's release fence and puts the
 * frame back. The pool itself is counted: the caller's reference and one per
 * frame in use, so vmafx_frame_pool_destroy() with frames still in a context
 * frees nothing those frames need.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "error_internal.h"
#include "internal.h"
#ifdef HAVE_CUDA
#include "cuda/vmafx_cuda.h"
#endif
#ifdef HAVE_SYCL
#include "sycl/vmafx_sycl.h"
#endif
#include "picture.h"
#include "ref.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Largest pool (HISS-02 bound): far more frames than a decoder or filter
 * keeps in flight. */
#define VMAFX_POOL_MAX 1024u

struct VmafxFramePool {
    VmafRef *refs; /* the caller's reference and one per frame in use */
    pthread_mutex_t lock;
    VmafxDevice *device;
    uint32_t count;
    uint32_t n_free;
    VmafxFrame **frames;      /* every frame, for the final release */
    VmafxFrame **free_frames; /* the first n_free are free */
};

/* Free every frame's pixels and the pool. */
static void pool_free(VmafxFramePool *pool)
{
    for (uint32_t i = 0; i < pool->count; i++) {
        VmafxFrame *const frame = pool->frames[i];
        if (!frame) {
            continue;
        }
        if (frame->inner_release) {
            /* The picture's own release returns the pixel buffer
             * (vmaf_picture_alloc's buffer pool); it reads data and the
             * cookie. */
            VmafPicture pic = frame->pic;
            (void)frame->inner_release(&pic, frame->inner_cookie);
        }
#ifdef HAVE_CUDA
        if (frame->residency == VMAFX_BACKEND_CUDA) {
            vmafx_cuda_pool_frame_free(frame);
        }
#endif
#ifdef HAVE_SYCL
        if (frame->residency == VMAFX_BACKEND_SYCL) {
            vmafx_sycl_pool_frame_free(frame);
        }
#endif
        free(frame);
    }
    free((void *)pool->frames);
    free((void *)pool->free_frames);
    (void)pthread_mutex_destroy(&pool->lock);
    vmafx_device_unref(pool->device);
    (void)vmaf_ref_close(pool->refs);
    free(pool);
}

static void pool_unref(VmafxFramePool *pool)
{
    if (vmaf_ref_fetch_decrement(pool->refs) == 1) {
        pool_free(pool);
    }
}

/* The last reference of an acquired frame is gone: signal its release fence
 * and put it back. The engine frees the picture's private slot and count
 * after this returns; the next acquire makes new ones. */
static int pool_frame_release(VmafPicture *pic, void *cookie)
{
    VmafxFrame *const frame = cookie;
    VmafxFramePool *const pool = frame->pool;
    int err = 0;
    if (frame->lane_release) {
        /* A device frame: its fences are signalled after the last reader. */
        err = frame->lane_release(frame, pic);
        frame->lane_release = NULL;
    } else {
        vmafx_frame_signal_released(frame);
    }
    /* `pic` is the last holder's copy, whose slot and count the engine frees
     * after this returns; the frame's own picture forgets them now. */
    frame->pic.priv = NULL;
    frame->pic.ref = NULL;
    (void)pthread_mutex_lock(&pool->lock);
    assert(pool->n_free < pool->count);
    pool->free_frames[pool->n_free++] = frame;
    (void)pthread_mutex_unlock(&pool->lock);
    pool_unref(pool);
    return err;
}

/* The pixels of a pool frame on a device: 0, or a negative errno. */
static int device_planes(VmafxFramePool *pool, const VmafxFrameDesc *d, VmafxFrame *frame)
{
#ifdef HAVE_CUDA
    if (pool->device->backend == VMAFX_BACKEND_CUDA) {
        frame->device = pool->device;
        return vmafx_cuda_pool_frame_init(pool->device, d, frame);
    }
#endif
#ifdef HAVE_SYCL
    if (pool->device->backend == VMAFX_BACKEND_SYCL) {
        frame->device = pool->device;
        return vmafx_sycl_pool_frame_init(pool->device, d, frame);
    }
#endif
    (void)d;
    (void)frame;
    assert(pool->device->backend != VMAFX_BACKEND_CPU);
    return -ENOTSUP;
}

/* One frame of the pool with its pixels; its picture is armed on acquire. */
static VmafxFrame *pool_frame_new(VmafxFramePool *pool, const VmafxFrameDesc *d)
{
    VmafxFrame *const frame = calloc(1, sizeof(*frame));
    if (!frame) {
        return NULL;
    }
    if (pool->device->backend != VMAFX_BACKEND_CPU) {
        if (device_planes(pool, d, frame) != 0) {
            free(frame);
            return NULL;
        }
        frame->pool = pool;
        return frame;
    }
    if (vmaf_picture_alloc(&frame->pic, vmafx_engine_pixel_format(d->pix_fmt), d->bpc, d->w,
                           d->h) != 0) {
        free(frame);
        return NULL;
    }
    const VmafPicturePrivate *const priv = frame->pic.priv;
    frame->inner_release = priv->release_picture;
    frame->inner_cookie = priv->cookie;
    /* The allocation's own slot and count go; each acquire makes new ones. */
    free(frame->pic.priv);
    (void)vmaf_ref_close(frame->pic.ref);
    frame->pic.priv = NULL;
    frame->pic.ref = NULL;
    frame->pool = pool;
    frame->device = pool->device;
    frame->residency = pool->device->backend;
    return frame;
}

/* A pool of `count` frames with every frame free, or NULL (no memory). */
static VmafxFramePool *pool_new(VmafxDevice *device, const VmafxFrameDesc *d, uint32_t count)
{
    VmafxFramePool *const pool = calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }
    pool->frames = (VmafxFrame **)calloc(count, sizeof(*pool->frames));
    pool->free_frames = (VmafxFrame **)calloc(count, sizeof(*pool->free_frames));
    if (!pool->frames || !pool->free_frames || vmaf_ref_init(&pool->refs) != 0 ||
        pthread_mutex_init(&pool->lock, NULL) != 0) {
        free((void *)pool->frames);
        free((void *)pool->free_frames);
        (void)vmaf_ref_close(pool->refs);
        free(pool);
        return NULL;
    }
    pool->device = vmafx_device_ref(device);
    pool->count = count;
    for (uint32_t i = 0; i < count; i++) {
        pool->frames[i] = pool_frame_new(pool, d);
        if (!pool->frames[i]) {
            pool_free(pool);
            return NULL;
        }
        pool->free_frames[i] = pool->frames[i];
    }
    pool->n_free = count;
    return pool;
}

/* The device a pool allocates on: the CPU (NULL too), or a device whose
 * backend lane allocates pool frames (CUDA). HIP devices import frames only
 * (ADR-2092): a pool frame written on a stream the library does not know
 * has no ordering against the library stream's copies without a fence. */
static VmafxStatus pool_device(const VmafxReport *report, VmafxDevice *device,
                               VmafxDevice **resolved)
{
#ifdef HAVE_CUDA
    if (device && device->backend == VMAFX_BACKEND_CUDA) {
        *resolved = device;
        return VMAFX_OK;
    }
#endif
#ifdef HAVE_SYCL
    if (device && device->backend == VMAFX_BACKEND_SYCL) {
        *resolved = device;
        return VMAFX_OK;
    }
#endif
    if (device && device->backend == VMAFX_BACKEND_HIP) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "backend hip: a HIP device has no frame pools; import the frames "
                          "your producer holds with vmafx_frame_import() and an acquire fence");
    }
    return vmafx_frame_host_device(report, device, resolved);
}

VmafxStatus vmafx_frame_pool_create(VmafxDevice *device, const VmafxFrameDesc *desc, uint32_t count,
                                    VmafxFramePool **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "out",
                          "no place to store the pool");
    }
    *out = NULL;
    VmafxFrameDesc d = VMAFX_FRAME_DESC_INIT;
    VmafxDevice *host = NULL;
    VmafxStatus status = vmafx_frame_read_desc(&report, desc, &d);
    if (status == VMAFX_OK && (count == 0u || count > VMAFX_POOL_MAX)) {
        status = VMAFX_FAIL(&report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PARAMETER, "count",
                            "%u frames; a pool holds 1 to %u", (unsigned)count, VMAFX_POOL_MAX);
    }
    if (status == VMAFX_OK) {
        status = pool_device(&report, device, &host);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxFramePool *const pool = pool_new(host, &d, count);
    if (!pool) {
        return VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "pool",
                          "cannot allocate %u frames of %ux%u", (unsigned)count, (unsigned)d.w,
                          (unsigned)d.h);
    }
    *out = pool;
    return VMAFX_OK;
}

/* Give `frame` a fresh picture slot and count, with the pool's release: 0, or
 * a negative errno with nothing allocated. */
static int arm_frame(VmafxFrame *frame)
{
    VmafPicture *const pic = &frame->pic;
    int err = vmaf_picture_priv_init(pic);
    if (!err) {
        err = vmaf_picture_set_release_callback(pic, frame, pool_frame_release);
    }
    if (!err) {
        err = vmaf_ref_init(&pic->ref);
    }
#ifdef HAVE_CUDA
    if (!err && frame->residency == VMAFX_BACKEND_CUDA) {
        err = vmafx_cuda_pool_frame_arm(frame);
        if (err) {
            (void)vmaf_ref_close(pic->ref);
            pic->ref = NULL;
        }
    }
#endif
#ifdef HAVE_SYCL
    if (!err && frame->residency == VMAFX_BACKEND_SYCL) {
        err = vmafx_sycl_pool_frame_arm(frame);
        if (err) {
            (void)vmaf_ref_close(pic->ref);
            pic->ref = NULL;
        }
    }
#endif
    if (err) {
        free(pic->priv);
        pic->priv = NULL;
    }
    return err;
}

VmafxStatus vmafx_frame_pool_acquire(VmafxFramePool *pool, VmafxFrame **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!pool || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !pool ? "pool" : "out", "NULL argument");
    }
    *out = NULL;
    (void)pthread_mutex_lock(&pool->lock);
    VmafxFrame *const frame = pool->n_free ? pool->free_frames[--pool->n_free] : NULL;
    (void)pthread_mutex_unlock(&pool->lock);
    if (!frame) {
        return VMAFX_FAIL(&report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FRAME, "pool",
                          "all %u frames are in use; one returns when its last reference is "
                          "dropped",
                          (unsigned)pool->count);
    }
    assert(frame->pool == pool && frame->pic.ref == NULL);
    if (arm_frame(frame) != 0) {
        (void)pthread_mutex_lock(&pool->lock);
        pool->free_frames[pool->n_free++] = frame;
        (void)pthread_mutex_unlock(&pool->lock);
        return VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "frame",
                          "cannot allocate a reference count");
    }
    vmaf_ref_fetch_increment(pool->refs);
    *out = frame;
    return VMAFX_OK;
}

void vmafx_frame_pool_destroy(VmafxFramePool *pool)
{
    if (pool) {
        pool_unref(pool);
    }
}

/* NOLINTEND(modernize-use-nullptr) */
