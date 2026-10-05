/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VmafxModel and VmafxModelSet (ADR-1852, RC4 WP2): refcounted wrappers of the
 * engine's VmafModel / VmafModelCollection. A wrapper holds one owner of its
 * engine model (ADR-1755); a context that uses a model holds a reference to
 * the wrapper, and the feature collector the engine mounts it on holds its own
 * owner, so the caller may drop its reference at any time.
 *
 * Every load hashes the bytes it parses (SHA-256, design section 2.9): the
 * embedded JSON of a built-in model, the file's bytes otherwise. A built-in
 * model's bytes are its JSON file's bytes under model/ (xxd -i), so both loads of
 * one model report the same hash.
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/path_utf8.h"
#include "error_internal.h"
#include "feature/feature_extractor.h"
#include "internal.h"
#include "model.h"
#include "options_internal.h"
#include "read_json_model.h"
#include "sha256.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Largest model file read; the largest shipped model is far below 1 MiB. */
#define VMAFX_MODEL_FILE_MAX (UINT64_C(64) << 20)
/* Bound of the built-in table walk (HISS-02); the table holds about 20. */
#define VMAFX_MODEL_BUILTIN_MAX 1024u

static const uint64_t known_model_flags =
    VMAFX_MODEL_DISABLE_CLIP | VMAFX_MODEL_ENABLE_TRANSFORM | VMAFX_MODEL_DISABLE_TRANSFORM;

/* ---- Loading ------------------------------------------------------------- */

/* One load: its report, the engine configuration, and where its messages go
 * (a model belongs to no context, so the configuration names the callback).
 * Self-referential once begun (sink.user): never copied. */
typedef struct ModelLoad {
    VmafxReport report;
    VmafModelConfig cfg;
    VmafxLogCallback callback;
    void *user;
    VmafLogSink sink;
    const VmafLogSink *previous;
} ModelLoad;

static void model_log_deliver(enum VmafLogLevel level, const char *message, void *user)
{
    const ModelLoad *const load = user;
    load->callback((uint32_t)level, message, load->user);
}

/* Read the configuration and route the load's messages, failures reported
 * without an error out-parameter included, to its callback (else the process
 * log) until load_end(). On failure nothing is routed and load_end() is not
 * called. */
static VmafxStatus load_begin(ModelLoad *load, const VmafxModelConfig *config, VmafxError **error)
{
    memset(load, 0, sizeof(*load));
    load->report = (VmafxReport)VMAFX_REPORT(NULL, error);
    VmafxModelConfig c = VMAFX_MODEL_CONFIG_INIT;
    if (config) {
        const VmafxStatus status = vmafx_read_sized(&load->report, &c, (uint32_t)sizeof(c), config,
                                                    VMAFX_MIN_MODEL_CONFIG, "config");
        if (status != VMAFX_OK) {
            return status;
        }
    }
    if (c.log_callback) {
        load->callback = c.log_callback;
        load->user = c.log_user;
        load->sink.deliver = model_log_deliver;
        load->sink.user = load;
        load->sink.level = vmafx_engine_log_level(c.log_level);
        load->report.sink = &load->sink;
    }
    if (c.flags & ~known_model_flags) {
        return VMAFX_FAIL(&load->report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          "config.flags", "unknown model flag bits 0x%llx",
                          (unsigned long long)(c.flags & ~known_model_flags));
    }
    load->cfg.name = c.name;
    load->cfg.flags = c.flags;
    load->previous = vmaf_log_swap_thread_sink(load->report.sink);
    return VMAFX_OK;
}

static void load_end(const ModelLoad *load)
{
    (void)vmaf_log_swap_thread_sink(load->previous);
}

/* The output (`has_out`) and the version or path of a load are not NULL. */
static VmafxStatus load_arguments(const ModelLoad *load, bool has_out, const char *source,
                                  const char *source_name)
{
    if (!has_out || !source) {
        return VMAFX_FAIL(&load->report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !has_out ? "out" : source_name, "NULL argument");
    }
    return VMAFX_OK;
}

static bool has_pkl_extension(const char *path)
{
    const char *const ext = strrchr(path, '.');
    return ext && strcmp(ext, ".pkl") == 0;
}

/* The size of the model file at `path`, refused when it cannot be a model. */
static VmafxStatus model_file_size(const VmafxReport *report, const char *path, size_t *size)
{
    if (has_pkl_extension(path)) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PATH, path,
                          "pkl model files are not supported; use the JSON model");
    }
    VmafPathInfo info;
    const int err = vmaf_path_info_utf8(path, &info);
    if (err || !info.is_regular) {
        return VMAFX_FAIL(report, VMAFX_E_IO, err, VMAFX_SUBJECT_PATH, path,
                          "not a readable regular file (%d)", err);
    }
    if (info.size == 0 || info.size > VMAFX_MODEL_FILE_MAX) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PATH, path,
                          "%llu bytes; a model file holds 1 to %llu bytes",
                          (unsigned long long)info.size, (unsigned long long)VMAFX_MODEL_FILE_MAX);
    }
    *size = (size_t)info.size;
    return VMAFX_OK;
}

