/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Context-owned frames and per-frame side data of the VMAFx API (RC4 WP6,
 * ADR-1852 decision D3): what the libvmaf calls vmaf_preallocate_pictures(),
 * vmaf_fetch_preallocated_picture(), vmaf_context_get_backend() and
 * vmaf_set_perceptual_sidedata() did, as context functions.
 *
 * The preallocated frames are the engine's own picture pool, so a context
 * with an imported GPU device state keeps its page-locked host pool and the
 * frame retention check of ADR-1478 (a pool too small for an n-2 reader is
 * refused at preallocation and at a later registration). Each frame handed
 * out adopts the pool picture (bridge.c): its last reference returns the
 * picture to the pool.
 */

#include <assert.h>
#include <limits.h>
#include <stdint.h>

#include "engine.h"
#include "error_internal.h"
#include "internal.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/perceptual_weight.h"
#include "ref.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The engine's failure `err` of `what` as a status naming `subject`. */
static VmafxStatus engine_failure(const VmafxReport *report, int err, uint32_t kind,
                                  const char *subject, const char *what)
{
    return VMAFX_FAIL(report, vmafx_status_from_errno(err), err, kind, subject, "%s failed (%d)",
                      what, err);
}

VmafxStatus vmafx_context_preallocate(VmafxContext *context, const VmafxFrameDesc *desc,
                                      uint32_t count, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "context",
                          "NULL argument");
    }
    VmafxFrameDesc d = VMAFX_FRAME_DESC_INIT;
    const VmafxStatus status = vmafx_frame_read_desc(&report, desc, &d);
    if (status != VMAFX_OK) {
        return status;
    }
    VmafPictureConfiguration cfg = {0};
    cfg.pic_params.w = d.w;
    cfg.pic_params.h = d.h;
    cfg.pic_params.bpc = d.bpc;
    cfg.pic_params.pix_fmt = vmafx_engine_pixel_format(d.pix_fmt);
    cfg.pic_cnt = count;
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_preallocate_pictures(context->engine, cfg);
    vmafx_engine_leave(previous);
    if (err) {
        return engine_failure(&report, err, VMAFX_SUBJECT_PARAMETER, "count",
                              "preallocating the context's frames");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_context_acquire_frame(VmafxContext *context, VmafxFrame **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" : "out", "NULL argument");
    }
    *out = NULL;
    VmafPicture pic = {0};
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_fetch_preallocated_picture(context->engine, &pic);
    vmafx_engine_leave(previous);
    if (err) {
        return engine_failure(&report, err, VMAFX_SUBJECT_CONTEXT, "context",
                              "taking a preallocated frame (none preallocated?)");
    }
    /* A pooled picture carries the pool's reference count and release hook. */
    assert(pic.ref != NULL && pic.priv != NULL);
    VmafxFrame *frame = NULL;
    const VmafxStatus status = vmafx_frame_adopt_picture(&report, &pic, &frame);
    if (status != VMAFX_OK) {
        (void)vmaf_engine_picture_unref(&pic);
        return status;
    }
    assert(frame != NULL);
    *out = frame;
    return VMAFX_OK;
}

uint32_t vmafx_context_backend(const VmafxContext *context)
{
    if (!context) {
        return VMAFX_BACKEND_CPU;
    }
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    /* Cannot fail: the engine context is set. */
    const int err = vmaf_engine_context_get_backend(context->engine, &backend);
    assert(err == 0);
    (void)err;
    return (uint32_t)backend;
}

VmafxStatus vmafx_context_attach_sidedata(VmafxContext *context, uint64_t index, const void *data,
                                          size_t size, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !data) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" : "data", "NULL argument");
    }
    if (index > UINT_MAX) {
        return VMAFX_FAIL(&report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_FRAME, "index",
                          "frame index %llu exceeds %u", (unsigned long long)index, UINT_MAX);
    }
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err =
        vmaf_engine_set_perceptual_sidedata(context->engine, data, size, (unsigned)index);
    vmafx_engine_leave(previous);
    if (err) {
        return engine_failure(&report, err, VMAFX_SUBJECT_PARAMETER, "data",
                              "reading the perceptual side data");
    }
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
