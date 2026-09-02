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
 * carries. The table stores, for every registered Metal extractor, BOTH
 * its registry name ("<basename>_metal") AND every key in its
 * provided_features[] array. Aliases stored here that do not match a
 * provided_features entry exactly cause vmaf_metal_dispatch_supports() to
 * return 0 for the canonical name and silently fall back to CPU — see
 * ADR-0421 §Routing and the metal/AGENTS.md "Dispatch table contract"
 * section.
 *
 * All 17 registered Metal extractors are listed (T8 completeness — see
 * core/src/feature/feature_extractor.c's feature_extractor_list[] and
 * core/src/metal/meson.build's metal_objcpp_lib sources). Each block is
 * verified against the corresponding .mm's provided_features[] array
 * (the source-of-truth) on master tip 2026-06-15:
 *
 *   - integer_motion_v2_metal.mm → "motion_v2_metal" +
 *       motion_v2_sad / motion2_v2 / motion3_v2 scores
 *   - integer_motion_metal.mm    → "integer_motion_metal" +
 *       motion_y / motion2 scores
 *   - float_motion_metal.mm      → "float_motion_metal" +
 *       motion / motion2 scores
 *
 * Provided-features keys are copied verbatim from each .mm; do not abbreviate
 * (the previous short forms "motion2_v2_score" / "motion2_score" never matched
 * any canonical name). motion3_v2 IS provided by integer_motion_v2_metal.mm
 * and is therefore listed; standalone "motion3" remains unimplemented on Metal.
 */
static const char *const g_metal_features[] = {
    /* integer_motion_v2_metal.mm */
    "motion_v2_metal",
    "VMAF_integer_feature_motion_v2_sad_score",
    "VMAF_integer_feature_motion2_v2_score",
    "VMAF_integer_feature_motion3_v2_score",
    /* float_psnr_metal.mm */
    "float_psnr_metal",
    "float_psnr",
    /* float_moment_metal.mm */
    "float_moment_metal",
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
    /* integer_psnr_metal.mm */
    "integer_psnr_metal",
    "psnr_y",
    "psnr_cb",
    "psnr_cr",
    /* float_motion_metal.mm */
    "float_motion_metal",
    "VMAF_feature_motion_score",
    "VMAF_feature_motion2_score",
    /* integer_motion_metal.mm */
    "integer_motion_metal",
    "VMAF_integer_feature_motion_y_score",
    "VMAF_integer_feature_motion2_score",
    /* float_ssim_metal.mm */
    "float_ssim_metal",
    "float_ssim",
    "float_ssim_l",
    "float_ssim_c",
    "float_ssim_s",
    /* float_ms_ssim_metal.mm */
    "float_ms_ssim_metal",
    "float_ms_ssim",
    /* integer_ssim_metal.mm */
    "integer_ssim_metal",
    "ssim",
    /* float_vif_metal.mm */
    "float_vif_metal",
    "VMAF_feature_vif_scale0_score",
    "VMAF_feature_vif_scale1_score",
    "VMAF_feature_vif_scale2_score",
    "VMAF_feature_vif_scale3_score",
    "vif",
    "vif_num",
    "vif_den",
    "vif_num_scale0",
    "vif_den_scale0",
    "vif_num_scale1",
    "vif_den_scale1",
    "vif_num_scale2",
    "vif_den_scale2",
    "vif_num_scale3",
    "vif_den_scale3",
    /* integer_vif_metal.mm */
    "integer_vif_metal",
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
    "integer_vif",
    "integer_vif_num",
    "integer_vif_den",
    "integer_vif_num_scale0",
    "integer_vif_den_scale0",
    "integer_vif_num_scale1",
    "integer_vif_den_scale1",
    "integer_vif_num_scale2",
    "integer_vif_den_scale2",
    "integer_vif_num_scale3",
    "integer_vif_den_scale3",
    /* float_adm_metal.mm */
    "float_adm_metal",
    "VMAF_feature_adm2_score",
    "VMAF_feature_aim_score",
    "VMAF_feature_adm3_score",
    "VMAF_feature_adm_scale0_score",
    "VMAF_feature_adm_scale1_score",
    "VMAF_feature_adm_scale2_score",
    "VMAF_feature_adm_scale3_score",
    "adm_num",
    "adm_den",
    "adm_scale0",
    "adm_num_scale0",
    "adm_den_scale0",
    "adm_num_scale1",
    "adm_den_scale1",
    "adm_num_scale2",
    "adm_den_scale2",
    "adm_num_scale3",
    "adm_den_scale3",
    /* integer_adm_metal.mm */
    "integer_adm_metal",
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_aim_score",
    "VMAF_integer_feature_adm3_score",
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
    "integer_adm",
    "integer_adm_num",
    "integer_adm_den",
    "integer_adm_num_scale0",
    "integer_adm_den_scale0",
    "integer_adm_num_scale1",
    "integer_adm_den_scale1",
    "integer_adm_num_scale2",
    "integer_adm_den_scale2",
    "integer_adm_num_scale3",
    "integer_adm_den_scale3",
    /* integer_ciede_metal.mm */
    "integer_ciede_metal",
    "ciede2000",
    /* integer_psnr_hvs_metal.mm */
    "integer_psnr_hvs_metal",
    "psnr_hvs_y",
    "psnr_hvs_cb",
    "psnr_hvs_cr",
    "psnr_hvs",
    /* integer_cambi_metal.mm */
    "integer_cambi_metal",
    "Cambi_feature_cambi_score",
    /* ssimulacra2_metal.mm */
    "ssimulacra2_metal",
    "ssimulacra2",
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
