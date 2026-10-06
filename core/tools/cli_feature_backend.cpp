/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * `--feature` backend routing for the vmaf CLI (ADR-1359).
 */

#include "cli_feature_backend.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace
{

void format_twin_warning(char *warn, size_t warn_sz, const CliFeatureChoice &choice,
                         const char *backend, const char *feature_name, const char *option,
                         const VmafPictureConfiguration *pic_cfg)
{
    int written = 0;
    if (choice.status == -ENOENT) {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: the %s backend has no twin of this "
                           "extractor; computing it on the CPU\n",
                           feature_name, backend);
    } else if (choice.status == -ENOTSUP && option) {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: %s cannot honour option '%s'; "
                           "computing it on the CPU\n",
                           feature_name, choice.twin_name, option);
    } else if (choice.status == -ENOTSUP && pic_cfg) {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: %s cannot run %ux%u %u-bit pictures "
                           "with these options; computing it on the CPU\n",
                           feature_name, choice.twin_name, pic_cfg->pic_params.w,
                           pic_cfg->pic_params.h, pic_cfg->pic_params.bpc);
    } else if (choice.status == -ENODEV) {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: %s feature extraction is disabled "
                           "(non-zero --gpumask); computing it on the CPU\n",
                           feature_name, backend);
    } else {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: could not check for a %s twin "
                           "(error %d); computing it on the CPU\n",
                           feature_name, backend, choice.status);
    }
    if (written < 0)
        warn[0] = '\0';
}

} // namespace

bool cli_backend_is_device(const char *backend)
{
    return backend && strcmp(backend, "auto") != 0 && strcmp(backend, "cpu") != 0;
}

CliFeatureChoice cli_choose_feature_extractor(VmafContext *vmaf, const char *backend,
                                              const char *feature_name,
                                              const VmafFeatureDictionary *opts_dict,
                                              const VmafPictureConfiguration *pic_cfg, char *warn,
                                              size_t warn_sz)
{
    if (warn && warn_sz > 0)
        warn[0] = '\0';
    if (!cli_backend_is_device(backend))
        return CliFeatureChoice{.extractor = feature_name, .twin_name = nullptr, .status = 0};
    const char *twin = nullptr;
    const char *option = nullptr;
    const int status =
        vmaf_feature_backend_twin(vmaf, feature_name, opts_dict, pic_cfg, &twin, &option);
    const CliFeatureChoice choice = {
        .extractor = status == 0 && twin ? twin : feature_name,
        .twin_name = twin,
        .status = status,
    };
    /* -EINVAL: the name is not a CPU extractor (a twin name or an unknown
     * name) or an option value does not parse. Registering the name as given
     * either works or reports that error, so a warning would only add noise. */
    if (choice.extractor == feature_name && status != -EINVAL && warn && warn_sz > 0)
        format_twin_warning(warn, warn_sz, choice, backend, feature_name, option, pic_cfg);
    return choice;
}
