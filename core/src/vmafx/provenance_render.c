/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The provenance record as text (#2142, ADR-2073, RC4 WP5): canonical JSON
 * (RFC 8785, provenance_json.h) with the record's digest, the members the
 * JSON report embeds, and the XML report's <provenance> element.
 *
 * The JSON is the proto JSON mapping of the generated `Provenance` message
 * (proto/vmafx_api.proto): one member per field of VmafxProvenance named as
 * the field, 64-bit integers as strings, enums by their lower-case value
 * names, and the proto-only `models`, `features` and `annotations` arrays.
 * The field tables below list every field of the structs; the Meson test
 * test_vmafx_provenance_schema compares them with core/api/vmafx.toml.
 */

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "error_internal.h"
#include "internal.h"
#include "provenance_json.h"
#include "provenance_record.h"
#include "report_fragments.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum FieldKind { F_U32, F_I32, F_U64, F_CSTR, F_BACKEND, F_PIX_FMT, F_SOURCE };

typedef struct FieldDesc {
    const char *key;
    enum FieldKind kind;
    size_t offset;
} FieldDesc;

#define FIELD(type, name, kind) {#name, kind, offsetof(type, name)}

static const FieldDesc provenance_fields[] = {
    FIELD(VmafxProvenance, abi_major, F_U32),
    FIELD(VmafxProvenance, abi_minor, F_U32),
    FIELD(VmafxProvenance, abi_patch, F_U32),
    FIELD(VmafxProvenance, active_backend, F_BACKEND),
    FIELD(VmafxProvenance, n_extractors, F_U32),
    FIELD(VmafxProvenance, version, F_CSTR),
    FIELD(VmafxProvenance, commit, F_CSTR),
    FIELD(VmafxProvenance, build_id, F_CSTR),
    FIELD(VmafxProvenance, compiler, F_CSTR),
    FIELD(VmafxProvenance, build_flags, F_CSTR),
    FIELD(VmafxProvenance, fp_policy, F_CSTR),
    FIELD(VmafxProvenance, backends, F_CSTR),
    FIELD(VmafxProvenance, rust_twins, F_U32),
    FIELD(VmafxProvenance, simd, F_CSTR),
    FIELD(VmafxProvenance, device_index, F_I32),
    FIELD(VmafxProvenance, device_name, F_CSTR),
    FIELD(VmafxProvenance, device_runtime, F_CSTR),
    FIELD(VmafxProvenance, n_threads, F_U32),
    FIELD(VmafxProvenance, n_subsample, F_U32),
    FIELD(VmafxProvenance, cpumask, F_U64),
    FIELD(VmafxProvenance, gpumask, F_U64),
    FIELD(VmafxProvenance, frame_width, F_U32),
    FIELD(VmafxProvenance, frame_height, F_U32),
    FIELD(VmafxProvenance, pix_fmt, F_PIX_FMT),
    FIELD(VmafxProvenance, bpc, F_U32),
    FIELD(VmafxProvenance, n_frames, F_U64),
    FIELD(VmafxProvenance, n_models, F_U32),
    FIELD(VmafxProvenance, n_features, F_U32),
    FIELD(VmafxProvenance, n_annotations, F_U32),
    FIELD(VmafxProvenance, encode_record, F_CSTR),
    FIELD(VmafxProvenance, scores_digest, F_CSTR),
    FIELD(VmafxProvenance, elapsed_ns, F_U64),
    FIELD(VmafxProvenance, digest, F_CSTR),
};

static const FieldDesc model_fields[] = {
    FIELD(VmafxModelProvenance, name, F_CSTR),      FIELD(VmafxModelProvenance, version, F_CSTR),
    FIELD(VmafxModelProvenance, sha256, F_CSTR),    FIELD(VmafxModelProvenance, flags, F_U64),
    FIELD(VmafxModelProvenance, overrides, F_CSTR),
};

static const FieldDesc feature_fields[] = {
    FIELD(VmafxFeatureProvenance, feature, F_CSTR),
    FIELD(VmafxFeatureProvenance, extractor, F_CSTR),
    FIELD(VmafxFeatureProvenance, implementation, F_CSTR),
    FIELD(VmafxFeatureProvenance, backend, F_BACKEND),
    FIELD(VmafxFeatureProvenance, device, F_CSTR),
    FIELD(VmafxFeatureProvenance, runtime, F_CSTR),
    FIELD(VmafxFeatureProvenance, options, F_CSTR),
    FIELD(VmafxFeatureProvenance, exactness, F_CSTR),
    FIELD(VmafxFeatureProvenance, source, F_SOURCE),
};

static const FieldDesc annotation_fields[] = {
    FIELD(VmafxAnnotation, key, F_CSTR),
    FIELD(VmafxAnnotation, value, F_CSTR),
};

#define N_FIELDS(table) (sizeof(table) / sizeof((table)[0]))

/* The members the digest does not cover: itself and the timing. */
static const char *const digest_skip[] = {"digest", "elapsed_ns", NULL};

/* ---- Field values -------------------------------------------------------------- */

static uint32_t read_u32(const void *record, size_t offset)
{
    uint32_t v = 0;
    memcpy(&v, (const char *)record + offset, sizeof(v));
    return v;
}

static const char *read_cstr(const void *record, size_t offset)
{
    const char *s = NULL;
    memcpy((void *)&s, (const char *)record + offset, sizeof(s));
    return s ? s : "";
}

/* The value of a field as text: `*quoted` when JSON writes it as a string. */
static const char *field_text(const void *record, const FieldDesc *f, char buf[32], bool *quoted)
{
    *quoted = f->kind != F_U32 && f->kind != F_I32;
    switch (f->kind) {
    case F_U32:
        (void)snprintf(buf, 32u, "%" PRIu32, read_u32(record, f->offset));
        return buf;
    case F_I32: {
        int32_t v = 0;
        memcpy(&v, (const char *)record + f->offset, sizeof(v));
        (void)snprintf(buf, 32u, "%" PRId32, v);
        return buf;
    }
    case F_U64: {
        uint64_t v = 0;
        memcpy(&v, (const char *)record + f->offset, sizeof(v));
        (void)snprintf(buf, 32u, "%" PRIu64, v);
        return buf;
    }
    case F_BACKEND:
        return vmafx_backend_name(read_u32(record, f->offset));
    case F_PIX_FMT:
        return vmafx_pixel_format_name(read_u32(record, f->offset));
    case F_SOURCE:
        return vmafx_feature_source_name(read_u32(record, f->offset));
    default:
        return read_cstr(record, f->offset);
    }
}

/* Members of every field but those named in the NULL-terminated `skip`. */
static void add_fields(VmafxJsonObject *object, const void *record, const FieldDesc *fields,
                       size_t n, const char *const *skip)
{
    for (size_t i = 0; i < n; i++) {
        bool skipped = false;
        for (size_t s = 0; skip && skip[s] && !skipped; s++) {
            skipped = strcmp(fields[i].key, skip[s]) == 0;
        }
        if (skipped) {
            continue;
        }
        char buf[32];
        bool quoted = false;
        const char *const text = field_text(record, &fields[i], buf, &quoted);
        if (quoted) {
            vmafx_json_object_string(object, fields[i].key, text);
        } else {
            VmafxJsonText number = {0};
            vmafx_json_text_puts(&number, text);
            vmafx_json_object_take(object, fields[i].key, vmafx_json_text_take(&number));
        }
    }
}

static char *record_json(const void *record, const FieldDesc *fields, size_t n)
{
    VmafxJsonObject object;
    vmafx_json_object_init(&object);
    add_fields(&object, record, fields, n, NULL);
    char *const json = vmafx_json_object_finish(&object, NULL);
    vmafx_json_object_release(&object);
    return json;
}

/* ---- Arrays ------------------------------------------------------------------- */

/* `n` items made by `make(i)`, as a JSON array; NULL on failure. */
static char *array_of(uint32_t n, char *(*make)(const void *, uint32_t), const void *arg)
{
    char **const items = (char **)calloc(n ? n : 1u, sizeof(*items));
    if (!items) {
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        items[i] = make(arg, i);
    }
    char *const json = vmafx_json_array(items, n);
    free((void *)items);
    return json;
}

static char *model_item(const void *arg, uint32_t i)
{
    const VmafxProvenanceSnapshot *const snap = arg;
    VmafxModelProvenance model;
    vmafx_provenance_model_record(vmafx_provenance_model(snap->fc, i), &model);
    return record_json(&model, model_fields, N_FIELDS(model_fields));
}

static char *feature_item(const void *arg, uint32_t i)
{
    const VmafxProvenanceSnapshot *const snap = arg;
    VmafxFeatureProvenance feature;
    vmafx_provenance_feature(snap->features[i], &snap->device, &feature);
    return record_json(&feature, feature_fields, N_FIELDS(feature_fields));
}

static char *annotation_item(const void *arg, uint32_t i)
{
    VmafxAnnotation annotation;
    vmafx_provenance_annotation(arg, i, &annotation);
    return record_json(&annotation, annotation_fields, N_FIELDS(annotation_fields));
}

/* ---- The record ---------------------------------------------------------------- */

static char *render_text(VmafxContext *context, const VmafxProvenanceSnapshot *snap,
                         VmafxProvenance *rec, uint32_t flags)
{
    VmafxProvenanceState *const state = &context->provenance;
    VmafxJsonObject object;
    vmafx_json_object_init(&object);
    add_fields(&object, rec, provenance_fields, N_FIELDS(provenance_fields), digest_skip);
    vmafx_json_object_take(&object, "models", array_of(snap->n_models, model_item, snap));
    vmafx_json_object_take(&object, "features", array_of(snap->n_features, feature_item, snap));
    vmafx_json_object_take(&object, "annotations",
                           array_of(state->n_annotations, annotation_item, state));
    char *const canonical = vmafx_json_object_finish(&object, digest_skip);
    if (canonical) {
        vmafx_digest_text(canonical, strlen(canonical), state->digest);
    }
    rec->digest = state->digest;
    if (!canonical || (flags & VMAFX_PROVENANCE_JSON_CANONICAL)) {
        vmafx_json_object_release(&object);
        return canonical;
    }
    vmafx_json_object_string(&object, "digest", state->digest);
    vmafx_json_object_u64(&object, "elapsed_ns", rec->elapsed_ns);
    char *const full = vmafx_json_object_finish(&object, NULL);
    vmafx_json_object_release(&object);
    free(canonical);
    return full;
}

VmafxStatus vmafx_provenance_render(const VmafxReport *report, VmafxContext *context,
                                    uint32_t flags, char **json, VmafxProvenance *rec)
{
    assert(report && context && json);
    *json = NULL;
    VmafxProvenanceSnapshot snap;
    VmafxStatus status = vmafx_provenance_snapshot_begin(report, context, &snap);
    VmafxProvenance full;
    if (status == VMAFX_OK) {
        status = vmafx_provenance_scalars(report, context, &snap, &full);
    }
    if (status == VMAFX_OK) {
        *json = render_text(context, &snap, &full, flags);
        if (!*json) {
            status = VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_CONTEXT, "context",
                                "cannot write the provenance record");
        }
    }
    vmafx_provenance_snapshot_end(&snap);
    if (status == VMAFX_OK && rec) {
        *rec = full;
    }
    return status;
}

