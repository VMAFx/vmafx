/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Provenance records of VMAFx contexts (#2142, ADR-2073, RC4 WP5): what the
 * record holds and the functions of vmafx/provenance.h. The record is read
 * from the engine when it is asked for: the build description
 * (provenance_build.c), the context's options and frames, its device, the
 * models mounted on its feature collector, every feature vector and the
 * producer the collector recorded for it, the caller's annotations and the
 * encode record. provenance_render.c turns it into canonical JSON and digests
 * it.
 *
 * Record queries may run on any thread (design section 2.5): the state's lock
 * serialises them, and the strings of a record point into the context (the
 * collector, the mounted models, the state), so they live as long as it.
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "error_internal.h"
#include "exactness.h"
#include "feature/alias.h"
#include "feature/feature_collector.h"
#include "internal.h"
#include "libvmaf_priv.h"
#include "model.h"
#include "provenance_record.h"
#include "sha256.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define ANNOTATIONS_MAX 256u
#define ANNOTATION_KEY_MAX 64u
#define ANNOTATION_VALUE_MAX 4096u
/* Models a collector mounts (HISS-02); the collector predicts with 32. */
#define MODELS_MAX 4096u
#define ENCODE_RECORD_HEX 64u

/* ---- State ------------------------------------------------------------------ */

int vmafx_provenance_init(VmafxProvenanceState *state)
{
    memset(state, 0, sizeof(*state));
    const int err = pthread_mutex_init(&state->lock, NULL);
    state->lock_ready = err == 0;
    return err ? -err : 0;
}

void vmafx_provenance_release(VmafxProvenanceState *state)
{
    for (uint32_t i = 0; i < state->n_annotations; i++) {
        free(state->annotations[i].key);
        free(state->annotations[i].value);
    }
    free(state->annotations);
    free(state->json);
    if (state->lock_ready) {
        (void)pthread_mutex_destroy(&state->lock);
    }
    memset(state, 0, sizeof(*state));
}

/* The state of a context a record query reads; queries are serialised by its
 * lock, so the logically const query may update the record's text. */
static VmafxProvenanceState *state_of(const VmafxContext *context)
{
    return (VmafxProvenanceState *)&context->provenance;
}

/* ---- Names -------------------------------------------------------------------- */

const char *vmafx_pixel_format_name(uint32_t pix_fmt)
{
    switch (pix_fmt) {
    case VMAFX_PIXEL_FORMAT_YUV420P:
        return "yuv420p";
    case VMAFX_PIXEL_FORMAT_YUV422P:
        return "yuv422p";
    case VMAFX_PIXEL_FORMAT_YUV444P:
        return "yuv444p";
    case VMAFX_PIXEL_FORMAT_YUV400P:
        return "yuv400p";
    case VMAFX_PIXEL_FORMAT_NV12:
        return "nv12";
    case VMAFX_PIXEL_FORMAT_P010:
        return "p010";
    case VMAFX_PIXEL_FORMAT_P016:
        return "p016";
    default:
        return "unknown";
    }
}

const char *vmafx_feature_source_name(uint32_t source)
{
    static const char *const names[] = {"unknown", "extractor", "imported", "model"};
    return source < sizeof(names) / sizeof(names[0]) ? names[source] : "unknown";
}

/* ---- The device a context scores on --------------------------------------------- */

void vmafx_provenance_device(const VmafxContext *context, VmafxProvenanceDevice *out)
{
    assert(context && context->engine && out);
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    (void)vmaf_context_get_backend(context->engine, &backend);
    out->backend = (uint32_t)backend;
    out->index = 0;
    out->name = "cpu";
    out->runtime = vmafx_build_info()->arch;
    if (backend == VMAF_BACKEND_UNKNOWN) {
        return;
    }
    /* A device backend: the attached VMAFx device names itself; a state the
     * caller imported through the libvmaf API is the caller's own device. The
     * runtime version comes with the backend lanes of RC4 WP3. */
    out->runtime = "unknown";
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    if (context->device && vmafx_device_describe(context->device, &info, NULL) == VMAFX_OK &&
        info.name) {
        out->index = context->device->index;
        out->name = info.name;
        return;
    }
    out->index = -1;
    out->name = vmafx_backend_name((uint32_t)backend);
}

/* ---- Features ----------------------------------------------------------------- */

static const char *report_name(const FeatureVector *fv)
{
    return vmaf_feature_name_alias(fv->name);
}

uint32_t vmafx_provenance_features(VmafFeatureCollector *fc, const FeatureVector **out,
                                   uint32_t max)
{
    const uint32_t n = fc->cnt < max ? fc->cnt : max;
    for (uint32_t i = 0; i < n; i++) {
        const FeatureVector *const fv = fc->feature_vector[i];
        uint32_t j = i;
        for (; j > 0 && strcmp(report_name(out[j - 1u]), report_name(fv)) > 0; j--) {
            out[j] = out[j - 1u];
        }
        out[j] = fv;
    }
    return n;
}

static const char *implementation_of(const char *extractor)
{
    static const char suffix[] = "_rust";
    const size_t len = strlen(extractor);
    const size_t n = sizeof(suffix) - 1u;
    return len > n && strcmp(extractor + len - n, suffix) == 0 ? "rust" : "c";
}

void vmafx_provenance_feature(const FeatureVector *fv, const VmafxProvenanceDevice *device,
                              VmafxFeatureProvenance *out)
{
    const bool extractor = fv->source == VMAF_FEATURE_SOURCE_EXTRACTOR && fv->producer;
    const uint32_t backend =
        extractor ? (uint32_t)vmaf_engine_extractor_backend(fv->producer) : VMAFX_BACKEND_CPU;
    const VmafxFeatureProvenance full = {
        .struct_size = (uint32_t)sizeof(VmafxFeatureProvenance),
        .feature = report_name(fv),
        .extractor = fv->producer ? fv->producer : "",
        .implementation = extractor ? implementation_of(fv->producer) : "",
        .backend = backend,
        .device = backend == VMAFX_BACKEND_CPU ? "cpu" : device->name,
        .runtime = backend == VMAFX_BACKEND_CPU ? vmafx_build_info()->arch : device->runtime,
        .options = fv->producer_options ? fv->producer_options : "",
        .exactness = extractor ? vmafx_exactness_text(fv->producer, backend) : "",
        .source = (uint32_t)fv->source,
    };
    *out = full;
}

/* ---- Scores digest -------------------------------------------------------------- */

void vmafx_provenance_scores_digest(const FeatureVector *const *features, uint32_t n,
                                    char out[VMAFX_DIGEST_TEXT_SIZE])
{
    VmafxSha256 sha;
    vmafx_sha256_init(&sha);
    for (uint32_t f = 0; f < n; f++) {
        const FeatureVector *const fv = features[f];
        const char *const name = report_name(fv);
        for (unsigned i = 0; i < fv->capacity; i++) {
            if (!fv->score[i].written) {
                continue;
            }
            uint64_t bits = 0;
            memcpy(&bits, &fv->score[i].value, sizeof(bits));
            char line[48];
            const int len = snprintf(line, sizeof(line), " %u %016" PRIx64 "\n", i, bits);
            vmafx_sha256_update(&sha, name, strlen(name));
            vmafx_sha256_update(&sha, line, len > 0 ? (size_t)len : 0u);
        }
    }
    char hex[VMAFX_SHA256_HEX_CHARS];
    vmafx_sha256_final_hex(&sha, hex);
    (void)snprintf(out, VMAFX_DIGEST_TEXT_SIZE, "sha256:%s", hex);
}

/* ---- Models ------------------------------------------------------------------- */

const VmafModel *vmafx_provenance_model(const VmafFeatureCollector *fc, uint32_t index)
{
    const VmafPredictModel *node = fc->models;
    for (uint32_t i = 0; node && i < index && i < MODELS_MAX; i++) {
        node = node->next;
    }
    return node ? node->model : NULL;
}

uint32_t vmafx_provenance_model_count(const VmafFeatureCollector *fc)
{
    uint32_t n = 0;
    for (const VmafPredictModel *node = fc->models; node && n < MODELS_MAX; node = node->next) {
        n++;
    }
    return n;
}

void vmafx_provenance_model_record(const VmafModel *model, VmafxModelProvenance *out)
{
    const VmafxModelProvenance full = {
        .struct_size = (uint32_t)sizeof(VmafxModelProvenance),
        .name = model->name ? model->name : "",
        .version = model->source ? model->source : "",
        .sha256 = model->sha256,
        .flags = model->load_flags,
        .overrides = model->overrides ? model->overrides : "",
    };
    *out = full;
}

/* ---- The record's scalar fields ------------------------------------------------- */

static void fill_build(VmafxProvenance *rec)
{
    const VmafxBuildInfo *const build = vmafx_build_info();
    rec->abi_major = VMAFX_ABI_VERSION_MAJOR;
    rec->abi_minor = VMAFX_ABI_VERSION_MINOR;
    rec->abi_patch = VMAFX_ABI_VERSION_PATCH;
    rec->version = vmaf_engine_version();
    rec->commit = build->commit;
    rec->build_id = vmafx_build_id();
    rec->compiler = build->compiler;
    rec->build_flags = build->flags;
    rec->fp_policy = build->fp_policy;
    rec->backends = build->backends;
    rec->rust_twins = build->rust_twins;
    rec->simd = vmafx_simd_level();
}

static void fill_run(VmafxProvenance *rec, const VmafEngineRunInfo *run)
{
    rec->n_threads = run->cfg.n_threads;
    rec->n_subsample = run->cfg.n_subsample > 1u ? run->cfg.n_subsample : 1u;
    rec->cpumask = run->cfg.cpumask;
    rec->gpumask = run->cfg.gpumask;
    rec->frame_width = run->w;
    rec->frame_height = run->h;
    rec->pix_fmt = (uint32_t)run->pix_fmt;
    rec->bpc = run->bpc;
    rec->n_frames = run->pic_cnt;
    rec->elapsed_ns = run->elapsed_ns;
}

VmafxStatus vmafx_provenance_scalars(const VmafxReport *report, VmafxContext *context,
                                     const VmafxProvenanceSnapshot *snap, VmafxProvenance *rec)
{
    assert(report && context && snap && rec);
    VmafEngineRunInfo run;
    const int err = vmaf_engine_run_info(context->engine, &run);
    if (err) {
        return VMAFX_FAIL(report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_CONTEXT,
                          "context", "cannot read the run of the context (%d)", err);
    }
    VmafxProvenanceState *const state = &context->provenance;
    const VmafxProvenance init = VMAFX_PROVENANCE_INIT;
    *rec = init;
    fill_build(rec);
    fill_run(rec, &run);
    rec->active_backend = snap->device.backend;
    rec->n_extractors = vmaf_engine_extractor_count(context->engine);
    rec->device_index = snap->device.index;
    rec->device_name = snap->device.name;
    rec->device_runtime = snap->device.runtime;
    rec->n_models = snap->n_models;
    rec->n_features = snap->n_features;
    rec->n_annotations = state->n_annotations;
    rec->encode_record = state->encode_record;
    vmafx_provenance_scores_digest(snap->features, snap->n_features, state->scores_digest);
    rec->scores_digest = state->scores_digest;
    rec->digest = state->digest;
    return VMAFX_OK;
}

/* ---- Snapshot ------------------------------------------------------------------- */

VmafxStatus vmafx_provenance_snapshot_begin(const VmafxReport *report, VmafxContext *context,
                                            VmafxProvenanceSnapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    vmafx_provenance_device(context, &snap->device);
    snap->fc = vmaf_feature_collector_get(context->engine);
    if (!snap->fc) {
        return VMAFX_FAIL(report, VMAFX_E_INTERNAL, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "the context has no feature collector");
    }
    const uint32_t cap = VMAFX_PROVENANCE_FEATURES_MAX;
    snap->features = (const FeatureVector **)malloc(sizeof(*snap->features) * cap);
    if (!snap->features) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "cannot list the features");
    }
    (void)pthread_mutex_lock(&snap->fc->lock);
    snap->locked = true;
    snap->n_features = vmafx_provenance_features(snap->fc, snap->features, cap);
    snap->n_models = vmafx_provenance_model_count(snap->fc);
    return VMAFX_OK;
}

