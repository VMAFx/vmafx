/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA host glue for the ciede2000 feature extractor
 *  (T7-23 / batch 1c part 2). See ADR-0182 for the bundle scope
 *  and ADR-0187 for the float-precision contract.
 */
#ifndef FEATURE_CIEDE_CUDA_H_
#define FEATURE_CIEDE_CUDA_H_

#include <stdint.h>
#include "common.h"

extern const unsigned char ciede_score_ptx[];

#endif /* FEATURE_CIEDE_CUDA_H_ */
