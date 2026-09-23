/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAF_FEATURE_ADM_SCORE_H
#define VMAF_FEATURE_ADM_SCORE_H

#include <errno.h>
#include <math.h>

static inline int vmaf_adm_finalize_scores(double num, double den, double aim_num, double aim_den,
                                           double *score, double *score_aim)
{
    if (!score || !score_aim)
        return -EINVAL;
    if (!isfinite(num) || !isfinite(den) || !isfinite(aim_num) || !isfinite(aim_den))
        return -EINVAL;

    if (den == 0.0) {
        *score = 1.0;
        *score_aim = 1.0;
        return 0;
    }

    if (aim_den == 0.0)
        return -EINVAL;

    const double final_score = num / den;
    const double aim_ratio = aim_num / aim_den;
    if (!isfinite(final_score) || !isfinite(aim_ratio))
        return -EINVAL;

    const double final_aim = aim_ratio < 1.0 ? aim_ratio : 1.0;
    *score = final_score;
    *score_aim = final_aim;
    return 0;
}

static inline int vmaf_adm3_score(double score, double score_aim, int apply_harmonic_mean,
                                  double dlm_weight, double min_value, double *adm3)
{
    if (!adm3)
        return -EINVAL;
    if (!isfinite(score) || !isfinite(score_aim) || !isfinite(dlm_weight) || !isfinite(min_value))
        return -EINVAL;

    double raw_score;
    if (apply_harmonic_mean) {
        const double sum = score + score_aim;
        if (!isfinite(sum))
            return -EINVAL;
        raw_score = sum == 0.0 ? 0.0 : 2.0 * score * score_aim / sum;
    } else {
        raw_score = score * dlm_weight + (1.0 - score_aim) * (1.0 - dlm_weight);
    }
    if (!isfinite(raw_score))
        return -EINVAL;

    *adm3 = raw_score > min_value ? raw_score : min_value;
    return 0;
}

#endif /* VMAF_FEATURE_ADM_SCORE_H */