void vmafx_provenance_snapshot_end(VmafxProvenanceSnapshot *snap)
{
    if (snap->locked) {
        (void)pthread_mutex_unlock(&snap->fc->lock);
    }
    free((void *)snap->features);
    memset(snap, 0, sizeof(*snap));
}

/* ---- Public queries ------------------------------------------------------------- */

static VmafxStatus null_argument(const VmafxReport *report, const char *subject)
{
    return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, subject,
                      "NULL argument");
}

VmafxStatus vmafx_context_provenance(const VmafxContext *context, VmafxProvenance *out,
                                     VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !out) {
        return null_argument(&report, context ? "out" : "context");
    }
    VmafxContext *const ctx = (VmafxContext *)context;
    VmafxProvenanceState *const state = state_of(context);
    (void)pthread_mutex_lock(&state->lock);
    VmafxProvenance full;
    char *json = NULL;
    VmafxStatus status =
        vmafx_provenance_render(&report, ctx, VMAFX_PROVENANCE_JSON_CANONICAL, &json, &full);
    free(json);
    if (status == VMAFX_OK) {
        status = vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
    }
    (void)pthread_mutex_unlock(&state->lock);
    return status;
}

static VmafxStatus past_last(const VmafxReport *report, const char *what, uint32_t index,
                             uint32_t count)
{
    return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_PARAMETER, "index",
                      "no %s %u (the context has %u)", what, (unsigned)index, (unsigned)count);
}

