/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Minimal internal accessor declarations for in-tree tests that need selected
 * libvmaf internals without including libvmaf.c and creating duplicate
 * external-linkage definitions.
 */

#ifndef LIBVMAF_PRIV_H
#define LIBVMAF_PRIV_H

#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "feature/feature_collector.h"

#ifdef __cplusplus
using VmafContext = struct VmafContext;
using VmafFeatureExtractor = struct VmafFeatureExtractor;
using VmafDictionary = struct VmafDictionary;
using VmafPictureConfiguration = struct VmafPictureConfiguration;

extern "C" {
#else
typedef struct VmafContext VmafContext;
typedef struct VmafFeatureExtractor VmafFeatureExtractor;
typedef struct VmafDictionary VmafDictionary;
typedef struct VmafPictureConfiguration VmafPictureConfiguration;
#endif

/*
 * Return the feature collector owned by @vmaf, or NULL when @vmaf is NULL.
 * Test binaries use this instead of including libvmaf.c to reach the opaque
 * context layout.
 */
VmafFeatureCollector *vmaf_feature_collector_get(const VmafContext *vmaf);

/*
 * Test accessors for inspecting internal context state and exercising
 * flush ordering without including libvmaf.c directly.
 */
bool vmaf_context_is_flushed(const VmafContext *vmaf);
bool vmaf_context_has_thread_pool(const VmafContext *vmaf);
int vmaf_context_flush_threaded_for_test(VmafContext *vmaf);
int vmaf_context_flush_for_test(VmafContext *vmaf);

/*
 * Test accessors for ADR-1359 device-twin lookup, gpumask gating,
 * registered feature extractor inspection, and context fallback resolution.
 */
int vmaf_backend_twin_verdict_for_test(const VmafFeatureExtractor *twin, const VmafDictionary *opts,
                                       const VmafPictureConfiguration *pic_cfg,
                                       const char **unsupported_option);
unsigned vmaf_context_fake_backend_for_test(VmafContext *vmaf, void *token);
void vmaf_context_set_gpumask_for_test(VmafContext *vmaf, unsigned gpumask);
int vmaf_context_append_registered_feature_extractor_for_test(VmafContext *vmaf,
                                                              const VmafFeatureExtractor *fex,
                                                              bool allow_context_fallback);
int vmaf_context_resolve_context_fallbacks_for_test(VmafContext *vmaf,
                                                    const VmafPictureConfiguration *pic_cfg);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_PRIV_H */
