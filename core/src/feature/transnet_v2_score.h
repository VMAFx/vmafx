/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAF_FEATURE_TRANSNET_V2_SCORE_H
#define VMAF_FEATURE_TRANSNET_V2_SCORE_H

#include <errno.h>
#include <math.h>

static inline int vmaf_transnet_v2_scores(double logit, double threshold, double *probability,
                                          double *boundary)
{
    if (!probability || !boundary)
        return -EINVAL;
    if (!isfinite(logit) || !isfinite(threshold) || threshold < 0.0 || threshold > 1.0)
        return -EINVAL;

    double prob;
    if (logit >= 30.0) {
        prob = 1.0;
    } else if (logit <= -30.0) {
        prob = 0.0;
    } else {
        prob = 1.0 / (1.0 + exp(-logit));
    }

    if (!isfinite(prob))
        return -EINVAL;

    const double boundary_value = prob >= threshold ? 1.0 : 0.0;
    *probability = prob;
    *boundary = boundary_value;
    return 0;
}

#endif /* VMAF_FEATURE_TRANSNET_V2_SCORE_H */