VmafxStatus vmafx_context_model_provenance(const VmafxContext *context, uint32_t index,
                                           VmafxModelProvenance *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !out) {
        return null_argument(&report, context ? "out" : "context");
    }
    VmafxProvenanceState *const state = state_of(context);
    (void)pthread_mutex_lock(&state->lock);
    VmafxProvenanceSnapshot snap;
    VmafxStatus status = vmafx_provenance_snapshot_begin(&report, (VmafxContext *)context, &snap);
    VmafxModelProvenance full;
    if (status == VMAFX_OK && index >= snap.n_models) {
        status = past_last(&report, "model", index, snap.n_models);
    }
    if (status == VMAFX_OK) {
        vmafx_provenance_model_record(vmafx_provenance_model(snap.fc, index), &full);
    }
    vmafx_provenance_snapshot_end(&snap);
    if (status == VMAFX_OK) {
        status = vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
    }
    (void)pthread_mutex_unlock(&state->lock);
    return status;
}

/* The feature at `index` in name order, or the one named `feature` (report
 * or collector name) when `feature` is not NULL. */
static VmafxStatus find_feature(const VmafxReport *report, const VmafxProvenanceSnapshot *snap,
                                const char *feature, uint32_t index, VmafxFeatureProvenance *out)
{
    for (uint32_t i = 0; feature && i < snap->n_features; i++) {
        const FeatureVector *const fv = snap->features[i];
        if (strcmp(fv->name, feature) == 0 || strcmp(report_name(fv), feature) == 0) {
            vmafx_provenance_feature(fv, &snap->device, out);
            return VMAFX_OK;
        }
    }
    if (feature) {
        return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_FEATURE, feature,
                          "the context has no score of this feature");
    }
    if (index >= snap->n_features) {
        return past_last(report, "feature", index, snap->n_features);
    }
    vmafx_provenance_feature(snap->features[index], &snap->device, out);
    return VMAFX_OK;
}

