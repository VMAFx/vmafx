/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA host glue for the real integer_ssim feature extractor
 *  (ADR-0553). The `integer_ssim_score_ptx` symbol is the PTX
 *  byte array generated from `cuda/integer_ssim/integer_ssim_score.cu`.
 */
#ifndef FEATURE_SSIM_INTEGER_CUDA_H_
#define FEATURE_SSIM_INTEGER_CUDA_H_

#include <stdint.h>
#include "common.h"

extern const unsigned char integer_ssim_score_ptx[];

#endif /* FEATURE_SSIM_INTEGER_CUDA_H_ */