/* ---- Report fragments ----------------------------------------------------------- */

/* The full record of the context `vmaf` is bound to, or NULL. */
static char *bound_record(VmafContext *vmaf)
{
    VmafxContext *const context = vmafx_context_from_libvmaf(vmaf);
    if (!context) {
        return NULL;
    }
    VmafxError **const error = NULL; /* failures go to the context's log */
    const VmafxReport report = VMAFX_REPORT(context, error);
    VmafxProvenanceState *const state = &context->provenance;
    (void)pthread_mutex_lock(&state->lock);
    char *json = NULL;
    const VmafxStatus status = vmafx_provenance_render(&report, context, 0u, &json, NULL);
    (void)pthread_mutex_unlock(&state->lock);
    if (status != VMAFX_OK) {
        free(json);
        return NULL;
    }
    return json;
}

void vmafx_backend_receipt(VmafxJsonText *text, const char *const *names, const uint32_t *backends,
                           unsigned n)
{
    const char *used = "cpu";
    vmafx_json_text_puts(text, ",\n  \"feature_backends\": [");
    for (unsigned i = 0; i < n; i++) {
        if (backends[i] != VMAFX_BACKEND_CPU && strcmp(used, "cpu") == 0) {
            used = vmafx_backend_name(backends[i]);
        }
        vmafx_json_text_puts(text, i ? ", {\"extractor\": " : "{\"extractor\": ");
        vmafx_json_text_string(text, names[i]);
        vmafx_json_text_puts(text, ", \"backend\": ");
        vmafx_json_text_string(text, vmafx_backend_name(backends[i]));
        vmafx_json_text_puts(text, "}");
    }
    vmafx_json_text_puts(text, "],\n  \"backend_used\": ");
    vmafx_json_text_string(text, used);
}