static VmafxStatus feature_query(const VmafxContext *context, const char *feature, uint32_t index,
                                 VmafxFeatureProvenance *out, const VmafxReport *report)
{
    VmafxProvenanceState *const state = state_of(context);
    (void)pthread_mutex_lock(&state->lock);
    VmafxProvenanceSnapshot snap;
    VmafxStatus status = vmafx_provenance_snapshot_begin(report, (VmafxContext *)context, &snap);
    VmafxFeatureProvenance full;
    if (status == VMAFX_OK) {
        status = find_feature(report, &snap, feature, index, &full);
    }
    vmafx_provenance_snapshot_end(&snap);
    if (status == VMAFX_OK) {
        status = vmafx_write_sized(report, out, &full, (uint32_t)sizeof(full), "out");
    }
    (void)pthread_mutex_unlock(&state->lock);
    return status;
}

VmafxStatus vmafx_context_feature_provenance(const VmafxContext *context, uint32_t index,
                                             VmafxFeatureProvenance *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !out) {
        return null_argument(&report, context ? "out" : "context");
    }
    return feature_query(context, NULL, index, out, &report);
}

VmafxStatus vmafx_feature_provenance(const VmafxContext *context, const char *feature,
                                     VmafxFeatureProvenance *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !feature || !out) {
        return null_argument(&report, !context ? "context" : !feature ? "feature" : "out");
    }
    return feature_query(context, feature, 0, out, &report);
}

/* ---- Annotations and the encode record ---------------------------------------- */

static bool valid_key(const char *key)
{
    const size_t len = strnlen(key, ANNOTATION_KEY_MAX + 1u);
    if (len == 0 || len > ANNOTATION_KEY_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

static VmafxStatus check_annotation(const VmafxReport *report, const VmafxProvenanceState *state,
                                    const char *key, const char *value)
{
    if (!valid_key(key)) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "key",
                          "an annotation key is 1 to %u characters of a-z, 0-9 and _",
                          ANNOTATION_KEY_MAX);
    }
    if (strnlen(value, ANNOTATION_VALUE_MAX + 1u) > ANNOTATION_VALUE_MAX) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PARAMETER, "value",
                          "an annotation value is at most %u bytes", ANNOTATION_VALUE_MAX);
    }
    if (state->n_annotations >= ANNOTATIONS_MAX) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PARAMETER, "key",
                          "a context holds at most %u annotations", ANNOTATIONS_MAX);
    }
    return VMAFX_OK;
}

static char *copy_text(const char *text)
{
    const size_t len = strlen(text);
    char *const copy = malloc(len + 1u);
    if (copy) {
        memcpy(copy, text, len + 1u);
    }
    return copy;
}

