/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Frame pools on a SYCL device (RC4 WP3, ADR-1929 item 7, ADR-2091): the
 * frames are device USM of the device's context, allocated once. A frame
 * returns to its pool where its last reference is dropped, which may be
 * before the device ran its readers; its release records an idle event in a
 * slot of the release-event table, and the next acquire waits on it on the
 * host, so the caller's next write cannot overwrite a frame still being read.
 *
 * A pool frame carries no acquire fence: the caller's writes to it must be
 * complete when it is submitted (a wait on the queue that wrote it). SYCL has
 * no context-wide barrier the library could put in its place (the CUDA lane
 * uses the ADR-1199 one).
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "picture.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_sycl.h"
#include "vmafx_sycl_internal.h"
#include "vmafx_sycl_rt.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Row alignment of a pool frame's planes. */
#define VMAFX_SYCL_POOL_ALIGN 64u
/* Longest host wait for a pool frame's previous readers (10 s). */
#define VMAFX_SYCL_IDLE_WAIT_NS 10000000000ull

static size_t pool_pitch(unsigned w, size_t bytes)
{
    const size_t row = (size_t)w * bytes;
    return (row + VMAFX_SYCL_POOL_ALIGN - 1u) & ~(size_t)(VMAFX_SYCL_POOL_ALIGN - 1u);
}

int vmafx_sycl_pool_frame_init(VmafxDevice *device, const VmafxFrameDesc *d, VmafxFrame *frame)
{
    VmafxSyclDevice *const dev = vmafx_sycl_dev(device);
    VmafPicture *const pic = &frame->pic;
    assert(dev != NULL && d->w > 0u && d->h > 0u);
    pic->pix_fmt = vmafx_engine_pixel_format(d->pix_fmt);
    pic->bpc = d->bpc;
    vmaf_picture_plane_extents(pic->pix_fmt, d->w, d->h, pic->w, pic->h);
    const unsigned n = pic->pix_fmt == VMAF_PIX_FMT_YUV400P ? 1u : 3u;
    const size_t bytes = d->bpc > 8u ? 2u : 1u;
    size_t total = 0;
    for (unsigned i = 0; i < n; i++) {
        pic->stride[i] = (ptrdiff_t)pool_pitch(pic->w[i], bytes);
        total += (size_t)pic->stride[i] * pic->h[i];
    }
    VmafxSyclPoolFrame *const pf = calloc(1, sizeof(*pf));
    if (!pf || vmafx_sycl_rt_slot_new(true, &pf->idle_slot) != 0) {
        free(pf);
        return -ENOMEM;
    }
    uint8_t *const base = vmafx_sycl_rt_alloc(dev->rt, total);
    if (!base) {
        vmafx_sycl_rt_slot_unref(pf->idle_slot);
        free(pf);
        return -ENOMEM;
    }
    size_t offset = 0;
    for (unsigned i = 0; i < n; i++) {
        pic->data[i] = base + offset;
        offset += (size_t)pic->stride[i] * pic->h[i];
    }
    frame->lane_persistent = pf;
    frame->residency = VMAFX_BACKEND_SYCL;
    return 0;
}

uint32_t vmafx_sycl_pool_idle_slot(const VmafxFrame *frame)
{
    const VmafxSyclPoolFrame *const pf = frame->lane_persistent;
    return pf ? pf->idle_slot : 0u;
}

/* The idle slot of a pool frame. */
static uint32_t idle_slot(const VmafxFrame *frame)
{
    return vmafx_sycl_pool_idle_slot(frame);
}

/* The slot as a poll answer. */
static int idle_done(const void *arg)
{
    return vmafx_sycl_rt_slot_poll(*(const uint32_t *)arg);
}

int vmafx_sycl_pool_frame_arm(VmafxFrame *frame)
{
    VmafxSyclDevice *const dev = vmafx_sycl_dev(frame->device);
    /* The frame went back to the pool where its last reference was dropped,
     * possibly before the device ran its readers: the caller writes the
     * planes next, on a queue the library does not know, so the readers must
     * be done (ADR-2091). Usually they are. */
    const uint32_t idle = idle_slot(frame);
    assert(frame->lane == NULL); /* armed once per hand-out, released in between */
    const int state = vmafx_fence_poll(idle_done, &idle, VMAFX_SYCL_IDLE_WAIT_NS);
    if (state != 1) {
        return state < 0 ? state : -ETIMEDOUT;
    }
    VmafxSyclFrame *const sf = vmafx_sycl_frame_state_new(dev);
    if (!sf) {
        return -ENOMEM;
    }
    vmafx_sycl_picture_attach(&frame->pic, sf);
    frame->lane = sf;
    frame->lane_release = vmafx_sycl_frame_release;
    return 0;
}

void vmafx_sycl_pool_frame_free(VmafxFrame *frame)
{
    VmafxSyclDevice *const dev = vmafx_sycl_dev(frame->device);
    const uint32_t idle = idle_slot(frame);
    if (idle) {
        /* The last release recorded its readers' completion here. */
        (void)vmafx_fence_poll(idle_done, &idle, VMAFX_SYCL_IDLE_WAIT_NS);
        vmafx_sycl_rt_slot_unref(idle);
    }
    free(frame->lane_persistent);
    frame->lane_persistent = NULL;
    if (frame->pic.data[0]) {
        vmafx_sycl_rt_free(dev->rt, frame->pic.data[0]);
        frame->pic.data[0] = NULL;
    }
}

/* NOLINTEND(modernize-use-nullptr) */
