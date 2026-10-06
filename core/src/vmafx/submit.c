/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Frame submission of the VMAFx API (ADR-1852, RC4 WP2): vmafx_submit() hands
 * one reference of each frame to the engine (vmaf_read_pictures() semantics:
 * consumed on every path) and vmafx_flush() finishes the stream. The checks
 * here name what is wrong with a frame before the engine sees it; the engine
 * keeps its own checks.
 *
 * RC4 WP3: before a frame is counted, both inputs must live in the same
 * memory and every registered extractor must be able to read it there
 * (admission, frame_import_admit.c).
 */

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "engine.h"
#include "error_internal.h"
#include "frame_import_hooks.h"
#include "internal.h"
#include "config.h"
#ifdef HAVE_CUDA
#include "cuda/vmafx_cuda.h"
#endif
#ifdef HAVE_HIP
#include "hip/vmafx_hip.h"
#endif
#include "ref.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static VmafxFrameDesc picture_desc(const VmafPicture *pic)
{
    VmafxFrameDesc d = VMAFX_FRAME_DESC_INIT;
    d.pix_fmt = (uint32_t)pic->pix_fmt;
    d.bpc = pic->bpc;
    d.w = pic->w[0];
    d.h = pic->h[0];
    return d;
}

static bool same_geometry(const VmafxFrameDesc *a, const VmafxFrameDesc *b)
{
    return a->pix_fmt == b->pix_fmt && a->bpc == b->bpc && a->w == b->w && a->h == b->h;
}

static VmafxStatus geometry_failure(const VmafxReport *report, const char *subject,
                                    const VmafxFrameDesc *got, const VmafxFrameDesc *want,
                                    const char *want_name)
{
    return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, subject,
                      "%ux%u, %u-bit, pixel format %u; %s is %ux%u, %u-bit, pixel format %u",
                      (unsigned)got->w, (unsigned)got->h, (unsigned)got->bpc,
                      (unsigned)got->pix_fmt, want_name, (unsigned)want->w, (unsigned)want->h,
                      (unsigned)want->bpc, (unsigned)want->pix_fmt);
}

/* The frame pair may follow the frames the context has seen. */
static VmafxStatus check_submit(const VmafxReport *report, const VmafxContext *context,
                                const VmafxFrameDesc *ref, const VmafxFrameDesc *dist,
                                uint64_t index)
{
    if (vmaf_engine_is_flushed(context->engine)) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "the context was flushed; score more frames in a new context");
    }
    if (index > UINT_MAX) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_FRAME, "index",
                          "frame index %llu exceeds %u", (unsigned long long)index, UINT_MAX);
    }
    if (context->have_frame && index <= context->last_index) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, "index",
                          "frame index %llu does not follow %llu; indices increase strictly",
                          (unsigned long long)index, (unsigned long long)context->last_index);
    }
    if (!same_geometry(dist, ref)) {
        return geometry_failure(report, "distorted", dist, ref, "the reference");
    }
    if (context->have_frame && !same_geometry(ref, &context->first_desc)) {
        return geometry_failure(report, "reference", ref, &context->first_desc, "the first frame");
    }
    return VMAFX_OK;
}

/* Both inputs live in the same memory, and the context's extractors read it
 * there (RC4 WP3). */
static VmafxStatus admit_pair(const VmafxReport *report, const VmafxContext *context,
                              const VmafxFrame *reference, const VmafxFrame *distorted)
{
    if (reference->residency != distorted->residency || reference->device != distorted->device) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, "distorted",
                          "the distorted frame is in %s memory of another device than the "
                          "reference frame (%s); both inputs live on one device",
                          vmafx_backend_name(distorted->residency),
                          vmafx_backend_name(reference->residency));
    }
    return vmafx_admit_frame(report, context, reference);
}

/* The planted early-release defect (test switch): signal the frames' release
 * fences when the submit returns, while the engine may still hold them. */
