/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Specification strings of the filters (RC4 WP9, ADR-1852 decisions 4-5):
 * the `model` and `feature` option values of the FFmpeg filter and the
 * GStreamer element, parsed once here so every consumer of the VMAFx API
 * reads them alike (HISS-19).
 *
 *   model:   key=value[:key=value...]  keys version, path, name, disable_clip,
 *            enable_transform, <extractor>.<option>
 *   feature: <extractor>[=key=value[:key=value...]]
 *            name=<extractor>[:key=value...]  (upstream FFmpeg's spelling)
 *
 * A backslash escapes the next `:`, `=`, `.` or backslash; any other
 * backslash is data, so Windows paths need no escaping.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "error_internal.h"
#include "internal.h"
#include "libvmaf/model.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define SPEC_MAX_BYTES 4096u
#define SPEC_MAX_ITEMS 64u

/* One `key[=value]` item, unescaped; `value` is NULL without `=`. */
typedef struct SpecItem {
    char *key;
    char *value;
} SpecItem;

/* A specification string split into items, on one owned copy. */
typedef struct SpecItems {
    char *buf;
    SpecItem item[SPEC_MAX_ITEMS];
    unsigned n;
} SpecItems;

static bool spec_escapable(char c)
{
    return c == ':' || c == '=' || c == '.' || c == '\\';
}

/* Split `*s` at its first unescaped `sep`: returns the token (NULL when `*s`
 * is NULL) and moves `*s` past the separator, or to NULL at the end. The
 * scan is bounded by the string, at most SPEC_MAX_BYTES. */
static char *spec_split(char **s, char sep)
{
    char *const token = *s;
    if (!token) {
        return NULL;
    }
    for (size_t i = 0; i < SPEC_MAX_BYTES && token[i] != '\0'; i++) {
        if (token[i] == '\\' && spec_escapable(token[i + 1u])) {
            i++;
            continue;
        }
        if (token[i] == sep) {
            token[i] = '\0';
            *s = token + i + 1u;
            return token;
        }
    }
    *s = NULL;
    return token;
}

/* Drop the backslash of every escape in `s`, in place. */
static void spec_unescape(char *s)
{
    if (!s) {
        return;
    }
    char *w = s;
    for (size_t r = 0; r < SPEC_MAX_BYTES && s[r] != '\0'; r++) {
        if (s[r] == '\\' && spec_escapable(s[r + 1u])) {
            r++;
        }
        *w = s[r];
        w++;
    }
    *w = '\0';
}

static void spec_items_free(SpecItems *items)
{
    free(items->buf);
    items->buf = NULL;
    items->n = 0;
}

/* Copy `spec` and split it into `key[=value]` items at `:`. Keys keep their
 * escapes (an override key is split at `.` before it is unescaped). */
static VmafxStatus spec_items_split(const VmafxReport *report, const char *spec, SpecItems *items)
{
    memset(items, 0, sizeof(*items));
    const size_t len = strnlen(spec, SPEC_MAX_BYTES + 1u);
    if (len > SPEC_MAX_BYTES) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PARAMETER, "spec",
                          "the specification is longer than %u bytes", SPEC_MAX_BYTES);
    }
    items->buf = malloc(len + 1u);
    if (!items->buf) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_PARAMETER, "spec",
                          "cannot copy the specification");
    }
    memcpy(items->buf, spec, len + 1u);
    char *rest = len ? items->buf : NULL;
    for (char *tok = spec_split(&rest, ':'); tok; tok = spec_split(&rest, ':')) {
        if (items->n == SPEC_MAX_ITEMS) {
            spec_items_free(items);
            return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PARAMETER, "spec",
                              "more than %u items", SPEC_MAX_ITEMS);
        }
        SpecItem *const item = &items->item[items->n++];
        item->key = spec_split(&tok, '=');
        item->value = tok;
        spec_unescape(item->value);
    }
    return VMAFX_OK;
}

/* ---- Models ----------------------------------------------------------------- */

/* What a model specification asks for, before anything is loaded. */
typedef struct ModelSpec {
    const char *version;
    const char *path;
    VmafxModelConfig config;
} ModelSpec;

static VmafxStatus spec_bad_item(const VmafxReport *report, const char *key, const char *why)
{
    return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_OPTION, key, "%s", why);
}

