/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VmafxOptions (ADR-1852, RC4 WP2): the feature option set of the VMAFx API.
 * It is the libvmaf feature dictionary under its new name (design section
 * 2.11: VmafFeatureDictionary is VmafxOptions), so a set built here passes to
 * the engine without a conversion and the compat layer can bridge the two
 * names by a cast.
 */

#include <errno.h>
#include <stdint.h>

#include "dict.h"
#include "error_internal.h"
#include "internal.h"
#include "libvmaf/feature.h"
#include "options_internal.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

VmafFeatureDictionary **vmafx_options_dictionary(VmafxOptions **options)
{
    return (VmafFeatureDictionary **)options;
}

VmafxStatus vmafx_options_set(VmafxOptions **options, const char *key, const char *value,
                              VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!options || !key || !value) {
        const char *const which = !options ? "options" : !key ? "key" : "value";
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, which,
                          "NULL argument");
    }
    const int err = vmaf_feature_dictionary_set(vmafx_options_dictionary(options), key, value);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_OPTION, key,
                          "cannot set option %s=%s (%d)", key, value, err);
    }
    return VMAFX_OK;
}

void vmafx_options_free(VmafxOptions *options)
{
    VmafxOptions *held = options;
    (void)vmaf_feature_dictionary_free(vmafx_options_dictionary(&held));
}

VmafxStatus vmafx_options_copy(const VmafxReport *report, const VmafxOptions *options,
                               VmafFeatureDictionary **copy)
{
    *copy = NULL;
    if (!options) {
        return VMAFX_OK;
    }
    /* vmaf_dictionary_copy() takes a mutable source but only reads it. */
    VmafDictionary *source = (VmafDictionary *)options;
    const int err = vmaf_dictionary_copy(&source, (VmafDictionary **)copy);
    if (err) {
        (void)vmaf_feature_dictionary_free(copy);
        return VMAFX_FAIL(report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_PARAMETER,
                          "options", "cannot copy the options (%d)", err);
    }
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
