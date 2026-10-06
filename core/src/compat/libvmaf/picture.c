/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * libvmaf pictures on VMAFx frames (ADR-1852 design section 2.11). A
 * VmafPicture stays a caller-visible struct: a view of a frame holding one of
 * its references (vmafx_frame_to_picture()), released by vmaf_picture_unref()
 * or consumed by vmaf_read_pictures(). VmafPicture2 (ADR-0928) carries the
 * same view plus the backend tag. Conversions (#2140) run on frame
 * converters.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "compat_errno.h"
#include "libvmaf/picture.h"
#include "libvmaf/picture_v2.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* A host frame of the geometry, as a picture holding one reference. The
 * library checks the geometry (format, 8 to 16 bits, size) as libvmaf did. */
static int alloc_picture(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                         unsigned h)
{
    if (!pic || !pix_fmt) {
        return -EINVAL;
    }
    VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
    desc.pix_fmt = (uint32_t)pix_fmt;
    desc.bpc = bpc;
    desc.w = w;
    desc.h = h;
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    VmafxStatus status = vmafx_frame_create_host(NULL, &desc, &frame, &error);
    if (status == VMAFX_OK) {
        status = vmafx_frame_to_picture(frame, pic, &error);
        vmafx_frame_unref(frame);
    }
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_picture_alloc(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                       unsigned h)
{
    return alloc_picture(pic, pix_fmt, bpc, w, h);
}

/* Drop the reference `pic` holds and clear it. */
static int unref_picture(VmafPicture *pic)
{
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_frame_from_picture(pic, &frame, &error);
    if (status != VMAFX_OK) {
        return compat_errno(status, error);
    }
    /* Ours and the picture's. */
    vmafx_frame_unref(frame);
    vmafx_frame_unref(frame);
    memset(pic, 0, sizeof(*pic));
    return 0;
}

int vmaf_picture_unref(VmafPicture *pic)
{
    if (!pic || !pic->ref) {
        return -EINVAL;
    }
    return unref_picture(pic);
}

/* The v1 fields of a v2 picture (same prefix). */
static VmafPicture v1_view(const VmafPicture2 *src)
{
    VmafPicture v1;
    memset(&v1, 0, sizeof(v1));
    v1.pix_fmt = src->pix_fmt;
    v1.bpc = src->bpc;
    for (unsigned i = 0; i < 3; i++) {
        v1.w[i] = src->w[i];
        v1.h[i] = src->h[i];
        v1.stride[i] = src->stride[i];
        v1.data[i] = src->data[i];
    }
    v1.ref = src->ref;
    v1.priv = src->priv;
    return v1;
}

/* A CPU v2 picture of the v1 fields. */
static void v2_from_v1(const VmafPicture *v1, VmafPicture2 *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->pix_fmt = v1->pix_fmt;
    dst->bpc = v1->bpc;
    for (unsigned i = 0; i < 3; i++) {
        dst->w[i] = v1->w[i];
        dst->h[i] = v1->h[i];
        dst->stride[i] = v1->stride[i];
        dst->data[i] = v1->data[i];
    }
    dst->ref = v1->ref;
    dst->priv = v1->priv;
    dst->backend = VMAF_BACKEND_HANDLE_NONE;
    dst->backend_handle = 0;
}

int vmaf_picture2_alloc(VmafPicture2 *pic, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                        unsigned h)
{
    if (!pic) {
        return -EINVAL;
    }
    VmafPicture v1;
    memset(&v1, 0, sizeof(v1));
    const int err = alloc_picture(&v1, pix_fmt, bpc, w, h);
    if (err) {
        return err;
    }
    v2_from_v1(&v1, pic);
    return 0;
}

int vmaf_picture2_unref(VmafPicture2 *pic)
{
    if (!pic) {
        return 0; /* NULL is a documented no-op. */
    }
    if (!pic->ref) {
        return -EINVAL;
    }
    VmafPicture v1 = v1_view(pic);
    const int err = unref_picture(&v1);
    if (!err) {
        memset(pic, 0, sizeof(*pic));
    }
    return err;
}

/* A new reference of `src` as a picture in `dst` (the caller owns both). */
static int new_reference(const VmafPicture *src, VmafPicture *dst)
{
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    VmafxStatus status = vmafx_frame_from_picture((VmafPicture *)src, &frame, &error);
    if (status == VMAFX_OK) {
        status = vmafx_frame_to_picture(frame, dst, &error);
        vmafx_frame_unref(frame);
    }
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_picture_v1_to_v2(const VmafPicture *src, VmafPicture2 *dst)
{
    if (!src || !dst || !src->ref) {
        return -EINVAL;
    }
    VmafPicture v1;
    memset(&v1, 0, sizeof(v1));
    const int err = new_reference(src, &v1);
    if (err) {
        return err;
    }
    v2_from_v1(&v1, dst);
    return 0;
}

int vmaf_picture_v2_to_v1(const VmafPicture2 *src, VmafPicture *dst)
{
    if (!src || !dst || !src->ref) {
        return -EINVAL;
    }
    const VmafPicture v1 = v1_view(src);
    memset(dst, 0, sizeof(*dst));
    return new_reference(&v1, dst);
}

/* VMAF_BACKEND_HANDLE_* to VmafxBackend; NONE and the removed Vulkan backend
 * (ADR-0726) are not backends of the new API and keep libvmaf's names. */
const char *vmaf_backend_handle_name(VmafBackendHandle backend)
{
    switch (backend) {
    case VMAF_BACKEND_HANDLE_NONE:
        return "none";
    case VMAF_BACKEND_HANDLE_CUDA:
        return vmafx_backend_name(VMAFX_BACKEND_CUDA);
    case VMAF_BACKEND_HANDLE_SYCL:
        return vmafx_backend_name(VMAFX_BACKEND_SYCL);
    case VMAF_BACKEND_HANDLE_HIP:
        return vmafx_backend_name(VMAFX_BACKEND_HIP);
    case VMAF_BACKEND_HANDLE_METAL:
        return vmafx_backend_name(VMAFX_BACKEND_METAL);
    case VMAF_BACKEND_HANDLE_VULKAN:
        return "vulkan";
    default:
        return "unknown";
    }
}

/* ---- Conversions (#2140) ------------------------------------------------- */

static VmafxColor frame_color(const VmafColor *color)
{
    VmafxColor c = {0};
    c.range = (uint32_t)color->range;
    c.primaries = (uint32_t)color->primaries;
    c.trc = (uint32_t)color->trc;
    c.matrix = (uint32_t)color->matrix;
    return c;
}

static VmafxConvertDesc convert_desc(const VmafPicture *src, const VmafColor *src_color,
                                     const VmafPictureConvertTarget *target)
{
    VmafxConvertDesc desc = VMAFX_CONVERT_DESC_INIT;
    desc.src_pix_fmt = (uint32_t)src->pix_fmt;
    desc.src_bpc = src->bpc;
    desc.src_w = src->w[0];
    desc.src_h = src->h[0];
    desc.src_color = frame_color(src_color);
    desc.dst_pix_fmt = (uint32_t)target->pix_fmt;
    desc.dst_bpc = target->bpc;
    desc.dst_w = target->w;
    desc.dst_h = target->h;
    desc.dst_color = frame_color(&target->color);
    desc.filter = (uint32_t)target->resample_filter;
    return desc;
}

int vmaf_picture_convert_context_init_with_color(VmafPictureConvertContext **ctx,
                                                 const VmafPicture *src, const VmafColor *src_color,
                                                 const VmafPictureConvertTarget *target)
{
    VmafxError *error = NULL;
    VmafxStatus status = VMAFX_OK;
    if (!ctx || !src || !src_color || !target) {
        /* The library answers: -EINVAL, or -ENOTSUP in a build without zimg. */
        status = vmafx_frame_converter_create(NULL, NULL, &error);
        return compat_errno(status, error);
    }
    const VmafxConvertDesc desc = convert_desc(src, src_color, target);
    VmafxFrameConverter *converter = NULL;
    status = vmafx_frame_converter_create(&desc, &converter, &error);
    if (status != VMAFX_OK) {
        return compat_errno(status, error);
    }
    *ctx = (VmafPictureConvertContext *)converter;
    return 0;
}

/* A frame reading the picture's planes: its own reference when the picture
 * has one, else a frame wrapped around the caller's planes. */
static VmafxStatus source_frame(const VmafPicture *src, VmafxFrame **out, VmafxError **error)
{
    if (src->ref) {
        return vmafx_frame_from_picture((VmafPicture *)src, out, error);
    }
    VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
    desc.pix_fmt = (uint32_t)src->pix_fmt;
    desc.bpc = src->bpc;
    desc.w = src->w[0];
    desc.h = src->h[0];
    VmafxHostPlanes planes = VMAFX_HOST_PLANES_INIT;
    for (unsigned i = 0; i < 3; i++) {
        planes.data[i] = src->data[i];
        planes.stride[i] = (uint64_t)src->stride[i];
    }
    return vmafx_frame_wrap_host(NULL, &desc, &planes, out, error);
}

int vmaf_picture_convert(VmafPictureConvertContext *ctx, VmafPicture *dst, const VmafPicture *src)
{
    VmafxFrameConverter *const converter = (VmafxFrameConverter *)ctx;
    VmafxError *error = NULL;
    if (!ctx || !dst || !src) {
        const VmafxStatus status = vmafx_frame_convert(converter, NULL, NULL, &error);
        return compat_errno(status, error);
    }
    VmafxFrame *in = NULL;
    VmafxStatus status = source_frame(src, &in, &error);
    VmafxFrame *out = NULL;
    if (status == VMAFX_OK) {
        status = vmafx_frame_convert(converter, in, &out, &error);
        vmafx_frame_unref(in);
    }
    if (status == VMAFX_OK) {
        status = vmafx_frame_to_picture(out, dst, &error);
        vmafx_frame_unref(out);
    }
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_picture_convert_context_close(VmafPictureConvertContext *ctx)
{
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_frame_converter_destroy((VmafxFrameConverter *)ctx, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

/* NOLINTEND(modernize-use-nullptr) */
