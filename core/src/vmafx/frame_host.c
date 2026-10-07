/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Host frames of the VMAFx API (ADR-1852, RC4 WP2).
 *
 * A VmafxFrame wraps one engine VmafPicture. The picture's VmafRef counts
 * every reference: the caller's (vmafx_frame_ref / unref), the one
 * vmafx_submit() moves into the engine, and the ones the engine takes for
 * frames n-1 / n-2 (ADR-1478). Whoever drops the last one runs
 * frame_release(), which frees the pixels (or calls the borrowed planes'
 * release callback) and the frame itself. So one frame is scored by several
 * contexts without a copy (device-targeted scoring, PR #2185): each submit
 * consumes one reference.
 *
 * Imported frames (frame_import.c, RC4 WP3) are released here too: their
 * converted planes are freed and their release fence is signalled after the
 * last reader is done, wherever the last reference is dropped.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "error_internal.h"
#include "internal.h"
#include "mem.h"
#include "picture.h"
#include "ref.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Sample depths the engine reads (picture.c, VMAF_PIC_BPC_MIN / MAX). */
#define VMAFX_FRAME_BPC_MIN 8u
#define VMAFX_FRAME_BPC_MAX 16u

void vmafx_frame_signal_released(VmafxFrame *frame)
{
    VmafxHostFence *const fence = atomic_exchange(&frame->released, (VmafxHostFence *)NULL);
    if (fence) {
        vmafx_host_fence_signal(fence);
        vmafx_host_fence_unref(fence);
    }
}

/* Runs once, on the thread that drops the last reference. `pic` is that
 * holder's copy of the picture; `frame` is freed here. The release fence is
 * signalled after the last read of the producer's memory. */
int vmafx_frame_release(VmafPicture *pic, void *cookie)
{
    VmafxFrame *const frame = cookie;
    assert(frame->pool == NULL);
    int err = 0;
    if (frame->inner_release) {
        err = frame->inner_release(pic, frame->inner_cookie);
    }
    if (frame->release) {
        frame->release(frame->user);
    }
    vmafx_frame_signal_released(frame);
    vmafx_device_unref(frame->device);
    aligned_free(frame->owned);
    free(frame);
    return err;
}

enum VmafPixelFormat vmafx_engine_pixel_format(uint32_t pix_fmt)
{
    switch (pix_fmt) {
    case VMAFX_PIXEL_FORMAT_YUV420P:
        return VMAF_PIX_FMT_YUV420P;
    case VMAFX_PIXEL_FORMAT_YUV422P:
        return VMAF_PIX_FMT_YUV422P;
    case VMAFX_PIXEL_FORMAT_YUV444P:
        return VMAF_PIX_FMT_YUV444P;
    case VMAFX_PIXEL_FORMAT_YUV400P:
        return VMAF_PIX_FMT_YUV400P;
    default:
        return VMAF_PIX_FMT_UNKNOWN;
    }
}

VmafColor vmafx_engine_color(const VmafxColor *color)
{
    VmafColor c = {0};
    c.range = (enum VmafColorRange)color->range;
    c.primaries = (enum VmafColorPrimaries)color->primaries;
    c.trc = (enum VmafColorTransferCharacteristic)color->trc;
    c.matrix = (enum VmafColorMatrixCoefficients)color->matrix;
    return c;
}

static uint32_t plane_count(uint32_t pix_fmt)
{
    return pix_fmt == VMAFX_PIXEL_FORMAT_YUV400P ? 1u : 3u;
}

VmafxStatus vmafx_frame_read_desc(const VmafxReport *report, const VmafxFrameDesc *desc,
                                  VmafxFrameDesc *d)
{
    if (!desc) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc",
                          "NULL argument");
    }
    const VmafxStatus status =
        vmafx_read_sized(report, d, (uint32_t)sizeof(*d), desc, VMAFX_MIN_FRAME_DESC, "desc");
    if (status != VMAFX_OK) {
        return status;
    }
    if (d->pix_fmt < VMAFX_PIXEL_FORMAT_YUV420P || d->pix_fmt > VMAFX_PIXEL_FORMAT_YUV400P) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.pix_fmt",
                          "pixel format %u is not a VmafxPixelFormat", (unsigned)d->pix_fmt);
    }
    if (d->bpc < VMAFX_FRAME_BPC_MIN || d->bpc > VMAFX_FRAME_BPC_MAX) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.bpc",
                          "%u bits per component; frames hold 8 to 16", (unsigned)d->bpc);
    }
    if (d->w == 0 || d->h == 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          d->w == 0 ? "desc.w" : "desc.h", "a frame is at least 1x1, not %ux%u",
                          (unsigned)d->w, (unsigned)d->h);
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_frame_host_device(const VmafxReport *report, VmafxDevice *device,
                                    VmafxDevice **resolved)
{
    VmafxDevice *const d = device ? device : vmafx_device_cpu();
    if (d->backend != VMAFX_BACKEND_CPU) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "host frames live on the CPU device, not on backend %u",
                          (unsigned)d->backend);
    }
    *resolved = d;
    return VMAFX_OK;
}