static VmafxStatus append_annotation(const VmafxReport *report, VmafxProvenanceState *state,
                                     const char *key, const char *value)
{
    if (state->n_annotations >= state->annotations_capacity) {
        uint32_t cap = state->annotations_capacity ? state->annotations_capacity * 2u : 8u;
        cap = cap < ANNOTATIONS_MAX ? cap : ANNOTATIONS_MAX;
        VmafxAnnotationEntry *const grown =
            cap > state->n_annotations ? realloc(state->annotations, sizeof(*grown) * cap) : NULL;
        if (!grown) {
            return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_PARAMETER, "key",
                              "cannot store the annotation");
        }
        state->annotations = grown;
        state->annotations_capacity = cap;
    }
    VmafxAnnotationEntry entry = {.key = copy_text(key), .value = copy_text(value)};
    if (!entry.key || !entry.value) {
        free(entry.key);
        free(entry.value);
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_PARAMETER, "key",
                          "cannot store the annotation");
    }
    state->annotations[state->n_annotations++] = entry;
    return VMAFX_OK;
}

VmafxStatus vmafx_context_annotate(VmafxContext *context, const char *key, const char *value,
                                   VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !key || !value) {
        return null_argument(&report, !context ? "context" : !key ? "key" : "value");
    }
    VmafxProvenanceState *const state = &context->provenance;
    (void)pthread_mutex_lock(&state->lock);
    VmafxStatus status = check_annotation(&report, state, key, value);
    if (status == VMAFX_OK) {
        status = append_annotation(&report, state, key, value);
    }
    (void)pthread_mutex_unlock(&state->lock);
    return status;
}

void vmafx_provenance_annotation(const VmafxProvenanceState *state, uint32_t index,
                                 VmafxAnnotation *out)
{
    assert(index < state->n_annotations);
    const VmafxAnnotation full = {
        .struct_size = (uint32_t)sizeof(VmafxAnnotation),
        .key = state->annotations[index].key,
        .value = state->annotations[index].value,
    };
    *out = full;
}

VmafxStatus vmafx_context_annotation(const VmafxContext *context, uint32_t index,
                                     VmafxAnnotation *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !out) {
        return null_argument(&report, context ? "out" : "context");
    }
    VmafxProvenanceState *const state = state_of(context);
    (void)pthread_mutex_lock(&state->lock);
    VmafxStatus status = VMAFX_OK;
    VmafxAnnotation full;
    if (index >= state->n_annotations) {
        status = past_last(&report, "annotation", index, state->n_annotations);
    } else {
        vmafx_provenance_annotation(state, index, &full);
        status = vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
    }
    (void)pthread_mutex_unlock(&state->lock);
    return status;
}

static bool valid_encode_record(const char *record)
{
    static const char prefix[] = "sha256:";
    const size_t n = sizeof(prefix) - 1u;
    if (strnlen(record, n + ENCODE_RECORD_HEX + 1u) != n + ENCODE_RECORD_HEX ||
        strncmp(record, prefix, n) != 0) {
        return false;
    }
    for (size_t i = n; i < n + ENCODE_RECORD_HEX; i++) {
        const char c = record[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

VmafxStatus vmafx_context_set_encode_record(VmafxContext *context, const char *record,
                                            VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context) {
        return null_argument(&report, "context");
    }
    if (record && record[0] && !valid_encode_record(record)) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "record",
                          "an encode record digest is sha256: and 64 lower-case hex digits");
    }
    VmafxProvenanceState *const state = &context->provenance;
    (void)pthread_mutex_lock(&state->lock);
    (void)snprintf(state->encode_record, sizeof(state->encode_record), "%s", record ? record : "");
    (void)pthread_mutex_unlock(&state->lock);
    return VMAFX_OK;
}

VmafxStatus vmafx_context_provenance_json(VmafxContext *context, uint32_t flags, const char **json,
                                          VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !json) {
        return null_argument(&report, context ? "json" : "context");
    }
    *json = NULL;
    if (flags & ~(uint32_t)VMAFX_PROVENANCE_JSON_CANONICAL) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "flags",
                          "unknown flag bits 0x%x", (unsigned)flags);
    }
    VmafxProvenanceState *const state = &context->provenance;
    (void)pthread_mutex_lock(&state->lock);
    char *text = NULL;
    const VmafxStatus status = vmafx_provenance_render(&report, context, flags, &text, NULL);
    if (status == VMAFX_OK) {
        free(state->json);
        state->json = text;
        *json = state->json;
    }
    (void)pthread_mutex_unlock(&state->lock);
    return status;
}

/* NOLINTEND(modernize-use-nullptr) */
