/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA dispatch_strategy — translates a per-feature descriptor into
 *  a CUDA submission strategy. Today every CUDA extractor uses
 *  direct stream submission; this stub exposes the registry-aware
 *  decision surface so a future PR can opt-in graph capture for
 *  high-dispatch-density features (ADM = 16 dispatches/frame is the
 *  obvious first candidate). See ADR-0181.
 */
#ifndef LIBVMAF_CUDA_DISPATCH_STRATEGY_H_
#define LIBVMAF_CUDA_DISPATCH_STRATEGY_H_

#include "feature/feature_characteristics.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /// Direct stream submission per kernel — current default for
    /// every CUDA extractor.
    VMAF_CUDA_DISPATCH_DIRECT = 0,
    /// Capture the per-frame work into a CUDA graph and launch the
    /// graph instance every frame. Future opt-in for ADM.
    VMAF_CUDA_DISPATCH_GRAPH_CAPTURE,
} VmafCudaDispatchStrategy;

/**
 * Returns the CUDA dispatch strategy for the given feature.
 *
 * @param feature_name  Feature name; used by env-override parser.
 * @param chars         Per-feature characteristics descriptor.
 * @param frame_w       Frame width in pixels.
 * @param frame_h       Frame height in pixels.
 *
 * Env override: VMAF_CUDA_DISPATCH=<feature>:graph,<feature>:direct,...
 *
 * Stub: today returns DIRECT for every input pending profiler-
 * driven graph-capture follow-up (CUDA graph capture support
 * lands separately).
 */
VmafCudaDispatchStrategy vmaf_cuda_select_strategy(const char *feature_name,
                                                   const VmafFeatureCharacteristics *chars,
                                                   unsigned frame_w, unsigned frame_h);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_CUDA_DISPATCH_STRATEGY_H_ */
