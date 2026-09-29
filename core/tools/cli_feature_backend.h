/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * `--feature` backend routing and the backend receipt for the vmaf CLI
 * (ADR-1359). Pure helpers over the public libvmaf API so the CLI unit test
 * can replace libvmaf with fakes.
 */

#ifndef LIBVMAF_TOOLS_CLI_FEATURE_BACKEND_H_
#define LIBVMAF_TOOLS_CLI_FEATURE_BACKEND_H_

#include <stdbool.h>
#include <stddef.h>

#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"

/** Upper bound on the extractors one output receipt lists. */
#define CLI_FEATURE_REPORT_MAX 512U

#ifdef __cplusplus
extern "C" {
#endif

/** True when `--backend` names a device backend (not NULL, "auto" or "cpu"). */
bool cli_backend_is_device(const char *backend);

/** Extractor chosen for one `--feature` entry. */
struct CliFeatureChoice {
    const char *extractor; /**< Name to pass to vmaf_use_feature(). */
    const char *twin_name; /**< Twin libvmaf found, or NULL. */
    int status;            /**< vmaf_feature_backend_twin() result; 0 when no lookup ran. */
};

/**
 * Pick the extractor for `--feature feature_name`. Without a device
 * `--backend` (see cli_backend_is_device()) the name is kept as given and
 * libvmaf is not asked. With one, the backend's twin is chosen when libvmaf
 * reports one that can run this request, else the CPU extractor.
 *
 * When the CPU extractor is kept, @p warn receives one warning line naming the
 * feature and the reason. It is left empty when the twin is chosen, and when
 * libvmaf rejects the name or an option value (a twin name such as
 * `ciede_sycl` is not a CPU extractor either): registering the name as given
 * then either works or reports that error. Print the warning before the
 * options are passed to vmaf_use_feature(): it may quote an option key they
 * own.
 */
struct CliFeatureChoice cli_choose_feature_extractor(VmafContext *vmaf, const char *backend,
                                                     const char *feature_name,
                                                     const VmafFeatureDictionary *opts_dict,
                                                     const VmafPictureConfiguration *pic_cfg,
                                                     char *warn, size_t warn_sz);

/** Registered extractors and the backend each one ran on. */
struct CliExtractorReport {
    unsigned cnt;
    const char *name[CLI_FEATURE_REPORT_MAX];
    enum VmafBackend backend[CLI_FEATURE_REPORT_MAX];
};

/** Fill @p report from vmaf_registered_feature_extractor(); 0 or a negative errno. */
int cli_collect_extractor_report(VmafContext *vmaf, struct CliExtractorReport *report);

/** "cpu", "cuda", "sycl", "hip" or "metal"; "cpu" for VMAF_BACKEND_UNKNOWN. */
const char *cli_backend_label(enum VmafBackend backend);

/**
 * `backend_used` value: the device backend of the first extractor that ran on
 * a device, or "cpu" when every extractor ran on the CPU.
 */
const char *cli_report_backend_used(const struct CliExtractorReport *report);

/**
 * Format the JSON members `"backend_used": ..., "feature_backends": [...]`
 * into @p buf (snprintf semantics). Returns the length the full text needs,
 * excluding the terminating NUL.
 */
size_t cli_format_backend_members(const struct CliExtractorReport *report, char *buf, size_t sz);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_TOOLS_CLI_FEATURE_BACKEND_H_ */
