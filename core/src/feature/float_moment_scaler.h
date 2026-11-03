/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

#ifndef FEATURE_FLOAT_MOMENT_SCALER_H_
#define FEATURE_FLOAT_MOMENT_SCALER_H_

/* Match picture_copy() at every supported depth (8..16). First-moment sums
 * use units of 1 / scaler; float-square sums use units of 1 / scaler^2.
 * Keep this host-only: the device sum headers use integer arithmetic. */
static inline double vmaf_float_moment_scaler(unsigned bpc)
{
    return bpc > 8u ? (double)(1u << (bpc - 8u)) : 1.0;
}

#endif /* FEATURE_FLOAT_MOMENT_SCALER_H_ */
