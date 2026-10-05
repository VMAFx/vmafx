/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VMAFx API, prototype slice (ADR-1852): context create / destroy, version,
 * provenance and extractor queries, and one score call, on the scoring engine
 * of core/src/libvmaf.c. libvmaf's vmaf_init(), vmaf_close(),
 * vmaf_feature_score_at_index() and vmaf_version() are generated shims on
 * these (core/src/vmafx/compat_libvmaf_gen.c).
 *
 * Struct size negotiation: an input struct must hold at least the fields of
 * ABI 0.1; an output struct receives min(its struct_size, ours) bytes and
 * its struct_size is set to what was written.
 */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "error_internal.h"
#include "status_gen.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

struct VmafxContext {
    VmafContext *engine;
};

/* Copy `full` (our complete record of `full_size` bytes) into the caller's
 * `out`, whose first uint32_t is its struct_size. */
static VmafxStatus copy_out(void *out, const void *full, uint32_t full_size, VmafxError **error,
                            const char *name)
{
    uint32_t caller_size = 0;
    memcpy(&caller_size, out, sizeof(caller_size));
    if (caller_size < sizeof(uint32_t)) {
        return vmafx_fail(error, VMAFX_E_INVALID, 0, name, "struct_size %u is too small",
                          (unsigned)caller_size);
    }
    const uint32_t written = caller_size < full_size ? caller_size : full_size;
    memcpy(out, full, written);
    memcpy(out, &written, sizeof(written));
    return VMAFX_OK;
}

static VmafxStatus engine_config(const VmafxContextConfig *config, VmafConfiguration *cfg,
                                 VmafxError **error)
{
    memset(cfg, 0, sizeof(*cfg));
    if (!config) {
        return VMAFX_OK;
    }
    if (config->struct_size < sizeof(VmafxContextConfig)) {
        return vmafx_fail(error, VMAFX_E_INVALID, 0, "config.struct_size",
                          "struct_size %u is below the ABI 0.1 size %u",
                          (unsigned)config->struct_size, (unsigned)sizeof(VmafxContextConfig));
    }
    if (config->log_level > VMAFX_LOG_LEVEL_DEBUG) {
        return vmafx_fail(error, VMAFX_E_INVALID, 0, "config.log_level",
                          "log level %u is not a VmafxLogLevel", (unsigned)config->log_level);
    }
    cfg->log_level = (enum VmafLogLevel)config->log_level;
    cfg->n_threads = config->n_threads;
    cfg->n_subsample = config->n_subsample;
    cfg->cpumask = config->cpumask;
    cfg->gpumask = config->gpumask;
    return VMAFX_OK;
}

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

VmafxStatus vmafx_context_create(const VmafxContextConfig *config, VmafxContext **out,
                                 VmafxError **error)
{
    if (!out) {
        return vmafx_fail(error, VMAFX_E_INVALID, 0, "out", "no place to store the context");
    }
    *out = NULL;
    VmafConfiguration cfg;
    const VmafxStatus status = engine_config(config, &cfg, error);
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxContext *const context = malloc(sizeof(*context));
    if (!context) {
        return vmafx_fail(error, VMAFX_E_NOMEM, 0, "context", "cannot allocate a context");
    }
    const int err = vmaf_engine_init(&context->engine, cfg);
    if (err) {
        free(context);
        return vmafx_fail(error, vmafx_status_from_errno(err), err, "engine",
                          "engine initialisation failed (%d)", err);
    }
    vmaf_engine_set_api_owner(context->engine, context);
    *out = context;
    return VMAFX_OK;
}

VmafxStatus vmafx_context_destroy(VmafxContext *context, VmafxError **error)
{
    if (!context) {
        return vmafx_fail(error, VMAFX_E_INVALID, 0, "context", "no context");
    }
    const int err = vmaf_engine_close(context->engine);
    if (err) {
        return vmafx_fail(error, vmafx_status_from_errno(err), err, "engine",
                          "close failed (%d); the context stays valid for a retry", err);
    }
    free(context);
    return VMAFX_OK;
}

VmafxStatus vmafx_context_provenance(const VmafxContext *context, VmafxProvenance *out,
                                     VmafxError **error)
{
    if (!context || !out) {
        return vmafx_fail(error, VMAFX_E_INVALID, 0, context ? "out" : "context", "NULL argument");
    }
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    const int err = vmaf_context_get_backend(context->engine, &backend);
    if (err) {
        return vmafx_fail(error, vmafx_status_from_errno(err), err, "context",
                          "cannot read the active backend (%d)", err);
    }
    VmafxProvenance full = VMAFX_PROVENANCE_INIT;
    full.abi_major = VMAFX_ABI_VERSION_MAJOR;
    full.abi_minor = VMAFX_ABI_VERSION_MINOR;
    full.abi_patch = VMAFX_ABI_VERSION_PATCH;
    full.active_backend = (uint32_t)backend;
    full.n_extractors = vmaf_engine_extractor_count(context->engine);
    full.version = vmaf_engine_version();
    return copy_out(out, &full, (uint32_t)sizeof(full), error, "out.struct_size");
}

VmafxStatus vmafx_context_extractor_info(const VmafxContext *context, uint32_t index,
                                         VmafxExtractorInfo *out, VmafxError **error)
{
    if (!context || !out) {
        return vmafx_fail(error, VMAFX_E_INVALID, 0, context ? "out" : "context", "NULL argument");
    }
    const char *name = NULL;
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    const int err = vmaf_registered_feature_extractor(context->engine, index, &name, &backend);
    if (err) {
        return vmafx_fail(error, vmafx_status_from_errno(err), err, "index",
                          "no registered extractor %u (%d)", (unsigned)index, err);
    }
    VmafxExtractorInfo full = VMAFX_EXTRACTOR_INFO_INIT;
    full.backend = (uint32_t)backend;
    full.name = name;
    return copy_out(out, &full, (uint32_t)sizeof(full), error, "out.struct_size");
}

VmafxStatus vmafx_feature_score(VmafxContext *context, const char *feature, uint64_t index,
                                VmafxScore *out, VmafxError **error)
{
    if (!context || !feature || !out) {
        return vmafx_fail(error, VMAFX_E_INVALID, 0,
                          !context ? "context" :
                          !feature ? "feature" :
                                     "out",
                          "NULL argument");
    }
    if (index > UINT_MAX) {
        return vmafx_fail(error, VMAFX_E_RANGE, 0, "index", "frame index %llu exceeds %u",
                          (unsigned long long)index, UINT_MAX);
    }
    double value = 0.0;
    const int err =
        vmaf_engine_feature_score_at_index(context->engine, feature, &value, (unsigned)index);
    if (err) {
        return vmafx_fail(error, vmafx_status_from_errno(err), err, feature,
                          "no score for frame %llu (%d)", (unsigned long long)index, err);
    }
    const char *extractor = NULL;
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    (void)vmaf_engine_feature_producer(context->engine, feature, &extractor, &backend);
    VmafxScore full = VMAFX_SCORE_INIT;
    full.backend = (uint32_t)backend;
    full.index = index;
    full.value = value;
    full.feature = feature;
    full.extractor = extractor;
    return copy_out(out, &full, (uint32_t)sizeof(full), error, "out.struct_size");
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
