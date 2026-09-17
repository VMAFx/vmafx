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

#include "feature/feature_collector.h"

typedef struct VmafContext VmafContext;

/*
 * Return the feature collector owned by @vmaf, or NULL when @vmaf is NULL.
 * Test binaries use this instead of including libvmaf.c to reach the opaque
 * context layout.
 */
VmafFeatureCollector *vmaf_feature_collector_get(const VmafContext *vmaf);

#endif /* LIBVMAF_PRIV_H */
