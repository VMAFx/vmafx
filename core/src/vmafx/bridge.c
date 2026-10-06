/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The libvmaf bridge of the VMAFx API (vmafx/libvmaf_bridge.h, RC4 WP6,
 * ADR-1852 decision D3): libvmaf pictures, models and model collections as
 * VMAFx frames, models and model sets. The libvmaf compat library
 * (core/src/compat/libvmaf/) is written on these and the other exported
 * vmafx_ functions only.
 *
 * A libvmaf picture is a view of a frame: its VmafRef is the frame's
 * reference count (ADR-1906), so a frame handle and a picture copy hold
 * references of one count. A picture the engine made without a frame (a
 * preallocated pool picture, a device import, a conversion result) is adopted
 * by a frame object here: the frame takes over the picture's release and
 * calls it when the last reference goes, after restoring it.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "error_internal.h"
#include "internal.h"
#include "model.h"
#include "picture.h"
#include "ref.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Release of an adopted engine picture: give the picture its own release back
 * and run it, then free the frame object. Runs once, where the last
 * reference is dropped. */
static int adopted_release(VmafPicture *pic, void *cookie)
{
    VmafxFrame *const frame = cookie;
    VmafPicturePrivate *const priv = pic->priv;
    assert(frame != NULL && priv != NULL);
    priv->release_picture = frame->inner_release;
    priv->cookie = frame->inner_cookie;
    int err = 0;
    if (frame->inner_release) {
        err = frame->inner_release(pic, frame->inner_cookie);
    }
    vmafx_frame_signal_released(frame);
    vmafx_device_unref(frame->device);
    free(frame);
    return err;
}

VmafxFrame *vmafx_frame_of_picture(const VmafPicture *pic)
{
    assert(pic != NULL && pic->priv != NULL);
    const VmafPicturePrivate *const priv = pic->priv;
    if (priv->release_picture == vmafx_frame_release || priv->release_picture == adopted_release ||
        vmafx_frame_pool_release_is(priv->release_picture)) {
        return priv->cookie;
    }
    return NULL;
}

VmafxStatus vmafx_frame_adopt_picture(const VmafxReport *report, const VmafPicture *pic,
                                      VmafxFrame **out)
{
    assert(pic != NULL && pic->priv != NULL && pic->ref != NULL);
    VmafxFrame *const existing = vmafx_frame_of_picture(pic);
    if (existing) {
        *out = existing;
        return VMAFX_OK;
    }
    VmafxFrame *const frame = calloc(1, sizeof(*frame));
    if (!frame) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "picture",
                          "cannot allocate a frame for the picture");
    }
    VmafPicturePrivate *const priv = pic->priv;
    frame->pic = *pic;
    frame->inner_release = priv->release_picture;
    frame->inner_cookie = priv->cookie;
    frame->device = vmafx_device_ref(vmafx_device_cpu());
    frame->residency = VMAFX_BACKEND_CPU;
    priv->release_picture = adopted_release;
    priv->cookie = frame;
    *out = frame;
    return VMAFX_OK;
}

VmafxStatus vmafx_frame_from_picture(VmafPicture *picture, VmafxFrame **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!picture || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !picture ? "picture" : "out", "NULL argument");
    }
    *out = NULL;
    if (!picture->ref || !picture->priv) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, "picture",
                          "the picture carries no reference count: it did not come from "
                          "libvmaf (vmaf_picture_alloc(), a preallocated pool, an import)");
    }
    VmafxFrame *frame = NULL;
    const VmafxStatus status = vmafx_frame_adopt_picture(&report, picture, &frame);
    if (status != VMAFX_OK) {
        return status;
    }
    vmaf_ref_fetch_increment(picture->ref);
    *out = frame;
    return VMAFX_OK;
}

VmafxStatus vmafx_frame_to_picture(VmafxFrame *frame, VmafPicture *picture, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!frame || !picture) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !frame ? "frame" : "picture", "NULL argument");
    }
    assert(frame->pic.ref != NULL);
    vmaf_ref_fetch_increment(frame->pic.ref);
    *picture = frame->pic;
    return VMAFX_OK;
}

VmafxModel *vmafx_model_from_libvmaf(VmafModel *model)
{
    return model ? (VmafxModel *)model->api_owner : NULL;
}

VmafModel *vmafx_model_libvmaf_handle(VmafxModel *model)
{
    return model ? model->engine : NULL;
}

VmafxModelSet *vmafx_model_set_from_libvmaf(VmafModelCollection *collection)
{
    return collection ? (VmafxModelSet *)collection->api_owner : NULL;
}

VmafModelCollection *vmafx_model_set_libvmaf_handle(VmafxModelSet *set)
{
    return set ? set->engine : NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
