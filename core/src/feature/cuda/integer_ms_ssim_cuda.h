/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  CUDA host glue for the float_ms_ssim feature extractor
 *  (T7-23 / batch 2 part 2b). See ADR-0188 / ADR-0190 for the
 *  scope + design.
 */
#ifndef FEATURE_MS_SSIM_CUDA_H_
#define FEATURE_MS_SSIM_CUDA_H_

#include <stdint.h>
#include "common.h"

extern const unsigned char ms_ssim_score_ptx[];

#endif /* FEATURE_MS_SSIM_CUDA_H_ */