/* Read all of `file` (`size` bytes) into `data`. */
static bool read_exactly(FILE *file, char *data, size_t size)
{
    return fread(data, 1, size, file) == size && fgetc(file) == EOF && !ferror(file);
}

/* The bytes of the model file at `path`; the caller frees `*data`. */
static VmafxStatus read_model_file(const VmafxReport *report, const char *path, char **data,
                                   size_t *len)
{
    *data = NULL;
    *len = 0;
    VmafxStatus status = model_file_size(report, path, len);
    if (status != VMAFX_OK || *len == 0) {
        /* model_file_size() refuses an empty file, naming the path. */
        return status != VMAFX_OK ? status : VMAFX_E_RANGE;
    }
    FILE *const file = vmaf_fopen_utf8(path, "rb");
    if (!file) {
        return VMAFX_FAIL(report, VMAFX_E_IO, 0, VMAFX_SUBJECT_PATH, path, "cannot open");
    }
    char *const bytes = malloc(*len);
    if (!bytes) {
        (void)fclose(file);
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_PATH, path,
                          "cannot allocate %zu bytes", *len);
    }
    const bool complete = read_exactly(file, bytes, *len);
    if (fclose(file) != 0 || !complete) {
        free(bytes);
        return VMAFX_FAIL(report, VMAFX_E_IO, 0, VMAFX_SUBJECT_PATH, path,
                          "short or failed read of %zu bytes", *len);
    }
    *data = bytes;
    return VMAFX_OK;
}

/* The embedded bytes of built-in model `version`. */
static VmafxStatus builtin_bytes(const VmafxReport *report, const char *version, const char **data,
                                 size_t *len)
{
    const int err = vmaf_model_builtin_data(version, data, len);
    if (err) {
        return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, err, VMAFX_SUBJECT_MODEL, version,
                          "no built-in model has this version; list them with "
                          "vmafx_model_builtin_next()");
    }
    return VMAFX_OK;
}

static VmafxStatus parse_failure(const VmafxReport *report, int err, const char *subject,
                                 const char *expected)
{
    return VMAFX_FAIL(report, err == -ENOMEM ? VMAFX_E_NOMEM : VMAFX_E_INVALID, err,
                      VMAFX_SUBJECT_MODEL, subject, "not a %s (%d)", expected, err);
}

/* A wrapper holding the one owner of `engine` the loader returned; on failure
 * the engine model is released. */
static VmafxStatus wrap_model(const VmafxReport *report, VmafModel *engine, const char *hex,
                              VmafxModel **out)
{
    assert(out != NULL);
    VmafxModel *const model = malloc(sizeof(*model));
    if (!model || vmaf_ref_init(&model->refs) != 0) {
        free(model);
        vmaf_model_destroy(engine);
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_MODEL, "model",
                          "cannot allocate a model");
    }
    model->engine = engine;
    memcpy(model->sha256, hex, sizeof(model->sha256));
    *out = model;
    return VMAFX_OK;
}

static VmafxStatus parse_model(const VmafxReport *report, VmafModelConfig *cfg, const char *data,
                               size_t len, const char *subject, VmafxModel **out)
{
    assert(len <= INT_MAX);
    char hex[VMAFX_SHA256_HEX_CHARS];
    vmafx_sha256_hex(data, len, hex);
    VmafModel *engine = NULL;
    const int err = vmaf_read_json_model_from_buffer(&engine, cfg, data, (int)len);
    if (err) {
        return parse_failure(report, err, subject,
                             "single model (load a set with vmafx_model_set_*)");
    }
    return wrap_model(report, engine, hex, out);
}

VmafxStatus vmafx_model_load(const VmafxModelConfig *config, const char *version, VmafxModel **out,
                             VmafxError **error)
{
    if (out) {
        *out = NULL;
    }
    ModelLoad load;
    VmafxStatus status = load_begin(&load, config, error);
    if (status != VMAFX_OK) {
        return status;
    }
    const char *data = NULL;
    size_t len = 0;
    status = load_arguments(&load, out != NULL, version, "version");
    if (status == VMAFX_OK) {
        status = builtin_bytes(&load.report, version, &data, &len);
    }
    if (status == VMAFX_OK) {
        status = parse_model(&load.report, &load.cfg, data, len, version, out);
    }
    load_end(&load);
    return status;
}

