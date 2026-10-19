/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VMAFx contexts (ADR-1852): create / destroy, the per-context log callback,
 * context options, version and extractor queries (provenance: provenance.c,
 * RC4 WP5), on the scoring engine of core/src/libvmaf.c. libvmaf's
 * vmaf_init(), vmaf_close() and vmaf_version() are generated shims on these
 * (core/src/compat/libvmaf/libvmaf_gen.c, libvmaf.so.3).
 *
 * Logging (RC4 WP2): a context with a log callback receives its own messages
 * (failures reported without an error out-parameter, and what the engine logs
 * on the calling thread during this context's calls) at or below its
 * log_level, and never changes the process log level. A context without one
 * logs to the process log and sets its level, as vmaf_init() always did.
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "error_internal.h"
#include "internal.h"
#include "log.h"
#include "status_gen.h"
#include "thread_locale.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Bound of a held-reference list (HISS-02): far more models than a context
 * can score. */
#define VMAFX_HELD_MAX 4096u

/* ---- Held references -------------------------------------------------------- */

VmafxStatus vmafx_held_reserve(const VmafxReport *report, VmafxHeld *held)
{
    if (held->count < held->capacity) {
        return VMAFX_OK;
    }
    const uint32_t capacity = held->capacity ? held->capacity * 2u : 4u;
    void **const items = capacity <= VMAFX_HELD_MAX ?
                             (void **)realloc((void *)held->items, capacity * sizeof(*items)) :
                             NULL;
    if (!items) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "cannot hold %u more references", (unsigned)capacity);
    }
    held->items = items;
    held->capacity = capacity;
    return VMAFX_OK;
}

void vmafx_held_push(VmafxHeld *held, void *item)
{
    assert(held->count < held->capacity);
    held->items[held->count++] = item;
}

static void release_held(VmafxContext *context)
{
    for (uint32_t i = 0; i < context->models.count; i++) {
        vmafx_model_unref(context->models.items[i]);
    }
    for (uint32_t i = 0; i < context->model_sets.count; i++) {
        vmafx_model_set_unref(context->model_sets.items[i]);
    }
    free((void *)context->models.items);
    free((void *)context->model_sets.items);
    vmafx_context_release_device(context); /* RC4 WP3: vmafx_context_use_device() */
}

/* ---- Logging ----------------------------------------------------------------- */

static void deliver_to_callback(enum VmafLogLevel level, const char *message, void *user)
{
    const VmafxContext *const context = user;
    context->log_callback((uint32_t)level, message, context->log_user);
}

const VmafLogSink *vmafx_context_log_sink(const VmafxContext *context)
{
    return context && context->log_callback ? &context->sink : NULL;
}

const VmafLogSink *vmafx_engine_enter(const VmafxContext *context)
{
    vmafx_context_lock(context); /* RC4 WP4: the completion thread enters too */
    return vmaf_log_swap_thread_sink(vmafx_context_log_sink(context));
}

void vmafx_engine_leave(const VmafxContext *context, const VmafLogSink *previous)
{
    (void)vmaf_log_swap_thread_sink(previous);
    vmafx_context_unlock(context);
}

VmafContext *vmafx_context_engine(const VmafxContext *context)
{
    return context ? context->engine : NULL;
}

/* ---- Create / destroy --------------------------------------------------------- */

static VmafxStatus read_config(const VmafxReport *report, const VmafxContextConfig *config,
                               VmafxContextConfig *cfg)
{
    if (config) {
        const VmafxStatus status = vmafx_read_sized(report, cfg, (uint32_t)sizeof(*cfg), config,
                                                    VMAFX_MIN_CONTEXT_CONFIG, "config");
        if (status != VMAFX_OK) {
            return status;
        }
    }
    if (cfg->log_level > VMAFX_LOG_LEVEL_DEBUG) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "config.log_level",
                          "log level %u is not a VmafxLogLevel", (unsigned)cfg->log_level);
    }
    if (cfg->import_retry_wait_ns > VMAFX_IMPORT_RETRY_WAIT_MAX_NS) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PARAMETER,
                          "config.import_retry_wait_ns",
                          "%llu ns; the import rule waits 1 ns to %llu ns (0: the default, 10 s)",
                          (unsigned long long)cfg->import_retry_wait_ns,
                          (unsigned long long)VMAFX_IMPORT_RETRY_WAIT_MAX_NS);
    }
    return VMAFX_OK;
}

