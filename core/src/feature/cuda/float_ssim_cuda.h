/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  CUDA host glue for the float_ssim feature extractor.
 *  See integer_ssim_cuda.h for the legacy name; this file is
 *  the correctly-named successor used by float_ssim_cuda.c.
 */
#ifndef FEATURE_FLOAT_SSIM_CUDA_H_
#define FEATURE_FLOAT_SSIM_CUDA_H_

#include <stdint.h>
#include "common.h"

extern const unsigned char ssim_score_ptx[];

#endif /* FEATURE_FLOAT_SSIM_CUDA_H_ */
