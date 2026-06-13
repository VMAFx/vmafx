/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  SYCL dispatch_strategy — translates a per-feature descriptor into a
 *  SYCL submission strategy (direct submit vs graph replay). Consumed
 *  from libvmaf/src/sycl/common.cpp; replaces the inline
 *  GRAPH_AREA_THRESHOLD logic. See ADR-0181.
 */
#ifndef LIBVMAF_SYCL_DISPATCH_STRATEGY_H_
#define LIBVMAF_SYCL_DISPATCH_STRATEGY_H_

#include "feature/feature_characteristics.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /// Submit each kernel directly without recording into a graph.
    /// Best for low-dispatch-count features, small frames, or when
    /// the per-frame fixed setup cost would dominate the kernel work.
    VMAF_SYCL_DISPATCH_DIRECT = 0,
    /// Record the per-frame work into a SYCL graph and replay it.
    /// Best for high-dispatch-count features (ADM=16/frame) or
    /// large frames where per-pixel work dominates.
    VMAF_SYCL_DISPATCH_GRAPH_REPLAY,
} VmafSyclDispatchStrategy;

/**
 * Returns the SYCL dispatch strategy for the given feature.
 *
 * @param feature_name  Feature name (e.g. "vif", "motion", "adm").
 *                      Used by the env-override parser; may be NULL
 *                      for unnamed features (env override declines).
 * @param chars         Per-feature characteristics descriptor. May be
 *                      NULL → backend default (= GRAPH_REPLAY at
 *                      ≥720p area, DIRECT below).
 * @param frame_w       Frame width in pixels.
 * @param frame_h       Frame height in pixels.
 *
 * Env overrides (highest precedence first):
 *   VMAF_SYCL_DISPATCH=<feature>:graph,<feature>:direct,...
 *     Per-feature override; matches feature_name case-sensitively.
 *   VMAF_SYCL_USE_GRAPH=1   Force GRAPH_REPLAY for every feature
 *                           (legacy alias, deprecated).
 *   VMAF_SYCL_NO_GRAPH=1    Force DIRECT for every feature
 *                           (legacy alias, deprecated).
 */
VmafSyclDispatchStrategy vmaf_sycl_select_strategy(const char *feature_name,
                                                   const VmafFeatureCharacteristics *chars,
                                                   unsigned frame_w, unsigned frame_h);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_SYCL_DISPATCH_STRATEGY_H_ */
