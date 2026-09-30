/**
 *  Copyright 2016-2026 Netflix, Inc.
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

/* Launch geometry shared by psnr_score.cu and its host dispatch. A block is
 * PSNR_BLOCK_X x PSNR_BLOCK_Y threads; each thread sums PSNR_COLS_PER_THREAD
 * pixels of one row, PSNR_BLOCK_X apart so every load of a warp is one
 * contiguous run, and the block adds its sum to the plane's accumulator
 * with one atomic. A block therefore covers PSNR_BLOCK_COLS columns and
 * PSNR_BLOCK_Y rows. */
#define PSNR_BLOCK_X 32u
#define PSNR_BLOCK_Y 8u
#define PSNR_COLS_PER_THREAD 8u
#define PSNR_BLOCK_COLS (PSNR_BLOCK_X * PSNR_COLS_PER_THREAD)

#endif /* FEATURE_PSNR_CUDA_H_ */
