/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA host glue for the psnr_hvs feature extractor
 *  (T7-23 / batch 2 part 3b). See ADR-0188 / ADR-0191 for the
 *  scope + design. Mirror of psnr_hvs_vulkan.
 */
#ifndef FEATURE_PSNR_HVS_CUDA_H_
#define FEATURE_PSNR_HVS_CUDA_H_

#include <stdint.h>
#include "common.h"

extern const unsigned char psnr_hvs_score_ptx[];

#endif /* FEATURE_PSNR_HVS_CUDA_H_ */