VmafxStatus vmafx_frame_create_host(VmafxDevice *device, const VmafxFrameDesc *desc,
                                    VmafxFrame **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "out",
                          "no place to store the frame");
    }
    *out = NULL;
    VmafxFrameDesc d = VMAFX_FRAME_DESC_INIT;
    VmafxDevice *host = NULL;
    VmafxStatus status = vmafx_frame_read_desc(&report, desc, &d);
    if (status == VMAFX_OK) {
        status = vmafx_frame_host_device(&report, device, &host);
    }
    VmafxFrame *const frame = status == VMAFX_OK ? calloc(1, sizeof(*frame)) : NULL;
    if (status != VMAFX_OK || !frame) {
        return status != VMAFX_OK ? status :
                                    VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME,
                                               "frame", "cannot allocate a frame");
    }
    const int err =
        vmaf_picture_alloc(&frame->pic, vmafx_engine_pixel_format(d.pix_fmt), d.bpc, d.w, d.h);
    if (err) {
        free(frame);
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_PARAMETER,
                          "desc", "cannot allocate a %ux%u %u-bit frame (%d)", (unsigned)d.w,
                          (unsigned)d.h, (unsigned)d.bpc, err);
    }
    const VmafPicturePrivate *const priv = frame->pic.priv;
    frame->inner_release = priv->release_picture;
    frame->inner_cookie = priv->cookie;
    /* Cannot fail: the picture and the callback are set. */
    const int hooked = vmaf_picture_set_release_callback(&frame->pic, frame, vmafx_frame_release);
    assert(hooked == 0);
    (void)hooked;
    frame->device = vmafx_device_ref(host);
    frame->color = d.color;
    *out = frame;
    return VMAFX_OK;
}

/* Check the borrowed planes against the frame geometry. */
static VmafxStatus check_host_planes(const VmafxReport *report, const VmafxFrameDesc *d,
                                     const VmafxHostPlanes *p)
{
    static const char *const data_names[] = {"planes.data[0]", "planes.data[1]", "planes.data[2]"};
    static const char *const stride_names[] = {"planes.stride[0]", "planes.stride[1]",
                                               "planes.stride[2]"};
    unsigned w[3];
    unsigned h[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(d->pix_fmt), d->w, d->h, w, h);
    const uint64_t bytes = d->bpc > 8u ? 2u : 1u;
    const uint32_t n_planes = plane_count(d->pix_fmt);
    for (uint32_t i = 0; i < n_planes; i++) {
        if (!p->data[i]) {
            return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE, data_names[i],
                              "plane %u has no data", (unsigned)i);
        }
        if (p->stride[i] < (uint64_t)w[i] * bytes || p->stride[i] > (uint64_t)PTRDIFF_MAX) {
            return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE, stride_names[i],
                              "stride %llu cannot hold a row of %u samples",
                              (unsigned long long)p->stride[i], w[i]);
        }
    }
    return VMAFX_OK;
}