/* Extractors one receipt lists (HISS-02), as the CLI's receipt did. */
#define RECEIPT_EXTRACTORS_MAX 512u

/* The receipt of the extractors registered on `vmaf`. */
static void engine_receipt(VmafxJsonText *text, VmafContext *vmaf)
{
    const char **const names = (const char **)calloc(RECEIPT_EXTRACTORS_MAX, sizeof(*names));
    uint32_t *const backends = calloc(RECEIPT_EXTRACTORS_MAX, sizeof(*backends));
    unsigned n = 0;
    const unsigned count = vmaf_engine_extractor_count(vmaf);
    for (unsigned i = 0; names && backends && i < count && n < RECEIPT_EXTRACTORS_MAX; i++) {
        enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
        if (vmaf_engine_registered_feature_extractor(vmaf, i, &names[n], &backend) == 0) {
            backends[n++] = (uint32_t)backend;
        }
    }
    text->failed = text->failed || !names || !backends;
    vmafx_backend_receipt(text, names, backends, n);
    free((void *)names);
    free(backends);
}

char *vmafx_report_json_members(VmafContext *vmaf)
{
    char *const record = vmaf ? bound_record(vmaf) : NULL;
    if (!record) {
        return NULL;
    }
    VmafxJsonText text = {0};
    vmafx_json_text_puts(&text, "  \"provenance\": ");
    vmafx_json_text_puts(&text, record);
    free(record);
    engine_receipt(&text, vmaf);
    return vmafx_json_text_take(&text);
}

