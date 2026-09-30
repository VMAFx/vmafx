/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  CUDA host glue and kernel argument contracts for the psnr_hvs
 *  feature extractor.
 */
#ifndef FEATURE_PSNR_HVS_CUDA_H_
#define FEATURE_PSNR_HVS_CUDA_H_

#include <stddef.h>
#include <stdint.h>
#include "common.h"

#define PSNR_HVS_BLOCK 8
#define PSNR_HVS_STEP 7
#define PSNR_HVS_NUM_PLANES 3
#define PSNR_HVS_WG 64
#define PSNR_HVS_LANE_STRIDE (64 + 1)

typedef struct PsnrHvsPlaneArgs {
    const void *ref;
    const void *dist;
    size_t ref_stride;  /* in bytes */
    size_t dist_stride; /* in bytes */
    unsigned width;
    unsigned blocks_x;
    unsigned first_block;
    unsigned _pad;
} PsnrHvsPlaneArgs;

typedef struct PsnrHvsKernelArgs {
    PsnrHvsPlaneArgs plane[PSNR_HVS_NUM_PLANES];
    float *partials;
    unsigned n_planes;
    unsigned total_blocks;
    int wide;
    int _pad;
} PsnrHvsKernelArgs;

extern const unsigned char psnr_hvs_score_ptx[];

#endif /* FEATURE_PSNR_HVS_CUDA_H_ */
