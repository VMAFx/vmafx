/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Global feature-characteristics registry — see ADR-0181.
 *
 *  One descriptor per feature, attached to `VmafFeatureExtractor`.
 *  Backends consume the descriptor through their own
 *  `dispatch_strategy` modules (libvmaf/src/{sycl,cuda,vulkan}/).
 *  The registry is the single source of truth for "what does this
 *  feature look like to a dispatch heuristic"; the per-backend
 *  glue translates that into the backend's own primitive.
 */
#ifndef LIBVMAF_FEATURE_CHARACTERISTICS_H_
#define LIBVMAF_FEATURE_CHARACTERISTICS_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Backend-agnostic dispatch hint. Backends translate this to their
 * own primitive: CUDA graph capture / SYCL graph replay / Vulkan
 * secondary command-buffer reuse / etc.
 */
typedef enum {
    /// No preference; backend picks a sensible default. This is
    /// the conservative choice for new extractors that haven't
    /// been profiled yet — they get current backend behaviour.
    VMAF_FEATURE_DISPATCH_AUTO = 0,
    /// Direct submission per dispatch — best for low-dispatch-
    /// count features (e.g., motion = 2 dispatches/frame) and
    /// for small frames where setup cost dominates.
    VMAF_FEATURE_DISPATCH_DIRECT,
    /// Graph replay / batched submission — best for high-
    /// dispatch-count features (e.g., ADM = 16 dispatches/frame)
    /// and for large frames where per-pixel work dominates.
    VMAF_FEATURE_DISPATCH_BATCHED,
} VmafFeatureDispatchHint;

/**
 * Per-feature characteristics. Drives the per-backend
 * dispatch_strategy modules.
 */
typedef struct VmafFeatureCharacteristics {
    /// Number of distinct kernel dispatches per frame for this
    /// feature. Drives the per-frame fixed-overhead amortisation
    /// calculation. Examples: VIF=4 scales, ADM=16 (scale ×
    /// stage), motion=2 (blur + SAD reduction), psnr=1.
    /// Set to 0 for "unknown / not yet measured".
    unsigned n_dispatches_per_frame;

    /// Pure reduction (no per-pixel kernel work besides the
    /// reduction). Reduction-only kernels benefit least from
    /// graph replay because the per-frame work scales linearly
    /// with pixel count and dominates the fixed setup cost.
    bool is_reduction_only;

    /// Minimum frame area (w * h pixels) above which graph
    /// replay / batching wins versus direct submit. Below this,
    /// fixed per-frame setup overhead dominates the kernel work.
    /// 0 = no preference; backend picks a sensible default
    /// (currently 1280*720 = 921600 area for SYCL graph replay).
    unsigned min_useful_frame_area;

    /// Backend-agnostic hint. AUTO = use backend default; DIRECT
    /// and BATCHED override.
    VmafFeatureDispatchHint dispatch_hint;
} VmafFeatureCharacteristics;

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_FEATURE_CHARACTERISTICS_H_ */