enum VmafLogLevel vmafx_engine_log_level(uint32_t level)
{
    switch (level) {
    case VMAFX_LOG_LEVEL_ERROR:
        return VMAF_LOG_LEVEL_ERROR;
    case VMAFX_LOG_LEVEL_WARNING:
        return VMAF_LOG_LEVEL_WARNING;
    case VMAFX_LOG_LEVEL_INFO:
        return VMAF_LOG_LEVEL_INFO;
    case VMAFX_LOG_LEVEL_DEBUG:
        return VMAF_LOG_LEVEL_DEBUG;
    default:
        return VMAF_LOG_LEVEL_NONE;
    }
}

/* The engine configuration (the engine no longer sets the process log level;
 * vmafx_context_create() does, for a context without a callback). */
static VmafConfiguration engine_config(const VmafxContextConfig *cfg)
{
    VmafConfiguration ecfg;
    memset(&ecfg, 0, sizeof(ecfg));
    ecfg.log_level = vmafx_engine_log_level(cfg->log_level);
    ecfg.n_threads = cfg->n_threads;
    ecfg.n_subsample = cfg->n_subsample;
    ecfg.cpumask = cfg->cpumask;
    ecfg.gpumask = cfg->gpumask;
    return ecfg;
}

static VmafxContext *new_context(const VmafxContextConfig *cfg)
{
    VmafxContext *const context = calloc(1, sizeof(*context));
    if (!context) {
        return NULL;
    }
    if (vmafx_provenance_init(&context->provenance) != 0) {
        free(context);
        return NULL;
    }
    context->log_callback = cfg->log_callback;
    context->log_user = cfg->log_user;
    context->import_retry_wait_ns =
        cfg->import_retry_wait_ns ? cfg->import_retry_wait_ns : VMAFX_IMPORT_RETRY_WAIT_DEFAULT_NS;
    context->sink.deliver = deliver_to_callback;
    context->sink.user = context;
    context->sink.level = vmafx_engine_log_level(cfg->log_level);
    return context;
}

VmafxStatus vmafx_context_create(const VmafxContextConfig *config, VmafxContext **out,
                                 VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "out",
                          "no place to store the context");
    }
    *out = NULL;
    VmafxContextConfig cfg = VMAFX_CONTEXT_CONFIG_INIT;
    const VmafxStatus status = read_config(&report, config, &cfg);
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxContext *const context = new_context(&cfg);
    if (!context || !vmafx_windows_init(context)) {
        if (context) {
            vmafx_provenance_release(&context->provenance);
        }
        free(context);
        return VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "cannot allocate a context");
    }
    if (!cfg.log_callback) {
        /* The process log is this context's log: set its level, as
         * vmaf_init() always did. A context with a callback leaves it. */
        vmaf_set_log_level(vmafx_engine_log_level(cfg.log_level));
    }
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_init(&context->engine, engine_config(&cfg));
    vmafx_engine_leave(context, previous);
    if (err) {
        const VmafxReport own = VMAFX_REPORT(context, error);
        const VmafxStatus failed =
            VMAFX_FAIL(&own, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_CONTEXT, "engine",
                       "engine initialisation failed (%d)", err);
        vmafx_windows_close(context);
        vmafx_provenance_release(&context->provenance);
        free(context);
        return failed;
    }
    vmaf_engine_set_api_owner(context->engine, context);
    /* RC4 WP4: worker jobs wake the window completion thread (ADR-2074). */
    vmaf_engine_set_frame_listener(context->engine, vmafx_windows_frame_final, context->windows);
    *out = context;
    return VMAFX_OK;
}

VmafxStatus vmafx_context_destroy(VmafxContext *context, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "context",
                          "no context");
    }
    assert(context->engine != NULL); /* a context exists only with its engine */
    vmafx_windows_pause(context);    /* RC4 WP4: no window work during the close */
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err = vmaf_engine_close(context->engine);
    vmafx_engine_leave(context, previous);
    if (err) {
        vmafx_windows_resume(context);
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_CONTEXT,
                          "engine", "close failed (%d); the context stays valid for a retry", err);
    }
    /* RC4 WP4: open windows complete with VMAFX_E_INVALID and every callback
     * runs before the context goes; window handles stay the caller's. */
    vmafx_windows_close(context);
    /* The engine released its collector's model owners; drop the context's
     * references (ADR-1755) only now, so a failed close keeps them. */
    release_held(context);
    vmafx_provenance_release(&context->provenance);
    free(context);
    return VMAFX_OK;
}

/* ---- Context options ------------------------------------------------------------ */

static VmafxStatus parse_switch(const VmafxReport *report, const char *key, const char *value,
                                int *enabled)
{
    if (!strcmp(value, "1") || !strcmp(value, "true")) {
        *enabled = 1;
        return VMAFX_OK;
    }
    if (!strcmp(value, "0") || !strcmp(value, "false")) {
        *enabled = 0;
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_OPTION, key,
                      "value \"%s\" is not 0, 1, true or false", value);
}

