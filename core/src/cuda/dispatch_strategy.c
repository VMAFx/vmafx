/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA dispatch_strategy stub. Today every extractor returns
 *  DIRECT; this TU exists to expose the registry-aware decision
 *  surface so future graph-capture work for ADM (16 dispatches/
 *  frame) can land without touching the registration sites.
 *  See ADR-0181.
 */
#include "dispatch_strategy.h"
#include "../gpu_dispatch_env.h"
#include "../gpu_dispatch_parse.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit where NULL is the canonical null pointer constant. ADR-1138. */

/* Strategy name table — index matches VmafCudaDispatchStrategy enum values:
 *   0 → VMAF_CUDA_DISPATCH_DIRECT
 *   1 → VMAF_CUDA_DISPATCH_GRAPH_CAPTURE
 * See ADR-0483. */
static const char *const k_cuda_strategy_names[] = {
    "direct", /* VMAF_CUDA_DISPATCH_DIRECT        = 0 */
    "graph",  /* VMAF_CUDA_DISPATCH_GRAPH_CAPTURE = 1 */
    NULL,
};

VmafCudaDispatchStrategy vmaf_cuda_select_strategy(const char *feature_name,
                                                   const VmafFeatureCharacteristics *chars,
                                                   unsigned frame_w, unsigned frame_h)
{
    (void)chars;
    (void)frame_w;
    (void)frame_h;

    const char *env_disp = vmaf_gpu_dispatch_env_get("VMAF_CUDA_DISPATCH");

    int idx = (int)VMAF_CUDA_DISPATCH_DIRECT;
    if (vmaf_gpu_dispatch_parse_env(env_disp, feature_name, k_cuda_strategy_names, &idx)) {
        if (idx == (int)VMAF_CUDA_DISPATCH_GRAPH_CAPTURE) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "libvmaf: CUDA graph dispatch requested for '%s' but graph capture "
                     "is not implemented; falling back to direct\n",
                     feature_name ? feature_name : "<unknown>");
            return VMAF_CUDA_DISPATCH_DIRECT;
        }
        return (VmafCudaDispatchStrategy)idx;
    }

    /* Stub default — DIRECT for every feature. CUDA graph capture
     * is a follow-up PR (see ADR-0181 § Consequences / follow-ups). */
    return VMAF_CUDA_DISPATCH_DIRECT;
}

/* NOLINTEND(modernize-use-nullptr) */
