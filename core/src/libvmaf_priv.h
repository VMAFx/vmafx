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

extern "C" {
#else
typedef struct VmafContext VmafContext;
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

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_PRIV_H */
