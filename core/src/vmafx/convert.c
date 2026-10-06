/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Frame converters of the VMAFx API (RC4 WP6): the engine's picture
 * conversion (core/src/picture_convert.c, #2140, zimg) on frames. A
 * VmafxFrameConverter is the engine's conversion context under its new name.
 * The engine decides what it supports: a build without zimg refuses every
 * call with -ENOTSUP, NULL arguments included, as libvmaf did.
 */

#include <stddef.h>
#include <stdint.h>

#include "error_internal.h"
#include "internal.h"
#include "libvmaf/picture.h"
#include "picture.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The engine context of a converter: the same object. */
static VmafPictureConvertContext *engine_converter(VmafxFrameConverter *converter)
{
    return (VmafPictureConvertContext *)converter;
}

static VmafColor engine_color(const VmafxColor *c)
{
    VmafColor color = {0};
    color.range = (enum VmafColorRange)c->range;
    color.primaries = (enum VmafColorPrimaries)c->primaries;
    color.trc = (enum VmafColorTransferCharacteristic)c->trc;
    color.matrix = (enum VmafColorMatrixCoefficients)c->matrix;
    return color;
}

/* A picture that carries only the geometry the engine reads at init. */
static VmafPicture source_template(const VmafxConvertDesc *d)
{
    VmafPicture pic = {0};
    pic.pix_fmt = vmafx_engine_pixel_format(d->src_pix_fmt);
    pic.bpc = d->src_bpc;
    vmaf_picture_plane_extents(pic.pix_fmt, d->src_w, d->src_h, pic.w, pic.h);
    return pic;
}

static VmafPictureConvertTarget engine_target(const VmafxConvertDesc *d)
{
    VmafPictureConvertTarget target = {0};
    target.pix_fmt = vmafx_engine_pixel_format(d->dst_pix_fmt);
    target.bpc = d->dst_bpc;
    target.w = d->dst_w;
    target.h = d->dst_h;
    target.color = engine_color(&d->dst_color);
    target.resample_filter = (enum VmafResampleFilter)d->filter;
    return target;
}

static VmafxStatus converter_failure(const VmafxReport *report, int err, const char *subject,
                                     const char *what)
{
    return VMAFX_FAIL(report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_PARAMETER, subject,
                      "%s failed (%d)", what, err);
}

VmafxStatus vmafx_frame_converter_create(const VmafxConvertDesc *desc, VmafxFrameConverter **out,
                                         VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!desc || !out) {
        /* The engine names what it refuses first: -ENOTSUP without zimg. */
        const int err = vmaf_engine_picture_convert_context_init_with_color(NULL, NULL, NULL, NULL);
        return converter_failure(&report, err, !desc ? "desc" : "out", "creating a converter");
    }
    *out = NULL;
    VmafxConvertDesc d = VMAFX_CONVERT_DESC_INIT;
    const VmafxStatus status =
        vmafx_read_sized(&report, &d, (uint32_t)sizeof(d), desc, VMAFX_MIN_CONVERT_DESC, "desc");
    if (status != VMAFX_OK) {
        return status;
    }
    const VmafPicture src = source_template(&d);
    const VmafColor src_color = engine_color(&d.src_color);
    const VmafPictureConvertTarget target = engine_target(&d);
    VmafPictureConvertContext *engine = NULL;
    const int err =
        vmaf_engine_picture_convert_context_init_with_color(&engine, &src, &src_color, &target);
    if (err) {
        return converter_failure(&report, err, "desc", "creating a converter");
    }
    *out = (VmafxFrameConverter *)engine;
    return VMAFX_OK;
}

VmafxStatus vmafx_frame_convert(VmafxFrameConverter *converter, const VmafxFrame *src,
                                VmafxFrame **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (out) {
        *out = NULL;
    }
    VmafPicture dst = {0};
    const int err = vmaf_engine_picture_convert(engine_converter(converter), out ? &dst : NULL,
                                                src ? &src->pic : NULL);
    if (err) {
        return converter_failure(&report, err, !converter ? "converter" : "src",
                                 "converting a frame");
    }
    VmafxFrame *frame = NULL;
    const VmafxStatus status = vmafx_frame_adopt_picture(&report, &dst, &frame);
    if (status != VMAFX_OK) {
        (void)vmaf_engine_picture_unref(&dst);
        return status;
    }
    *out = frame;
    return VMAFX_OK;
}

VmafxStatus vmafx_frame_converter_destroy(VmafxFrameConverter *converter, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    const int err = vmaf_engine_picture_convert_context_close(engine_converter(converter));
    if (err) {
        return converter_failure(&report, err, "converter", "releasing a converter");
    }
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
