/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  SpEED scoring constants, evaluated by speed_internal.c exactly as speed.c
 *  evaluates them (ADR-1358). A device port that receives them as parameters
 *  reproduces the host values bit for bit, because they come from the same
 *  compiler flags and the same libm as the CPU extractor.
 */

#ifndef VMAF_SRC_FEATURE_SPEED_CONSTANTS_H_
#define VMAF_SRC_FEATURE_SPEED_CONSTANTS_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * `log2f(2 * pi * e)`, the per-eigenvalue constant of speed.c's
 * update_entropy().
 */
float speed_internal_entropy_constant(void);

/**
 * get_speed_score()'s entropy floor:
 * `elements * (log2f((1 + nn_floor) * sigma_nn) + log2f(2 * pi * e))`.
 *
 * @param elements_in_block  25 for SpEED.
 * @param sigma_nn           speed_sigma_nn, narrowed to float as speed.c does.
 * @param nn_floor           speed_nn_floor, narrowed to float.
 */
float speed_internal_base_entropy(size_t elements_in_block, float sigma_nn, float nn_floor);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_FEATURE_SPEED_CONSTANTS_H_ */