VmafxStatus vmafx_model_load_file(const VmafxModelConfig *config, const char *path,
                                  VmafxModel **out, VmafxError **error)
{
    if (out) {
        *out = NULL;
    }
    ModelLoad load;
    VmafxStatus status = load_begin(&load, config, error);
    if (status != VMAFX_OK) {
        return status;
    }
    char *data = NULL;
    size_t len = 0;
    status = load_arguments(&load, out != NULL, path, "path");
    if (status == VMAFX_OK) {
        status = read_model_file(&load.report, path, &data, &len);
    }
    if (status == VMAFX_OK) {
        status = parse_model(&load.report, &load.cfg, data, len, path, out);
    }
    free(data);
    load_end(&load);
    return status;
}

/* ---- Model queries and lifetime ------------------------------------------ */

/* True when one of the model's features is computed by `extractor`. */
static bool model_uses_extractor(const VmafModel *engine, const char *extractor)
{
    for (unsigned i = 0; i < engine->n_features; i++) {
        const VmafFeatureExtractor *const fex =
            vmaf_get_feature_extractor_by_feature_name(engine->feature[i].name, 0);
        if (fex && strcmp(fex->name, extractor) == 0) {
            return true;
        }
    }
    return false;
}

/* Arguments of an override: the target is unshared and reads `extractor`. */
static VmafxStatus override_check(const VmafxReport *report, VmafRef *const *refs, unsigned n_refs,
                                  const VmafModel *engine, const char *extractor)
{
    for (unsigned i = 0; i < n_refs; i++) {
        const long count = vmaf_ref_load(refs[i]);
        if (count > 1) {
            return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_MODEL, engine->name,
                              "the model is shared (%ld references); a model a context may use "
                              "is immutable",
                              count);
        }
    }
    if (!model_uses_extractor(engine, extractor)) {
        return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_EXTRACTOR, extractor,
                          "model %s reads no feature of this extractor", engine->name);
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_model_override_feature(VmafxModel *model, const char *extractor,
                                         const VmafxOptions *options, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!model || !extractor || !options) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !model     ? "model" :
                          !extractor ? "extractor" :
                                       "options",
                          "NULL argument");
    }
    VmafxStatus status = override_check(&report, &model->refs, 1, model->engine, extractor);
    VmafFeatureDictionary *copy = NULL;
    if (status == VMAFX_OK) {
        status = vmafx_options_copy(&report, options, &copy);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    /* Consumes `copy` on every path past its argument checks. */
    const int err = vmaf_model_feature_overload(model->engine, extractor, copy);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_EXTRACTOR,
                          extractor, "cannot merge the options into model %s (%d)",
                          model->engine->name, err);
    }
    return VMAFX_OK;
}

VmafxModel *vmafx_model_ref(VmafxModel *model)
{
    if (model) {
        vmaf_ref_fetch_increment(model->refs);
    }
    return model;
}

void vmafx_model_unref(VmafxModel *model)
{
    if (!model || vmaf_ref_fetch_decrement(model->refs) != 1) {
        return;
    }
    /* Drops this wrapper's owner; a collector the model is still mounted on
     * keeps its own (ADR-1755). */
    vmaf_model_destroy(model->engine);
    (void)vmaf_ref_close(model->refs);
    free(model);
}

const char *vmafx_model_name(const VmafxModel *model)
{
    return model ? model->engine->name : NULL;
}

uint32_t vmafx_model_feature_count(const VmafxModel *model)
{
    return model ? vmaf_model_feature_count(model->engine) : 0u;
}

const char *vmafx_model_feature_name(const VmafxModel *model, uint32_t index)
{
    return model ? vmaf_model_feature_name(model->engine, index) : NULL;
}

const char *vmafx_model_hash(const VmafxModel *model)
{
    return model ? model->sha256 : NULL;
}

const char *vmafx_model_builtin_next(const char *previous)
{
    const char *version = NULL;
    const void *entry = vmaf_model_version_next(NULL, &version);
    bool found = previous == NULL;
    for (unsigned i = 0; entry && i < VMAFX_MODEL_BUILTIN_MAX; i++) {
        if (found) {
            return version;
        }
        found = strcmp(version, previous) == 0;
        entry = vmaf_model_version_next(entry, &version);
    }
    return NULL;
}

const char *vmafx_model_default_version(void)
{
    return vmaf_default_model_version();
}

/* ---- Model sets ------------------------------------------------------------ */

/* A set of `engine` and its lead (one owner each), hashed as `hex`; on failure
 * both are released. */
