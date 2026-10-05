/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Internal entry points of the scoring engine (core/src/libvmaf.c) that the
 * VMAFx API (core/src/vmafx/) is built on (ADR-1852). The libvmaf functions
 * these replace (vmaf_init, vmaf_close, vmaf_feature_score_at_index,
 * vmaf_version) are generated compat shims on the VMAFx API now
 * (core/src/vmafx/compat_libvmaf_gen.c). Nothing here is exported.
 */

#ifndef VMAFX_ENGINE_H
#define VMAFX_ENGINE_H

#include "libvmaf/libvmaf.h"

/* Bound of the provided-feature walk in vmaf_engine_feature_producer()
 * (HISS-02); no extractor declares more than a few dozen. */
#define VMAF_ENGINE_MAX_PROVIDED_FEATURES 256u

struct VmafxContext;

/* The former bodies of vmaf_init(), vmaf_close(),
 * vmaf_feature_score_at_index() and vmaf_version(), unchanged. */
int vmaf_engine_init(VmafContext **vmaf, VmafConfiguration cfg);
int vmaf_engine_close(VmafContext *vmaf);
int vmaf_engine_feature_score_at_index(VmafContext *vmaf, const char *feature_name, double *score,
                                       unsigned index);
const char *vmaf_engine_version(void);

/* The VMAFx context an engine context belongs to (NULL for NULL). */
struct VmafxContext *vmaf_engine_api_owner(const VmafContext *vmaf);
void vmaf_engine_set_api_owner(VmafContext *vmaf, struct VmafxContext *owner);

/* Number of registered feature extractors. */
unsigned vmaf_engine_extractor_count(const VmafContext *vmaf);

/* The first registered extractor whose provided_features names `feature`:
 * 0 and its name and backend, or -ENOENT (name NULL, backend UNKNOWN). */
int vmaf_engine_feature_producer(const VmafContext *vmaf, const char *feature,
                                 const char **extractor, enum VmafBackend *backend);

#endif /* VMAFX_ENGINE_H */
