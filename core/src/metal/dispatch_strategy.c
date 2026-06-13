/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Dispatch support table for the Metal backend (ADR-0421 / T8-1c-k).
 *  Mirrors the CUDA / Vulkan dispatch predicates: callers ask whether
 *  a feature can route to this backend before they bind GPU pictures.
 */

#include "dispatch_strategy.h"

#include <string.h>

/* Each extractor's provided_features[] array is the source-of-truth for
 * the canonical score-level names the dispatcher's `feature` argument
 * carries. Aliases stored here that do not match a provided_features
 * entry exactly cause vmaf_metal_dispatch_supports() to return 0 for the
 * canonical name and silently fall back to CPU — see ADR-0421 §Routing
 * and the metal/AGENTS.md "Dispatch table contract" section.
 *
 * Verified against (all on master tip 2026-05-30):
 *   - integer_motion_v2_metal.mm  → "VMAF_integer_feature_motion_v2_sad_score",
 *                                   "VMAF_integer_feature_motion2_v2_score"
 *   - integer_motion_metal.mm     → "VMAF_integer_feature_motion_y_score",
 *                                   "VMAF_integer_feature_motion2_score"
 *   - float_motion_metal.mm       → "VMAF_feature_motion_score",
 *                                   "VMAF_feature_motion2_score"
 *
 * The previous short forms ("motion2_v2_score", "motion2_score",
 * "motion3_score") never matched any canonical name. "motion3" is also
 * not implemented on Metal (per integer_motion_metal.mm:15), so the
 * entry is removed entirely.
 */
static const char *const g_metal_features[] = {
    "motion_v2_metal",
    "VMAF_integer_feature_motion_v2_sad_score",
    "VMAF_integer_feature_motion2_v2_score",
    "float_psnr_metal",
    "float_psnr",
    "float_moment_metal",
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
    "integer_psnr_metal",
    "psnr_y",
    "psnr_cb",
    "psnr_cr",
    "float_motion_metal",
    "float_motion",
    "VMAF_feature_motion_score",
    "VMAF_feature_motion2_score",
    "integer_motion_metal",
    "VMAF_integer_feature_motion_y_score",
    "VMAF_integer_feature_motion2_score",
    "float_ssim_metal",
    "float_ssim",
    "float_ms_ssim",
    NULL,
};

int vmaf_metal_dispatch_supports(const VmafMetalContext *ctx, const char *feature)
{
    if (!ctx || !feature)
        return 0;

    for (size_t i = 0; g_metal_features[i]; ++i) {
        if (strcmp(feature, g_metal_features[i]) == 0)
            return 1;
    }
    return 0;
}