/* ---- XML ------------------------------------------------------------------------ */

/* `text` as an XML attribute value: markup characters and the control
 * characters XML allows as references; the others become U+FFFD. */
static void xml_value(VmafxJsonText *out, const char *text)
{
    for (size_t i = 0; text[i] && i < ((size_t)1u << 20); i++) {
        const unsigned char c = (unsigned char)text[i];
        char ref[8];
        switch (c) {
        case '&':
            vmafx_json_text_puts(out, "&amp;");
            break;
        case '<':
            vmafx_json_text_puts(out, "&lt;");
            break;
        case '>':
            vmafx_json_text_puts(out, "&gt;");
            break;
        case '"':
            vmafx_json_text_puts(out, "&quot;");
            break;
        case '\t':
        case '\n':
        case '\r':
            (void)snprintf(ref, sizeof(ref), "&#%u;", (unsigned)c);
            vmafx_json_text_puts(out, ref);
            break;
        default:
            vmafx_json_text_put(out, c < 0x20u ? "\xef\xbf\xbd" : &text[i], c < 0x20u ? 3u : 1u);
            break;
        }
    }
}

static void xml_element(VmafxJsonText *out, const char *indent, const char *tag, const void *record,
                        const FieldDesc *fields, size_t n, bool close)
{
    vmafx_json_text_puts(out, indent);
    vmafx_json_text_puts(out, "<");
    vmafx_json_text_puts(out, tag);
    for (size_t i = 0; i < n; i++) {
        char buf[32];
        bool quoted = false;
        vmafx_json_text_puts(out, " ");
        vmafx_json_text_puts(out, fields[i].key);
        vmafx_json_text_puts(out, "=\"");
        xml_value(out, field_text(record, &fields[i], buf, &quoted));
        vmafx_json_text_puts(out, "\"");
    }
    vmafx_json_text_puts(out, close ? " />\n" : ">\n");
}

