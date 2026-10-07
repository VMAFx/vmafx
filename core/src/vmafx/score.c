/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Synchronous scores of the VMAFx API (ADR-1852, RC4 WP2): per frame and
 * pooled, for features, models and model sets. Every value comes from the
 * engine's own score and pooling code (vmaf_engine_*), so a score read here
 * equals the libvmaf call's bit for bit. A score that is not final yet is
 * VMAFX_PENDING (libvmaf's -EAGAIN): an answer, not a failure, so no error is
 * created and nothing is logged; the output is not written.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include "engine.h"
#include "error_internal.h"
#include "internal.h"
#include "model.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static VmafxStatus check_index(const VmafxReport *report, uint64_t index, const char *subject)
{
    if (index > UINT_MAX) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_FRAME, subject,
                          "frame index %llu exceeds %u", (unsigned long long)index, UINT_MAX);
    }
    return VMAFX_OK;
}

static VmafxStatus check_range(const VmafxReport *report, uint32_t pool, uint64_t first,
                               uint64_t last)
{
    if (pool < VMAFX_POOL_MIN || pool > VMAFX_POOL_PERC20) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "pool",
                          "pool %u is not a VmafxPool method", (unsigned)pool);
    }
    if (first > last) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, "first",
                          "first frame %llu is after last frame %llu", (unsigned long long)first,
                          (unsigned long long)last);
    }
    return check_index(report, last, "last");
}

/* The status of an engine score call: VMAFX_PENDING for -EAGAIN (no error),
 * else a failure naming `subject` (a feature or model name). */
static VmafxStatus score_failure(const VmafxReport *report, int err, uint32_t kind,
                                 const char *subject, uint64_t first, uint64_t last)
{
    if (err == -EAGAIN) {
        return VMAFX_PENDING;
    }
    return VMAFX_FAIL(report, vmafx_status_from_errno(err), err, kind, subject,
                      "no score for frames %llu to %llu (%d)", (unsigned long long)first,
                      (unsigned long long)last, err);
}

static VmafxStatus null_arguments(const VmafxReport *report, const char *which)
{
    return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, which, "NULL argument");
}

static uint32_t active_backend(const VmafxContext *context)
{
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    const int err = vmaf_context_get_backend(context->engine, &backend);
    return err ? (uint32_t)VMAFX_BACKEND_CPU : (uint32_t)backend;
}

/* ---- Per frame ---------------------------------------------------------------- */

VmafxStatus vmafx_feature_score(VmafxContext *context, const char *feature, uint64_t index,
                                VmafxScore *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !feature || !out) {
        return null_arguments(&report, !context ? "context" : !feature ? "feature" : "out");
    }
    VmafxStatus status = check_index(&report, index, "index");
    double value = 0.0;
    if (status == VMAFX_OK) {
        const VmafLogSink *const previous = vmafx_engine_enter(context);
        const int err =
            vmaf_engine_feature_score_at_index(context->engine, feature, &value, (unsigned)index);
        vmafx_engine_leave(previous);
        status = err ? score_failure(&report, err, VMAFX_SUBJECT_FEATURE, feature, index, index) :
                       VMAFX_OK;
    }
    if (status != VMAFX_OK) {
        return status;
    }
    const char *extractor = NULL;
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    /* -ENOENT (an imported score, an option-decorated name) leaves both unset. */
    const int found = vmaf_engine_feature_producer(context->engine, feature, &extractor, &backend);
    VmafxScore full = VMAFX_SCORE_INIT;
    full.backend = found == 0 ? (uint32_t)backend : (uint32_t)VMAFX_BACKEND_CPU;
    full.index = index;
    full.value = value;
    full.feature = feature;
    full.extractor = found == 0 ? extractor : NULL;
    return vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
}

VmafxStatus vmafx_score_frame(VmafxContext *context, const VmafxModel *model, uint64_t index,
                              VmafxScore *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !model || !out) {
        return null_arguments(&report, !context ? "context" : !model ? "model" : "out");
    }
    VmafxStatus status = check_index(&report, index, "index");
    double value = 0.0;
    if (status == VMAFX_OK) {
        const VmafLogSink *const previous = vmafx_engine_enter(context);
        const int err =
            vmaf_engine_score_at_index(context->engine, model->engine, &value, (unsigned)index);
        vmafx_engine_leave(previous);
        status = err ? score_failure(&report, err, VMAFX_SUBJECT_MODEL, model->engine->name, index,
                                     index) :
                       VMAFX_OK;
    }
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxScore full = VMAFX_SCORE_INIT;
    full.backend = active_backend(context);
    full.index = index;
    full.value = value;
    full.feature = model->engine->name;
    full.extractor = NULL;
    return vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
}

static VmafxModelSetScore set_score(const VmafModelCollectionScore *s, const char *name,
                                    uint32_t pool, uint64_t first, uint64_t last)
{
    VmafxModelSetScore full = VMAFX_MODEL_SET_SCORE_INIT;
    full.pool = pool;
    full.first = first;
    full.last = last;
    full.bagging = s->bootstrap.bagging_score;
    full.stddev = s->bootstrap.stddev;
    full.ci95_lo = s->bootstrap.ci.p95.lo;
    full.ci95_hi = s->bootstrap.ci.p95.hi;
    full.name = name;
    return full;
}