static void signal_release_early(VmafxFrame *reference, VmafxFrame *distorted)
{
    VmafxFrame *const frames[2] = {reference, distorted};
    for (unsigned i = 0; i < 2u; i++) {
        VmafxHostFence *const fence = atomic_load(&frames[i]->released);
        if (fence) {
            vmafx_host_fence_signal(fence);
        }
#ifdef HAVE_CUDA
        if (frames[i]->residency == VMAFX_BACKEND_CUDA) {
            vmafx_cuda_release_early(frames[i]); /* its CUDA_EVENT release fences */
        }
#endif
#ifdef HAVE_HIP
        if (frames[i]->residency == VMAFX_BACKEND_HIP) {
            vmafx_hip_release_early(frames[i]); /* its HIP_EVENT release fences */
        }
#endif
    }
}

/* Hand both pictures to the engine, which releases them on every path
 * (ADR-1431); returns its errno. */
static int engine_read(VmafxContext *context, VmafxFrame *reference, VmafxFrame *distorted,
                       VmafPicture *ref, VmafPicture *dist, uint64_t index)
{
    const bool early = vmafx_test_switch(VMAFX_TEST_EARLY_RELEASE);
    if (early) {
        /* Keep both frames alive across the engine call for the switch. */
        (void)vmafx_frame_ref(reference);
        (void)vmafx_frame_ref(distorted);
    }
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_read_pictures(context->engine, ref, dist, (unsigned)index);
    vmafx_engine_leave(previous);
    if (early) {
        signal_release_early(reference, distorted);
        vmafx_frame_unref(reference);
        vmafx_frame_unref(distorted);
    }
    return err;
}

/* Release the pictures a failed submit took, each once. */
static void drop_pictures(VmafPicture *ref, VmafPicture *dist)
{
    if (ref->ref) {
        (void)vmaf_picture_unref(ref);
    }
    if (dist->ref) {
        (void)vmaf_picture_unref(dist);
    }
}

VmafxStatus vmafx_submit(VmafxContext *context, VmafxFrame *reference, VmafxFrame *distorted,
                         uint64_t index, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    /* From here on the call owns one reference of each frame. */
    VmafPicture ref = reference ? vmafx_frame_take_picture(reference) : (VmafPicture){0};
    VmafPicture dist = distorted ? vmafx_frame_take_picture(distorted) : (VmafPicture){0};
    if (!context || !reference || !distorted) {
        drop_pictures(&ref, &dist);
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context   ? "context" :
                          !reference ? "reference" :
                                       "distorted",
                          "NULL argument");
    }
    if (reference == distorted && vmaf_ref_load(ref.ref) < 2) {
        /* One reference cannot be consumed twice: release it once. */
        (void)vmaf_picture_unref(&ref);
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, "distorted",
                          "the same frame as both inputs needs two references (vmafx_frame_ref)");
    }
    const VmafxFrameDesc ref_desc = picture_desc(&ref);
    const VmafxFrameDesc dist_desc = picture_desc(&dist);
    VmafxStatus status = check_submit(&report, context, &ref_desc, &dist_desc, index);
    if (status == VMAFX_OK) {
        status = admit_pair(&report, context, reference, distorted);
    }
    if (status != VMAFX_OK) {
        drop_pictures(&ref, &dist);
        return status;
    }
    if (!context->have_frame) {
        context->first_desc = ref_desc;
        context->have_frame = true;
    }
    context->last_index = index;
    const int err = engine_read(context, reference, distorted, &ref, &dist, index);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_FRAME, "index",
                          "the engine could not score frame %llu (%d)", (unsigned long long)index,
                          err);
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_flush(VmafxContext *context, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "context",
                          "NULL argument");
    }
    if (vmaf_engine_is_flushed(context->engine)) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "the context was flushed already");
    }
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_read_pictures(context->engine, NULL, NULL, 0);
    vmafx_engine_leave(previous);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_CONTEXT,
                          "context", "flush failed (%d)", err);
    }
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
