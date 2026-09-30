/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  HIP host glue for the psnr_hvs feature extractor, shared with its device
 *  module (integer_psnr_hvs/psnr_hvs_score.hip): the kernel-argument structs,
 *  passed by value through hipModuleLaunchKernel(). Their layout must stay
 *  plain C (pointers and unsigned scalars only); they are struct tags, not
 *  typedefs, so the one declaration is idiomatic in C and C++.
 *
 *  The HSACO fat binary (`psnr_hvs_score.hip` compiled via
 *  `hipcc --genco`, embedded by `xxd -i`) is declared here when
 *  `HAVE_HIPCC` is defined.
 */
#ifndef FEATURE_PSNR_HVS_HIP_H_
#define FEATURE_PSNR_HVS_HIP_H_

#define PSNR_HVS_HIP_NUM_PLANES 3
/* Work-items per work-group: 32 blocks, a reference and a distorted
 * work-item each (ADR-1369 shape). */
#define PSNR_HVS_HIP_WG 64

/* One plane as the kernel sees it: packed raw samples of both images. */
struct PsnrHvsHipPlaneArgs {
    const void *ref;
    const void *dist;
    unsigned width;
    unsigned blocks_x;
    unsigned first_block; /* offset of this plane's blocks in `partials` */
};

struct PsnrHvsHipKernelArgs {
    struct PsnrHvsHipPlaneArgs plane[PSNR_HVS_HIP_NUM_PLANES];
    float *partials; /* one masked-error sum per block, planes back to back */
    unsigned n_planes;
    unsigned total_blocks;
    unsigned wide; /* 16-bit samples (bpc > 8) */
};

#ifdef HAVE_HIPCC
/*
 * Symbol produced by `xxd -i psnr_hvs_score.hsaco > psnr_hvs_score_hsaco.c`
 * in the meson `hip_hsaco_sources` custom_target pipeline.
 * Mirrors the way `psnr_hvs_score_ptx` is declared in
 * `core/src/feature/cuda/integer_psnr_hvs_cuda.h`.
 */
extern const unsigned char psnr_hvs_score_hsaco[];
extern const unsigned int psnr_hvs_score_hsaco_len;
#endif /* HAVE_HIPCC */

#endif /* FEATURE_PSNR_HVS_HIP_H_ */