/* A flag item: no value or `true` sets it, `false` leaves it. */
static VmafxStatus spec_flag(const VmafxReport *report, const SpecItem *item, uint64_t bit,
                             uint64_t *flags)
{
    if (!item->value || strcmp(item->value, "true") == 0) {
        *flags |= bit;
        return VMAFX_OK;
    }
    return strcmp(item->value, "false") == 0 ?
               VMAFX_OK :
               spec_bad_item(report, item->key, "a flag takes no value, true or false");
}

/* Apply one item that is not an override to `ms`; *is_override when it is. */
static VmafxStatus model_item(const VmafxReport *report, SpecItem *item, ModelSpec *ms,
                              bool *is_override)
{
    *is_override = false;
    spec_unescape(item->key);
    if (strcmp(item->key, "disable_clip") == 0) {
        return spec_flag(report, item, VMAF_MODEL_FLAG_DISABLE_CLIP, &ms->config.flags);
    }
    if (strcmp(item->key, "enable_transform") == 0) {
        return spec_flag(report, item, VMAF_MODEL_FLAG_ENABLE_TRANSFORM, &ms->config.flags);
    }
    if (!item->value) {
        return spec_bad_item(report, item->key, "the item has no value");
    }
    if (strcmp(item->key, "version") == 0) {
        ms->version = item->value;
    } else if (strcmp(item->key, "path") == 0) {
        ms->path = item->value;
    } else if (strcmp(item->key, "name") == 0) {
        ms->config.name = item->value;
    } else {
        *is_override = true;
    }
    return VMAFX_OK;
}

/* Read every item; overrides are marked and applied after the load. */
static VmafxStatus model_spec_read(const VmafxReport *report, SpecItems *items, ModelSpec *ms,
                                   bool overrides[SPEC_MAX_ITEMS])
{
    VmafxStatus status = VMAFX_OK;
    for (unsigned i = 0; i < items->n && status == VMAFX_OK; i++) {
        /* An override key `<extractor>.<option>` is unescaped in parts. */
        if (strchr(items->item[i].key, '.') && items->item[i].value) {
            overrides[i] = true;
            continue;
        }
        status = model_item(report, &items->item[i], ms, &overrides[i]);
    }
    if (status == VMAFX_OK && ms->version && ms->path) {
        status = spec_bad_item(report, "path", "give either version or path, not both");
    }
    return status;
}

/* `<extractor>.<option>=<value>`: merge into the model's options. */
static VmafxStatus model_override(const VmafxReport *report, VmafxModel *model, SpecItem *item,
                                  VmafxError **error)
{
    char *rest = item->key;
    char *const extractor = spec_split(&rest, '.');
    spec_unescape(extractor);
    spec_unescape(rest);
    if (!rest || !*rest || !*extractor) {
        return spec_bad_item(report, extractor, "an override is <extractor>.<option>=<value>");
    }
    VmafxOptions *options = NULL;
    VmafxStatus status = vmafx_options_set(&options, rest, item->value, error);
    if (status == VMAFX_OK) {
        status = vmafx_model_override_feature(model, extractor, options, error);
    }
    vmafx_options_free(options);
    return status;
}

static VmafxStatus model_spec_load(const ModelSpec *ms, VmafxModel **out, VmafxError **error)
{
    if (ms->path) {
        return vmafx_model_load_file(&ms->config, ms->path, out, error);
    }
    const char *const version = ms->version ? ms->version : vmafx_model_default_version();
    return vmafx_model_load(&ms->config, version, out, error);
}

/* The caller's defaults and the items of `spec`. */
static VmafxStatus model_spec_begin(const VmafxReport *report, const VmafxModelConfig *config,
                                    const char *spec, ModelSpec *ms, SpecItems *items)
{
    ms->version = NULL;
    ms->path = NULL;
    ms->config = (VmafxModelConfig)VMAFX_MODEL_CONFIG_INIT;
    if (config) {
        const VmafxStatus status =
            vmafx_read_sized(report, &ms->config, (uint32_t)sizeof(ms->config), config,
                             VMAFX_MIN_MODEL_CONFIG, "config");
        if (status != VMAFX_OK) {
            return status;
        }
    }
    return spec_items_split(report, spec, items);
}