/* A finite number >= 0, read in the C locale whatever the process locale. */
static VmafxStatus parse_strength(const VmafxReport *report, const char *key, const char *value,
                                  double *strength)
{
    VmafThreadLocaleState *const locale = vmaf_thread_locale_push_c();
    char *end = NULL;
    const double parsed = strtod(value, &end);
    vmaf_thread_locale_pop(locale);
    if (end == value || *end != '\0' || !isfinite(parsed) || parsed < 0.0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_OPTION, key,
                          "value \"%s\" is not a finite number >= 0", value);
    }
    *strength = parsed;
    return VMAFX_OK;
}

/* A switch: `perceptual_weight`, or `check_sample_range` (ADR-1918). */
static bool switch_option(const char *key)
{
    return !strcmp(key, "perceptual_weight") || !strcmp(key, "check_sample_range");
}

/* Parse `value` for `key`; VMAFX_E_NOTFOUND names a key no option has. */
static VmafxStatus parse_option(const VmafxReport *report, const char *key, const char *value,
                                int *enabled, double *strength)
{
    if (switch_option(key)) {
        return parse_switch(report, key, value, enabled);
    }
    if (!strcmp(key, "perceptual_weight_strength")) {
        return parse_strength(report, key, value, strength);
    }
    return VMAFX_FAIL(report, VMAFX_E_NOTFOUND, 0, VMAFX_SUBJECT_OPTION, key,
                      "no context option has this name; the context options are "
                      "perceptual_weight, perceptual_weight_strength and check_sample_range");
}

/* Apply a parsed option to the engine; returns its errno. */
static int apply_option(VmafxContext *context, const char *key, int enabled, double strength)
{
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    int err = 0;
    if (!strcmp(key, "check_sample_range")) {
        err = vmaf_engine_set_sample_range_check_enabled(context->engine, enabled);
    } else if (!strcmp(key, "perceptual_weight")) {
        err = vmaf_engine_set_perceptual_weight_enabled(context->engine, enabled);
    } else {
        err = vmaf_engine_set_perceptual_weight_strength(context->engine, strength);
    }
    vmafx_engine_leave(context, previous);
    return err;
}

VmafxStatus vmafx_context_set_option(VmafxContext *context, const char *key, const char *value,
                                     VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !key || !value) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" :
                          !key     ? "key" :
                                     "value",
                          "NULL argument");
    }
    int enabled = 0;
    double strength = 0.0;
    const VmafxStatus status = parse_option(&report, key, value, &enabled, &strength);
    if (status != VMAFX_OK) {
        return status;
    }
    const int err = apply_option(context, key, enabled, strength);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_OPTION, key,
                          "the engine refused %s=%s (%d)", key, value, err);
    }
    return VMAFX_OK;
}

/* ---- Queries -------------------------------------------------------------------- */

const char *vmafx_version_string(void)
{
    return vmaf_engine_version();
}

void vmafx_abi_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (major) {
        *major = VMAFX_ABI_VERSION_MAJOR;
    }
    if (minor) {
        *minor = VMAFX_ABI_VERSION_MINOR;
    }
    if (patch) {
        *patch = VMAFX_ABI_VERSION_PATCH;
    }
}

VmafxStatus vmafx_context_extractor_info(const VmafxContext *context, uint32_t index,
                                         VmafxExtractorInfo *out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          context ? "out" : "context", "NULL argument");
    }
    const char *name = NULL;
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    const int err =
        vmaf_engine_registered_feature_extractor(context->engine, index, &name, &backend);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_EXTRACTOR,
                          "index", "no registered extractor %u (%d)", (unsigned)index, err);
    }
    VmafxExtractorInfo full = VMAFX_EXTRACTOR_INFO_INIT;
    full.backend = (uint32_t)backend;
    full.name = name;
    return vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
}

uint32_t vmafx_context_extractor_count(const VmafxContext *context)
{
    return context ? vmaf_engine_extractor_count(context->engine) : 0u;
}

uint32_t vmafx_context_frame_retention(const VmafxContext *context)
{
    return context ? vmaf_engine_frame_retention(context->engine) : 0u;
}

VmafxContext *vmafx_context_from_libvmaf(VmafContext *vmaf)
{
    return vmaf_engine_api_owner(vmaf);
}

VmafContext *vmafx_context_libvmaf_handle(VmafxContext *context)
{
    return context ? context->engine : NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
