/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef SSIMULACRA2_SCORE_H_
#define SSIMULACRA2_SCORE_H_

#include <math.h>

static inline int vmaf_ss2_score_is_finite(double score)
{
    return isfinite(score);
}

/* The CPU and GPU host twins all perform these two ordered comparisons.
 * Keeping them here prevents one backend from silently choosing different
 * NaN behaviour while preserving the exact finite arithmetic. */
static inline void vmaf_ss2_split_edge_difference(double difference, double *artifact,
                                                  double *detail)
{
    /* Both ordered comparisons below are false for NaN. Preserve a failed
     * computation as non-finite in both accumulators so the final frame guard
     * can reject it; mapping it to two zeroes erases all evidence of failure. */
    if (!isfinite(difference)) {
        *artifact = difference;
        *detail = difference;
        return;
    }
    *artifact = difference > 0.0 ? difference : 0.0;
    *detail = difference < 0.0 ? -difference : 0.0;
}

/* Final SSIMULACRA 2 polynomial mapping shared by every host twin. */
static inline double vmaf_ss2_finalize_score(double score)
{
    /* NaN and -Inf both fail `score > 0.0` and used to become 100.0, the
     * metric's perfect score. Keep every non-finite input visible so the
     * extractor can fail the frame before publication. */
    if (!isfinite(score))
        return score;
    if (score > 0.0)
        return 100.0 - 10.0 * pow(score, 0.6276336467831387);
    return 100.0;
}

#endif /* SSIMULACRA2_SCORE_H_ */
