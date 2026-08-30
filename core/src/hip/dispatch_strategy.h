/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef LIBVMAF_HIP_DISPATCH_STRATEGY_H_
#define LIBVMAF_HIP_DISPATCH_STRATEGY_H_

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns 1 if the HIP backend can dispatch the named feature on the
 * given context, 0 otherwise. Currently returns 0 unconditionally —
 * HIP dispatch is routed through VMAF_FEATURE_EXTRACTOR_HIP in
 * compute_fex_flags() (libvmaf.c), not through this predicate. The
 * symbol is kept for parity with the Vulkan/SYCL/CUDA/Metal
 * dispatch-predicate siblings. See ADR-0533.
 */
int vmaf_hip_dispatch_supports(const VmafHipContext *ctx, const char *feature);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_HIP_DISPATCH_STRATEGY_H_ */
