/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * What a VMAFx context scores (ADR-1852, RC4 WP2): extractors, models, model
 * sets and imported scores, and which extractor a feature resolves to
 * (ADR-1359). A context holds a reference to every model and model set it
 * uses until it is destroyed (ADR-1755).
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "engine.h"
#include "error_internal.h"
#include "feature/feature_extractor.h"
#include "internal.h"
#include "model.h"
#include "options_internal.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static const uint64_t device_extractor_flags =
    VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_SYCL | VMAF_FEATURE_EXTRACTOR_HIP |
    VMAF_FEATURE_EXTRACTOR_METAL;

/* The CPU extractor named `extractor`. */
static VmafxStatus cpu_extractor(const VmafxReport *report, const char *extractor,
                                 const VmafFeatureExtractor **fex)
{
    *fex = vmaf_get_feature_extractor_by_name(extractor);
    if (!*fex) {
        return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_EXTRACTOR, extractor,
                          "no feature extractor has this name");
    }
    if ((*fex)->flags & device_extractor_flags) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_EXTRACTOR, extractor,
                          "names a device extractor; name the CPU extractor, the context picks "
                          "its device twin");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_context_use_feature(VmafxContext *context, const char *extractor,
                                      const VmafxOptions *options, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !extractor) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" : "extractor", "NULL argument");
    }
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(extractor);
    if (!fex) {
        return VMAFX_FAIL(&report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_EXTRACTOR, extractor,
                          "no feature extractor has this name");
    }
    VmafFeatureDictionary *copy = NULL;
    const VmafxStatus status = vmafx_options_copy(&report, options, &copy);
    if (status != VMAFX_OK) {
        return status;
    }
    /* Consumes `copy` on every path past its argument checks. */
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_use_feature(context->engine, extractor, copy);
    vmafx_engine_leave(context, previous);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_EXTRACTOR,
                          extractor, "cannot register the extractor (%d)", err);
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_context_use_model(VmafxContext *context, VmafxModel *model, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !model) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" : "model", "NULL argument");
    }
    const VmafxStatus status = vmafx_held_reserve(&report, &context->models);
    if (status != VMAFX_OK) {
        return status;
    }
    assert(context->models.count < context->models.capacity);
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_use_features_from_model(context->engine, model->engine);
    vmafx_engine_leave(context, previous);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_MODEL,
                          model->engine->name, "cannot register the model's extractors (%d)", err);
    }
    vmafx_held_push(&context->models, vmafx_model_ref(model));
    return VMAFX_OK;
}

VmafxStatus vmafx_context_use_model_set(VmafxContext *context, VmafxModelSet *set,
                                        VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !set) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" : "set", "NULL argument");
    }
    const VmafxStatus status = vmafx_held_reserve(&report, &context->model_sets);
    if (status != VMAFX_OK) {
        return status;
    }
    assert(context->model_sets.count < context->model_sets.capacity);
    VmafModelCollection *const collection = vmafx_model_set_engine(set);
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_use_features_from_model_collection(context->engine, collection);
    vmafx_engine_leave(context, previous);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_MODEL,
                          collection->name, "cannot register the set's extractors (%d)", err);
    }
    vmafx_held_push(&context->model_sets, vmafx_model_set_ref(set));
    return VMAFX_OK;
}

VmafxStatus vmafx_context_import_score(VmafxContext *context, const char *feature, uint64_t index,
                                       double value, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !feature) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" : "feature", "NULL argument");
    }
    if (index > UINT_MAX) {
        return VMAFX_FAIL(&report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_FRAME, "index",
                          "frame index %llu exceeds %u", (unsigned long long)index, UINT_MAX);
    }
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err =
        vmaf_engine_import_feature_score(context->engine, feature, value, (unsigned)index);
    vmafx_engine_leave(context, previous);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_FEATURE,
                          feature, "cannot record the score of frame %llu (%d)",
                          (unsigned long long)index, err);
    }
    vmafx_windows_note_index(context, index); /* RC4 WP4: windows this score made final */
    return VMAFX_OK;
}

/* ---- Feature resolution (ADR-1359) ---------------------------------------- */

/* The engine's picture configuration of an optional frame descriptor. */
static VmafxStatus resolve_geometry(const VmafxReport *report, const VmafxFrameDesc *frame,
                                    VmafPictureConfiguration *pic_cfg)
{
    VmafxFrameDesc d = VMAFX_FRAME_DESC_INIT;
    const VmafxStatus status =
        vmafx_read_sized(report, &d, (uint32_t)sizeof(d), frame, VMAFX_MIN_FRAME_DESC, "frame");
    if (status != VMAFX_OK) {
        return status;
    }
    memset(pic_cfg, 0, sizeof(*pic_cfg));
    pic_cfg->pic_params.w = d.w;
    pic_cfg->pic_params.h = d.h;
    pic_cfg->pic_params.bpc = d.bpc;
    pic_cfg->pic_params.pix_fmt = vmafx_engine_pixel_format(d.pix_fmt);
    return VMAFX_OK;
}

/* The answer of the engine's twin lookup as a resolution and a status. */
static VmafxStatus resolve_verdict(const VmafxReport *report, int err, const char *extractor,
                                   VmafxFeatureResolution *full)
{
    if (err == -ENODEV) {
        /* No device backend: the CPU extractor itself computes the feature. */
        full->backend = VMAFX_BACKEND_CPU;
        full->extractor = extractor;
        return VMAFX_OK;
    }
    full->backend = full->extractor ? (uint32_t)vmaf_engine_extractor_backend(full->extractor) :
                                      (uint32_t)VMAFX_BACKEND_CPU;
    if (err == 0) {
        return VMAFX_OK;
    }
    if (err == -ENOENT) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, err, VMAFX_SUBJECT_EXTRACTOR, extractor,
                          "the context's device backend has no twin of this extractor");
    }
    if (err == -ENOTSUP && full->unsupported_option) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, err, VMAFX_SUBJECT_OPTION,
                          full->unsupported_option, "twin %s cannot honour this option",
                          full->extractor);
    }
    return VMAFX_FAIL(report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_FRAME, "frame",
                      "twin %s refuses this frame geometry (%d)",
                      full->extractor ? full->extractor : "(none)", err);
}

VmafxStatus vmafx_feature_resolve(const VmafxContext *context, const char *extractor,
                                  const VmafxOptions *options, const VmafxFrameDesc *frame,
                                  VmafxFeatureResolution *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !extractor || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context   ? "context" :
                          !extractor ? "extractor" :
                                       "out",
                          "NULL argument");
    }
    const VmafFeatureExtractor *fex = NULL;
    VmafxStatus status = cpu_extractor(&report, extractor, &fex);
    VmafPictureConfiguration pic_cfg;
    if (status == VMAFX_OK && frame) {
        status = resolve_geometry(&report, frame, &pic_cfg);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxFeatureResolution full = VMAFX_FEATURE_RESOLUTION_INIT;
    /* Check the caller's struct before the lookup, so that the answer below
     * can always be written. */
    status = vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
    if (status != VMAFX_OK) {
        return status;
    }
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_feature_backend_twin(
        context->engine, extractor, (const VmafFeatureDictionary *)options, frame ? &pic_cfg : NULL,
        &full.extractor, &full.unsupported_option);
    vmafx_engine_leave(context, previous);
    status = resolve_verdict(&report, err, fex->name, &full);
    if (status == VMAFX_OK || status == VMAFX_E_NOTSUP) {
        vmafx_store_sized(out, &full, (uint32_t)sizeof(full));
    }
    return status;
}

/* NOLINTEND(modernize-use-nullptr) */
