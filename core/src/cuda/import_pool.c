/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Frame pools of a CUDA device (RC4 WP3, ADR-1929 item 11, ADR-2023): frames
 * whose planes the library allocates once on the device (the successor of
 * vmaf_cuda_preallocate_pictures() with the DEVICE method). The caller writes
 * a frame's planes (vmafx_frame_planes() gives the device addresses) and
 * submits it; a pool frame carries no acquire fence, so the engine orders
 * the caller's writes with the per-frame context barrier of ADR-1199. A pool
 * frame's release fences are the lane's (import_fence.c): signalled after
 * the device's last reader, before the frame is handed out again.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "picture.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_cuda.h"
#include "vmafx_cuda_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Free the planes allocated so far (context pushed). */
static void free_planes(const VmafxCudaDevice *dev, VmafPicture *pic)
{
    for (unsigned i = 0; i < 3u; i++) {
        if (pic->data[i]) {
            (void)dev->state.f->cuMemFree((CUdeviceptr)(uintptr_t)pic->data[i]);
            pic->data[i] = NULL;
        }
    }
}

int vmafx_cuda_pool_frame_init(VmafxDevice *device, const VmafxFrameDesc *d, VmafxFrame *frame)
{
    const VmafxCudaDevice *const dev = vmafx_cuda_dev(device);
    VmafPicture *const pic = &frame->pic;
    pic->pix_fmt = vmafx_engine_pixel_format(d->pix_fmt);
    pic->bpc = d->bpc;
    vmaf_picture_plane_extents(pic->pix_fmt, d->w, d->h, pic->w, pic->h);
    const unsigned n = pic->pix_fmt == VMAF_PIX_FMT_YUV400P ? 1u : 3u;
    const size_t bytes = d->bpc > 8u ? 2u : 1u;
    assert(dev != NULL && d->w > 0u && d->h > 0u);
    frame->residency = VMAFX_BACKEND_CUDA;
    int err = vmafx_cuda_push(dev);
    if (err) {
        return err;
    }
    CUevent idle = NULL;
    CUresult res = dev->state.f->cuEventCreate(&idle, CU_EVENT_DISABLE_TIMING);
    frame->lane_persistent = idle;
    for (unsigned i = 0; i < n && res == CUDA_SUCCESS; i++) {
        CUdeviceptr plane = 0;
        size_t pitch = 0;
        res = dev->state.f->cuMemAllocPitch(&plane, &pitch, (size_t)pic->w[i] * bytes, pic->h[i],
                                            (unsigned)(8u * bytes));
        /* NOLINTNEXTLINE(performance-no-int-to-ptr): device addresses travel in VmafPicture.data as void *, as vmaf_cuda_picture_alloc() stores them (ADR-2023). */
        pic->data[i] = res == CUDA_SUCCESS ? (void *)(uintptr_t)plane : NULL;
        pic->stride[i] = (ptrdiff_t)pitch;
    }
    if (res != CUDA_SUCCESS) {
        free_planes(dev, pic);
        if (idle) {
            (void)dev->state.f->cuEventDestroy(idle);
        }
        frame->lane_persistent = NULL;
        err = vmaf_cuda_result_to_errno((int)res);
    }
    return vmafx_cuda_pop(dev, err);
}

/* The event a pool frame's last readers are recorded behind. */
static CUevent idle_event(const VmafxFrame *frame)
{
    return (CUevent)frame->lane_persistent;
}

int vmafx_cuda_pool_frame_arm(VmafxFrame *frame)
{
    VmafxCudaDevice *const dev = vmafx_cuda_dev(frame->device);
    VmafxCudaFrame *const cf = vmafx_cuda_frame_state_new(dev);
    int err = cf ? vmafx_cuda_push(dev) : -ENOMEM;
    if (!err) {
        /* The frame went back to the pool where its last reference was
         * dropped, possibly before the device ran its readers: the caller
         * writes the planes next, on a stream the library does not know, so
         * the readers must be done (ADR-2023). Usually they are. */
        const CUresult res = dev->state.f->cuEventSynchronize(idle_event(frame));
        err = res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res);
        err = err ? err : vmafx_cuda_picture_attach(dev, &frame->pic, false);
        err = vmafx_cuda_pop(dev, err);
    }
    if (err) {
        vmafx_cuda_frame_state_free(cf);
        return err;
    }
    assert(cf != NULL);
    frame->lane = cf;
    frame->lane_release = vmafx_cuda_frame_release;
    return 0;
}

void vmafx_cuda_pool_frame_released(VmafxFrame *frame, CUstream stream)
{
    (void)vmafx_cuda_dev(frame->device)->state.f->cuEventRecord(idle_event(frame), stream);
}

void vmafx_cuda_pool_frame_free(VmafxFrame *frame)
{
    const VmafxCudaDevice *const dev = vmafx_cuda_dev(frame->device);
    if (vmafx_cuda_push(dev) != 0) {
        return;
    }
    /* The last release enqueued its readers' completion; drain before the
     * planes go. */
    (void)dev->state.f->cuStreamSynchronize(dev->state.str);
    free_planes(dev, &frame->pic);
    if (frame->lane_persistent) {
        (void)dev->state.f->cuEventDestroy(idle_event(frame));
        frame->lane_persistent = NULL;
    }
    (void)vmafx_cuda_pop(dev, 0);
}

/* NOLINTEND(modernize-use-nullptr) */
