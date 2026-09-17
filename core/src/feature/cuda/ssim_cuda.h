/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  CUDA host glue for the real integer_ssim feature extractor
 *  (ADR-0564). The `integer_ssim_score_ptx` symbol is the PTX
 *  byte array generated from `cuda/integer_ssim/integer_ssim_score.cu`.
 */
#ifndef FEATURE_SSIM_INTEGER_CUDA_H_
#define FEATURE_SSIM_INTEGER_CUDA_H_

#include <stdint.h>
#include "common.h"

extern const unsigned char integer_ssim_score_ptx[];

#endif /* FEATURE_SSIM_INTEGER_CUDA_H_ */
