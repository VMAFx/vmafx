/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Dispatch support table for the HIP backend (ADR-0212 / T7-10).
 *  Mirrors CUDA / Metal dispatch predicates: callers query whether a
 *  feature can route to HIP before scheduling GPU execution.
 */

#include "dispatch_strategy.h"
#include "../gpu_dispatch_env.h"
#include "../gpu_dispatch_parse.h"

#include <string.h>

#ifdef HAVE_HIPCC
static const char *const g_hip_features[] = {
    /* integer_motion_v2_hip */
    "motion_v2_hip",
    "VMAF_integer_feature_motion_v2_sad_score",
    "VMAF_integer_feature_motion2_v2_score",
    "VMAF_integer_feature_motion3_v2_score",
    /* float_psnr_hip */
    "float_psnr_hip",
    "float_psnr",
    /* float_moment_hip */
    "float_moment_hip",
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
    /* integer_psnr_hip */
    "integer_psnr_hip",
    "psnr_y",
    "psnr_cb",
    "psnr_cr",
    /* float_motion_hip */
    "float_motion_hip",
    "VMAF_feature_motion_score",
    "VMAF_feature_motion2_score",
    /* integer_motion_hip */
    "integer_motion_hip",
    "VMAF_integer_feature_motion_y_score",
    "VMAF_integer_feature_motion2_score",
    /* float_ssim_hip */
    "float_ssim_hip",
    "float_ssim",
    /* integer_ms_ssim_hip */
    "integer_ms_ssim_hip",
    "float_ms_ssim",
    /* float_vif_hip */
    "float_vif_hip",
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
    /* integer_vif_hip */
    "integer_vif_hip",
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
    "integer_vif",
    "integer_vif_scale0",
    "integer_vif_scale1",
    "integer_vif_scale2",
    "integer_vif_scale3",
    /* float_adm_hip */
    "float_adm_hip",
    "adm",
    "adm_num",
    "adm_den",
    "adm_num_scale0",
    "adm_den_scale0",
    "adm_num_scale1",
    "adm_den_scale1",
    "adm_num_scale2",
    "adm_den_scale2",
    "adm_num_scale3",
    "adm_den_scale3",
    /* ciede_hip */
    "ciede_hip",
    "ciede2000",
    /* integer_psnr_hvs_hip */
    "psnr_hvs_hip",
    "psnr_hvs_y",
    "psnr_hvs_cb",
    "psnr_hvs_cr",
    "psnr_hvs",
    /* integer_cambi_hip */
    "cambi_hip",
    "Cambi_feature_cambi_score",
    /* ssimulacra2_hip */
    "ssimulacra2_hip",
    "ssimulacra2",
    /* speed_chroma_hip & speed_temporal_hip */
    "speed_chroma_hip",
    "speed_chroma_u",
    "speed_chroma_v",
    "speed_chroma_uv",
    "speed_temporal_hip",
    "speed_temporal",
    NULL,
};
#endif /* HAVE_HIPCC */

int vmaf_hip_dispatch_supports(const VmafHipContext *ctx, const char *feature)
{
#ifndef HAVE_HIPCC
    (void)ctx;
    (void)feature;
    return 0;
#else
    if (!ctx || !feature)
        return 0;

    const char *env = vmaf_gpu_dispatch_env_get("VMAF_HIP_DISPATCH");
    if (env) {
        static const char *const k_hip_strategy_names[] = {
            "direct",
            "none",
            "disable",
            NULL,
        };
        int strat_idx = 0;
        if (vmaf_gpu_dispatch_parse_env(env, feature, k_hip_strategy_names, &strat_idx)) {
            if (strat_idx > 0)
                return 0;
        }
    }

    for (size_t i = 0; g_hip_features[i]; ++i) {
        if (strcmp(feature, g_hip_features[i]) == 0)
            return 1;
    }
    return 0;
#endif
}
