/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP host glue for the psnr_hvs feature extractor.
 *  Mirrors `libvmaf/src/feature/cuda/integer_psnr_hvs_cuda.h`.
 *  The HSACO fat binary (`psnr_hvs_score.hip` compiled via
 *  `hipcc --genco`, embedded by `xxd -i`) is declared here when
 *  `HAVE_HIPCC` is defined.
 */
#ifndef FEATURE_PSNR_HVS_HIP_H_
#define FEATURE_PSNR_HVS_HIP_H_

#ifdef HAVE_HIPCC
/*
 * Symbol produced by `xxd -i psnr_hvs_score.hsaco > psnr_hvs_score_hsaco.c`
 * in the meson `hip_hsaco_sources` custom_target pipeline.
 * Mirrors the way `psnr_hvs_score_ptx` is declared in
 * `libvmaf/src/feature/cuda/integer_psnr_hvs_cuda.h`.
 */
extern const unsigned char psnr_hvs_score_hsaco[];
extern const unsigned int psnr_hvs_score_hsaco_len;
#endif /* HAVE_HIPCC */

#endif /* FEATURE_PSNR_HVS_HIP_H_ */
