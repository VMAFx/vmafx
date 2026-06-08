/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Dispatch strategy stub for the HIP backend (ADR-0212 / T7-10).
 *  Mirrors libvmaf/src/vulkan/dispatch_strategy.c. The runtime PR will
 *  replace this with a feature-name → kernel routing table.
 */

#include "dispatch_strategy.h"

int vmaf_hip_dispatch_supports(const VmafHipContext *ctx, const char *feature)
{
    (void)ctx;
    (void)feature;
    /* Intentionally unconditional zero — HIP dispatch is routed through
     * VMAF_FEATURE_EXTRACTOR_HIP in compute_fex_flags() (libvmaf.c), not
     * through this predicate. Kept as a callable symbol so the runtime
     * shape mirrors the CUDA/SYCL/Vulkan/Metal dispatch_supports siblings
     * and the test harness can grow a HIP smoke twin without a new symbol.
     * See ADR-0533 (HIP extractor registration sweep). */
    return 0;
}
