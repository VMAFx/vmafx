/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA host glue for the PSNR feature extractor (T7-23 / batch 1b).
 *  See ADR-0182 for the bundle scope.
 */
#ifndef FEATURE_PSNR_CUDA_H_
#define FEATURE_PSNR_CUDA_H_

#include <stdint.h>
#include "common.h"

extern const unsigned char psnr_score_ptx[];

#endif /* FEATURE_PSNR_CUDA_H_ */
