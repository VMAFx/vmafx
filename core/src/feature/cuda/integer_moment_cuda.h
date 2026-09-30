/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA host glue for the float_moment feature extractor
 *  (T7-23 / batch 1d part 2). See ADR-0182 for the bundle scope.
 */
#ifndef FEATURE_MOMENT_CUDA_H_
#define FEATURE_MOMENT_CUDA_H_

#include <stdint.h>
#include "common.h"

extern const unsigned char moment_score_ptx[];

/* Launch geometry shared by moment_score.cu and its host dispatch
 * (ADR-1392, as integer_psnr_cuda.h). A block is MOMENT_BLOCK_X x
 * MOMENT_BLOCK_Y threads; each thread sums MOMENT_COLS_PER_THREAD luma
 * pixels of one row, MOMENT_BLOCK_X apart so every load of a warp is one
 * contiguous run, and the block adds each of its MOMENT_SUMS sums to its
 * accumulator with one atomic. A block therefore covers MOMENT_BLOCK_COLS
 * columns and MOMENT_BLOCK_Y rows. */
#define MOMENT_BLOCK_X 32u
#define MOMENT_BLOCK_Y 8u
#define MOMENT_COLS_PER_THREAD 8u
#define MOMENT_BLOCK_COLS (MOMENT_BLOCK_X * MOMENT_COLS_PER_THREAD)
/* Accumulators, in the order the host reads them: ref1, dis1, ref2, dis2. */
#define MOMENT_SUMS 4u

#endif /* FEATURE_MOMENT_CUDA_H_ */