VmafxStatus vmafx_score_frame_model_set(VmafxContext *context, const VmafxModelSet *set,
                                        uint64_t index, VmafxModelSetScore *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !set || !out) {
        return null_arguments(&report, !context ? "context" : !set ? "set" : "out");
    }
    VmafModelCollection *const collection = vmafx_model_set_engine(set);
    VmafxStatus status = check_index(&report, index, "index");
    VmafModelCollectionScore s;
    if (status == VMAFX_OK) {
        const VmafLogSink *const previous = vmafx_engine_enter(context);
        const int err = vmaf_engine_score_at_index_model_collection(context->engine, collection, &s,
                                                                    (unsigned)index);
        vmafx_engine_leave(previous);
        status =
            err ? score_failure(&report, err, VMAFX_SUBJECT_MODEL, collection->name, index, index) :
                  VMAFX_OK;
    }
    if (status != VMAFX_OK) {
        return status;
    }
    const VmafxModelSetScore full = set_score(&s, collection->name, VMAFX_POOL_NONE, index, index);
    return vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
}

/* ---- Pooled ------------------------------------------------------------------- */

static VmafxStatus write_pooled(const VmafxReport *report, VmafxPooledScore *out, uint32_t pool,
                                uint64_t first, uint64_t last, double value, const char *feature)
{
    VmafxPooledScore full = VMAFX_POOLED_SCORE_INIT;
    full.pool = pool;
    full.first = first;
    full.last = last;
    full.value = value;
    full.feature = feature;
    return vmafx_write_sized(report, out, &full, (uint32_t)sizeof(full), "out");
}

VmafxStatus vmafx_score_pooled(VmafxContext *context, const VmafxModel *model, uint32_t pool,
                               uint64_t first, uint64_t last, VmafxPooledScore *out,
                               VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !model || !out) {
        return null_arguments(&report, !context ? "context" : !model ? "model" : "out");
    }
    VmafxStatus status = check_range(&report, pool, first, last);
    double value = 0.0;
    if (status == VMAFX_OK) {
        const VmafLogSink *const previous = vmafx_engine_enter(context);
        const int err =
            vmaf_engine_score_pooled(context->engine, model->engine, (enum VmafPoolingMethod)pool,
                                     &value, (unsigned)first, (unsigned)last);
        vmafx_engine_leave(previous);
        status = err ? score_failure(&report, err, VMAFX_SUBJECT_MODEL, model->engine->name, first,
                                     last) :
                       VMAFX_OK;
    }
    return status != VMAFX_OK ?
               status :
               write_pooled(&report, out, pool, first, last, value, model->engine->name);
}

VmafxStatus vmafx_feature_score_pooled(VmafxContext *context, const char *feature, uint32_t pool,
                                       uint64_t first, uint64_t last, VmafxPooledScore *out,
                                       VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !feature || !out) {
        return null_arguments(&report, !context ? "context" : !feature ? "feature" : "out");
    }
    VmafxStatus status = check_range(&report, pool, first, last);
    double value = 0.0;
    if (status == VMAFX_OK) {
        const VmafLogSink *const previous = vmafx_engine_enter(context);
        const int err =
            vmaf_engine_feature_score_pooled(context->engine, feature, (enum VmafPoolingMethod)pool,
                                             &value, (unsigned)first, (unsigned)last);
        vmafx_engine_leave(previous);
        status = err ? score_failure(&report, err, VMAFX_SUBJECT_FEATURE, feature, first, last) :
                       VMAFX_OK;
    }
    return status != VMAFX_OK ? status :
                                write_pooled(&report, out, pool, first, last, value, feature);
}

VmafxStatus vmafx_score_pooled_model_set(VmafxContext *context, const VmafxModelSet *set,
                                         uint32_t pool, uint64_t first, uint64_t last,
                                         VmafxModelSetScore *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !set || !out) {
        return null_arguments(&report, !context ? "context" : !set ? "set" : "out");
    }
    VmafModelCollection *const collection = vmafx_model_set_engine(set);
    VmafxStatus status = check_range(&report, pool, first, last);
    VmafModelCollectionScore s;
    if (status == VMAFX_OK) {
        const VmafLogSink *const previous = vmafx_engine_enter(context);
        const int err = vmaf_engine_score_pooled_model_collection(context->engine, collection,
                                                                  (enum VmafPoolingMethod)pool, &s,
                                                                  (unsigned)first, (unsigned)last);
        vmafx_engine_leave(previous);
        status =
            err ? score_failure(&report, err, VMAFX_SUBJECT_MODEL, collection->name, first, last) :
                  VMAFX_OK;
    }
    if (status != VMAFX_OK) {
        return status;
    }
    const VmafxModelSetScore full = set_score(&s, collection->name, pool, first, last);
    return vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
}

/* NOLINTEND(modernize-use-nullptr) */