static VmafxStatus wrap_set(const VmafxReport *report, VmafModel *lead, VmafModelCollection *engine,
                            const char *hex, VmafxModelSet **out)
{
    assert(out != NULL);
    VmafxModelSet *const set = malloc(sizeof(*set));
    if (!set || vmaf_ref_init(&set->refs) != 0) {
        free(set);
        vmaf_model_destroy(lead);
        vmaf_model_collection_destroy(engine);
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_MODEL, "set",
                          "cannot allocate a model set");
    }
    set->engine = engine;
    memcpy(set->sha256, hex, sizeof(set->sha256));
    const VmafxStatus status = wrap_model(report, lead, hex, &set->lead);
    if (status != VMAFX_OK) {
        vmaf_model_collection_destroy(engine);
        (void)vmaf_ref_close(set->refs);
        free(set);
        return status;
    }
    *out = set;
    return VMAFX_OK;
}

static VmafxStatus parse_set(const VmafxReport *report, VmafModelConfig *cfg, const char *data,
                             size_t len, const char *subject, VmafxModelSet **out)
{
    assert(len <= INT_MAX);
    char hex[VMAFX_SHA256_HEX_CHARS];
    vmafx_sha256_hex(data, len, hex);
    VmafModel *lead = NULL;
    VmafModelCollection *engine = NULL;
    const int err =
        vmaf_read_json_model_collection_from_buffer(&lead, &engine, cfg, data, (int)len);
    if (err) {
        return parse_failure(report, err, subject,
                             "model set (load a single model with vmafx_model_load*)");
    }
    return wrap_set(report, lead, engine, hex, out);
}

VmafxStatus vmafx_model_set_load(const VmafxModelConfig *config, const char *version,
                                 VmafxModelSet **out, VmafxError **error)
{
    if (out) {
        *out = NULL;
    }
    ModelLoad load;
    VmafxStatus status = load_begin(&load, config, error);
    if (status != VMAFX_OK) {
        return status;
    }
    const char *data = NULL;
    size_t len = 0;
    status = load_arguments(&load, out != NULL, version, "version");
    if (status == VMAFX_OK) {
        status = builtin_bytes(&load.report, version, &data, &len);
    }
    if (status == VMAFX_OK) {
        status = parse_set(&load.report, &load.cfg, data, len, version, out);
    }
    load_end(&load);
    return status;
}

VmafxStatus vmafx_model_set_load_file(const VmafxModelConfig *config, const char *path,
                                      VmafxModelSet **out, VmafxError **error)
{
    if (out) {
        *out = NULL;
    }
    ModelLoad load;
    VmafxStatus status = load_begin(&load, config, error);
    if (status != VMAFX_OK) {
        return status;
    }
    char *data = NULL;
    size_t len = 0;
    status = load_arguments(&load, out != NULL, path, "path");
    if (status == VMAFX_OK) {
        status = read_model_file(&load.report, path, &data, &len);
    }
    if (status == VMAFX_OK) {
        status = parse_set(&load.report, &load.cfg, data, len, path, out);
    }
    free(data);
    load_end(&load);
    return status;
}

VmafxStatus vmafx_model_set_override_feature(VmafxModelSet *set, const char *extractor,
                                             const VmafxOptions *options, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!set || !extractor || !options) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !set       ? "set" :
                          !extractor ? "extractor" :
                                       "options",
                          "NULL argument");
    }
    VmafRef *const refs[] = {set->refs, set->lead->refs};
    VmafxStatus status = override_check(&report, refs, 2, set->lead->engine, extractor);
    VmafFeatureDictionary *copy = NULL;
    if (status == VMAFX_OK) {
        status = vmafx_options_copy(&report, options, &copy);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    VmafModelCollection *collection = set->engine;
    /* Consumes `copy` on every path past its argument checks. */
    const int err =
        vmaf_model_collection_feature_overload(set->lead->engine, &collection, extractor, copy);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_EXTRACTOR,
                          extractor, "cannot merge the options into model set %s (%d)",
                          set->engine->name, err);
    }
    return VMAFX_OK;
}

VmafxModelSet *vmafx_model_set_ref(VmafxModelSet *set)
{
    if (set) {
        vmaf_ref_fetch_increment(set->refs);
    }
    return set;
}

void vmafx_model_set_unref(VmafxModelSet *set)
{
    if (!set || vmaf_ref_fetch_decrement(set->refs) != 1) {
        return;
    }
    vmafx_model_unref(set->lead);
    /* Drops the collection's owner of each member; collectors the members
     * are mounted on keep theirs (ADR-1755). */
    vmaf_model_collection_destroy(set->engine);
    (void)vmaf_ref_close(set->refs);
    free(set);
}

VmafxModel *vmafx_model_set_lead(const VmafxModelSet *set)
{
    return set ? set->lead : NULL;
}

uint32_t vmafx_model_set_size(const VmafxModelSet *set)
{
    return set ? set->engine->cnt : 0u;
}

const char *vmafx_model_set_hash(const VmafxModelSet *set)
{
    return set ? set->sha256 : NULL;
}

VmafModelCollection *vmafx_model_set_engine(const VmafxModelSet *set)
{
    return set ? set->engine : NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