int vmafx_frame_bind(VmafxFrame *frame, const VmafxFrameDesc *d, void *const data[3],
                     const ptrdiff_t stride[3])
{
    VmafPicture *const pic = &frame->pic;
    frame->color = d->color;
    pic->pix_fmt = vmafx_engine_pixel_format(d->pix_fmt);
    pic->bpc = d->bpc;
    vmaf_picture_plane_extents(pic->pix_fmt, d->w, d->h, pic->w, pic->h);
    for (uint32_t i = 0; i < plane_count(d->pix_fmt); i++) {
        pic->data[i] = data[i];
        pic->stride[i] = stride[i];
    }
    int err = vmaf_picture_priv_init(pic);
    if (!err) {
        err = vmaf_picture_set_release_callback(pic, frame, vmafx_frame_release);
    }
    if (!err) {
        err = vmaf_ref_init(&pic->ref);
    }
    if (err) {
        free(pic->priv);
        pic->priv = NULL;
    }
    return err;
}

/* The engine picture over borrowed planes (no allocation of pixels). */
static int wrap_picture(VmafxFrame *frame, const VmafxFrameDesc *d, const VmafxHostPlanes *p)
{
    ptrdiff_t stride[3] = {0, 0, 0};
    for (uint32_t i = 0; i < 3u; i++) {
        stride[i] = (ptrdiff_t)p->stride[i];
    }
    return vmafx_frame_bind(frame, d, p->data, stride);
}

VmafxStatus vmafx_frame_wrap_host(VmafxDevice *device, const VmafxFrameDesc *desc,
                                  const VmafxHostPlanes *planes, VmafxFrame **out,
                                  VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!out || !planes) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !out ? "out" : "planes", "NULL argument");
    }
    *out = NULL;
    VmafxFrameDesc d = VMAFX_FRAME_DESC_INIT;
    VmafxHostPlanes p = VMAFX_HOST_PLANES_INIT;
    VmafxDevice *host = NULL;
    VmafxStatus status = vmafx_frame_read_desc(&report, desc, &d);
    if (status == VMAFX_OK) {
        status = vmafx_read_sized(&report, &p, (uint32_t)sizeof(p), planes, VMAFX_MIN_HOST_PLANES,
                                  "planes");
    }
    if (status == VMAFX_OK) {
        status = check_host_planes(&report, &d, &p);
    }
    if (status == VMAFX_OK) {
        status = vmafx_frame_host_device(&report, device, &host);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxFrame *const frame = calloc(1, sizeof(*frame));
    if (!frame || wrap_picture(frame, &d, &p) != 0) {
        free(frame);
        return VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "frame",
                          "cannot allocate a frame");
    }
    frame->release = p.release;
    frame->user = p.user;
    frame->device = vmafx_device_ref(host);
    *out = frame;
    return VMAFX_OK;
}

VmafxStatus vmafx_frame_planes(const VmafxFrame *frame, VmafxFramePlanes *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!frame || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !frame ? "frame" : "out", "NULL argument");
    }
    const VmafPicture *const pic = &frame->pic;
    VmafxFramePlanes full = VMAFX_FRAME_PLANES_INIT;
    full.pix_fmt = (uint32_t)pic->pix_fmt;
    full.bpc = pic->bpc;
    full.n_planes = plane_count(full.pix_fmt);
    for (uint32_t i = 0; i < full.n_planes; i++) {
        full.w[i] = pic->w[i];
        full.h[i] = pic->h[i];
        full.stride[i] = (uint64_t)pic->stride[i];
        full.data[i] = pic->data[i];
    }
    return vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
}

VmafxFrame *vmafx_frame_ref(VmafxFrame *frame)
{
    if (frame) {
        vmaf_ref_fetch_increment(frame->pic.ref);
    }
    return frame;
}

void vmafx_frame_unref(VmafxFrame *frame)
{
    if (!frame) {
        return;
    }
    /* A copy: the last unref frees `frame`, and with it frame->pic. */
    VmafPicture pic = frame->pic;
    (void)vmaf_picture_unref(&pic);
}

VmafPicture vmafx_frame_take_picture(VmafxFrame *frame)
{
    assert(frame != NULL);
    return frame->pic;
}

/* NOLINTEND(modernize-use-nullptr) */