static void xml_children(VmafxJsonText *out, const VmafxProvenanceSnapshot *snap,
                         const VmafxProvenanceState *state)
{
    for (uint32_t i = 0; i < snap->n_models; i++) {
        VmafxModelProvenance model;
        vmafx_provenance_model_record(vmafx_provenance_model(snap->fc, i), &model);
        xml_element(out, "    ", "model", &model, model_fields, N_FIELDS(model_fields), true);
    }
    for (uint32_t i = 0; i < snap->n_features; i++) {
        VmafxFeatureProvenance feature;
        vmafx_provenance_feature(snap->features[i], &snap->device, &feature);
        xml_element(out, "    ", "feature", &feature, feature_fields, N_FIELDS(feature_fields),
                    true);
    }
    for (uint32_t i = 0; i < state->n_annotations; i++) {
        VmafxAnnotation annotation;
        vmafx_provenance_annotation(state, i, &annotation);
        xml_element(out, "    ", "annotation", &annotation, annotation_fields,
                    N_FIELDS(annotation_fields), true);
    }
}

static VmafxStatus xml_text(VmafxContext *context, VmafxJsonText *out)
{
    VmafxError **const error = NULL; /* failures go to the context's log */
    const VmafxReport report = VMAFX_REPORT(context, error);
    char *json = NULL;
    VmafxProvenance rec;
    /* The render fills the digest the element carries. */
    VmafxStatus status = vmafx_provenance_render(&report, context, 0u, &json, &rec);
    free(json);
    VmafxProvenanceSnapshot snap;
    if (status == VMAFX_OK) {
        status = vmafx_provenance_snapshot_begin(&report, context, &snap);
        if (status == VMAFX_OK) {
            xml_element(out, "  ", "provenance", &rec, provenance_fields,
                        N_FIELDS(provenance_fields), false);
            xml_children(out, &snap, &context->provenance);
            vmafx_json_text_puts(out, "  </provenance>\n");
        }
        vmafx_provenance_snapshot_end(&snap);
    }
    return status;
}

char *vmafx_report_xml_element(VmafContext *vmaf)
{
    VmafxContext *const context = vmaf ? vmafx_context_from_libvmaf(vmaf) : NULL;
    if (!context) {
        return NULL;
    }
    VmafxProvenanceState *const state = &context->provenance;
    VmafxJsonText text = {0};
    (void)pthread_mutex_lock(&state->lock);
    const VmafxStatus status = xml_text(context, &text);
    (void)pthread_mutex_unlock(&state->lock);
    char *const element = vmafx_json_text_take(&text);
    if (status != VMAFX_OK) {
        free(element);
        return NULL;
    }
    return element;
}

/* NOLINTEND(modernize-use-nullptr) */
