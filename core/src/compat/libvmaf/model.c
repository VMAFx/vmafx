/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * libvmaf models and model collections on the VMAFx API (ADR-1852 design
 * section 2.11). A VmafModel / VmafModelCollection handle is the engine
 * object of a VmafxModel / VmafxModelSet (vmafx/libvmaf_bridge.h); destroying
 * it drops the caller's reference, and a model a context mounted lives on
 * (ADR-1755). vmaf_model_collection_load*() return the set's lead model with
 * a reference of its own, as libvmaf returned a separate model.
 */

#include <errno.h>
#include <stddef.h>

#include "compat_errno.h"
#include "libvmaf/feature.h"
#include "libvmaf/model.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static VmafxModelConfig model_config(const VmafModelConfig *cfg)
{
    VmafxModelConfig config = VMAFX_MODEL_CONFIG_INIT;
    if (cfg) {
        config.name = cfg->name;
        config.flags = cfg->flags;
    }
    return config;
}

/* libvmaf's answer to a failed load: -EINVAL for an unknown built-in version,
 * a file it cannot open (libvmaf's loader returned -EINVAL when fopen()
 * failed) or a `.pkl` file; else the engine's errno (a parse failure, -ENOMEM). */
static int load_errno(VmafxStatus status, VmafxError *error)
{
    if (status == VMAFX_E_NOTFOUND || status == VMAFX_E_IO || status == VMAFX_E_NOTSUP) {
        vmafx_error_free(error);
        return -EINVAL;
    }
    return compat_errno(status, error);
}

int vmaf_model_load(VmafModel **model, VmafModelConfig *cfg, const char *version)
{
    if (!version || !model) {
        return -EINVAL;
    }
    const VmafxModelConfig config = model_config(cfg);
    VmafxModel *loaded = NULL;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_model_load(&config, version, &loaded, &error);
    if (status != VMAFX_OK) {
        return load_errno(status, error);
    }
    *model = vmafx_model_libvmaf_handle(loaded);
    return 0;
}

int vmaf_model_load_from_path(VmafModel **model, VmafModelConfig *cfg, const char *path)
{
    if (!path || !model) {
        return -EINVAL;
    }
    const VmafxModelConfig config = model_config(cfg);
    VmafxModel *loaded = NULL;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_model_load_file(&config, path, &loaded, &error);
    if (status != VMAFX_OK) {
        return load_errno(status, error);
    }
    *model = vmafx_model_libvmaf_handle(loaded);
    return 0;
}

/* libvmaf's answer to an override: options for an extractor the model reads
 * no feature of were accepted and changed nothing (callers such as the vmaf
 * command line pass one option set to every model), so VMAFX_E_NOTFOUND is
 * success here. The provenance record lists only overrides that apply. */
static int override_errno(VmafxStatus status, VmafxError *error)
{
    if (status == VMAFX_OK || status == VMAFX_E_NOTFOUND) {
        vmafx_error_free(error);
        return 0;
    }
    return compat_errno(status, error);
}

/* libvmaf consumes opts_dict once the arguments are checked. */
int vmaf_model_feature_overload(VmafModel *model, const char *feature_name,
                                VmafFeatureDictionary *opts_dict)
{
    if (!model || !feature_name || !opts_dict) {
        return -EINVAL;
    }
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_model_override_feature(
        vmafx_model_from_libvmaf(model), feature_name, (const VmafxOptions *)opts_dict, &error);
    vmafx_options_free((VmafxOptions *)opts_dict);
    return override_errno(status, error);
}

/* A loaded set as libvmaf's pair: the set, and its lead model with one more
 * reference. */
static void collection_out(VmafxModelSet *set, VmafModel **model,
                           VmafModelCollection **model_collection)
{
    *model_collection = vmafx_model_set_libvmaf_handle(set);
    *model = vmafx_model_libvmaf_handle(vmafx_model_ref(vmafx_model_set_lead(set)));
}

int vmaf_model_collection_load(VmafModel **model, VmafModelCollection **model_collection,
                               VmafModelConfig *cfg, const char *version)
{
    if (!version || !model || !model_collection) {
        return -EINVAL;
    }
    const VmafxModelConfig config = model_config(cfg);
    VmafxModelSet *set = NULL;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_model_set_load(&config, version, &set, &error);
    if (status != VMAFX_OK) {
        return load_errno(status, error);
    }
    collection_out(set, model, model_collection);
    return 0;
}

int vmaf_model_collection_load_from_path(VmafModel **model, VmafModelCollection **model_collection,
                                         VmafModelConfig *cfg, const char *path)
{
    if (!path || !model || !model_collection) {
        return -EINVAL;
    }
    const VmafxModelConfig config = model_config(cfg);
    VmafxModelSet *set = NULL;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_model_set_load_file(&config, path, &set, &error);
    if (status != VMAFX_OK) {
        return load_errno(status, error);
    }
    collection_out(set, model, model_collection);
    return 0;
}

/* libvmaf overloads `model` and every member of the collection; `model` is
 * the collection's lead when it came from vmaf_model_collection_load*(). */
int vmaf_model_collection_feature_overload(VmafModel *model, VmafModelCollection **model_collection,
                                           const char *feature_name,
                                           VmafFeatureDictionary *opts_dict)
{
    if (!model_collection || !*model_collection) {
        return -EINVAL;
    }
    if (!model || !feature_name || !opts_dict) {
        return -EINVAL;
    }
    VmafxModelSet *const set = vmafx_model_set_from_libvmaf(*model_collection);
    VmafxModel *const lead = vmafx_model_from_libvmaf(model);
    const VmafxOptions *const options = (const VmafxOptions *)opts_dict;
    const int own_lead = lead == vmafx_model_set_lead(set);
    /* The caller's lead reference (vmaf_model_collection_load*() handed one
     * out) is lent to the set for the call: the set may only be changed while
     * nothing else holds its models, and the set holds the lead meanwhile. */
    if (own_lead) {
        vmafx_model_unref(lead);
    }
    VmafxError *error = NULL;
    VmafxStatus status = vmafx_model_set_override_feature(set, feature_name, options, &error);
    if (own_lead) {
        (void)vmafx_model_ref(lead);
    } else if (status == VMAFX_OK || status == VMAFX_E_NOTFOUND) {
        vmafx_error_free(error);
        error = NULL;
        status = vmafx_model_override_feature(lead, feature_name, options, &error);
    }
    vmafx_options_free((VmafxOptions *)opts_dict);
    return override_errno(status, error);
}

/* The cursor is the version string itself. libvmaf leaves *version alone at
 * the end of the list. */
const void *vmaf_model_version_next(const void *prev, const char **version)
{
    const char *const next = vmafx_model_builtin_next((const char *)prev);
    if (version && next) {
        *version = next;
    }
    return next;
}

/* NOLINTEND(modernize-use-nullptr) */
