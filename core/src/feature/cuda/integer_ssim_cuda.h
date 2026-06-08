/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA host glue for the float_ssim feature extractor
 *  (T7-23 / batch 2 part 1b). See ADR-0188 / ADR-0189 for the
 *  scope + design.
 */
#ifndef FEATURE_SSIM_CUDA_H_
#define FEATURE_SSIM_CUDA_H_

#include <stdint.h>
#include "common.h"

extern const unsigned char ssim_score_ptx[];

#endif /* FEATURE_SSIM_CUDA_H_ */