/* Load the model and apply the overrides in order; NULL on failure. */
static VmafxStatus model_spec_build(const VmafxReport *report, const ModelSpec *ms,
                                    SpecItems *items, const bool overrides[SPEC_MAX_ITEMS],
                                    VmafxModel **out, VmafxError **error)
{
    VmafxModel *model = NULL;
    VmafxStatus status = model_spec_load(ms, &model, error);
    for (unsigned i = 0; i < items->n && status == VMAFX_OK; i++) {
        if (overrides[i]) {
            status = model_override(report, model, &items->item[i], error);
        }
    }
    if (status != VMAFX_OK) {
        vmafx_model_unref(model);
        return status;
    }
    assert(model != NULL);
    *out = model;
    return VMAFX_OK;
}

VmafxStatus vmafx_model_load_spec(const VmafxModelConfig *config, const char *spec,
                                  VmafxModel **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!spec || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !spec ? "spec" : "out", "NULL argument");
    }
    *out = NULL;
    ModelSpec ms;
    SpecItems items;
    VmafxStatus status = model_spec_begin(&report, config, spec, &ms, &items);
    if (status != VMAFX_OK) {
        return status;
    }
    bool overrides[SPEC_MAX_ITEMS] = {false};
    status = model_spec_read(&report, &items, &ms, overrides);
    if (status == VMAFX_OK) {
        status = model_spec_build(&report, &ms, &items, overrides, out, error);
    }
    spec_items_free(&items);
    return status;
}

/* ---- Features --------------------------------------------------------------- */

/* The user-facing names the CLI maps to their extractors (cli_parse.cpp
 * cli_feature_aliases[]; RC5 moves the CLI onto this function, #1724). */
static const char *feature_alias(const char *name)
{
    static const struct {
        const char *alias;
        const char *target;
    } aliases[] = {
        {"integer_motion", "motion"}, {"integer_motion2", "motion_v2"},
        {"integer_ssim", "ssim"},     {"integer_ms_ssim", "float_ms_ssim"},
        {"integer_psnr", "psnr"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (strcmp(name, aliases[i].alias) == 0) {
            return aliases[i].target;
        }
    }
    return name;
}

/* The extractor's name and the option the first item carries. The VMAFx
 * form's first item is `<extractor>=<key>=<value>`: its value splits again. */
static VmafxStatus feature_head(const VmafxReport *report, SpecItems *items, const char **name,
                                SpecItem *head)
{
    SpecItem *const item = &items->item[0];
    if (items->n == 0u || !item->key) {
        return spec_bad_item(report, "spec", "the feature has no extractor name");
    }
    spec_unescape(item->key);
    if (strcmp(item->key, "name") == 0 && item->value && !strchr(item->value, '=')) {
        *name = item->value; /* name=<extractor>: upstream FFmpeg's spelling */
        head->key = NULL;
        return VMAFX_OK;
    }
    *name = item->key;
    head->key = NULL;
    if (item->value) {
        char *rest = item->value;
        head->key = spec_split(&rest, '=');
        head->value = rest;
    }
    return **name ? VMAFX_OK : spec_bad_item(report, "spec", "the feature has no extractor name");
}

static VmafxStatus feature_option(const VmafxReport *report, VmafxOptions **options, SpecItem *item,
                                  VmafxError **error)
{
    spec_unescape(item->key);
    if (!item->value || !*item->key) {
        return spec_bad_item(report, item->key, "a feature option is <key>=<value>");
    }
    return vmafx_options_set(options, item->key, item->value, error);
}

VmafxStatus vmafx_context_use_feature_spec(VmafxContext *context, const char *spec,
                                           VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !spec || !*spec) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" : "spec", "NULL or empty argument");
    }
    SpecItems items;
    VmafxStatus status = spec_items_split(&report, spec, &items);
    if (status != VMAFX_OK) {
        return status;
    }
    const char *name = NULL;
    SpecItem head = {NULL, NULL};
    VmafxOptions *options = NULL;
    status = feature_head(&report, &items, &name, &head);
    if (status == VMAFX_OK && head.key) {
        status = feature_option(&report, &options, &head, error);
    }
    for (unsigned i = 1u; i < items.n && status == VMAFX_OK; i++) {
        status = feature_option(&report, &options, &items.item[i], error);
    }
    if (status == VMAFX_OK && name) { /* feature_head() names it on success */
        status = vmafx_context_use_feature(context, feature_alias(name), options, error);
    }
    vmafx_options_free(options);
    spec_items_free(&items);
    return status;
}

/* NOLINTEND(modernize-use-nullptr) */
