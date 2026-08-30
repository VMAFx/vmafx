/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP host glue for the PSNR feature extractor (T7-10 first
 *  consumer / ADR-0241; real kernel: T7-10b batch-1 / ADR-0372).
 *
 *  Mirrors libvmaf/src/feature/cuda/integer_psnr_cuda.h. The HSACO
 *  fat binary (`psnr_score.hip` compiled via `hipcc --genco`, embedded
 *  by `xxd -i`) is declared here when `HAVE_HIPCC` is defined.
 */
#ifndef FEATURE_PSNR_HIP_H_
#define FEATURE_PSNR_HIP_H_

#ifdef HAVE_HIPCC
/*
 * Symbol produced by `xxd -i psnr_score.hsaco > psnr_score_hsaco.c`
 * in the meson `hip_hsaco_sources` custom_target pipeline (ADR-0372).
 * Mirrors the way `psnr_score_ptx` is declared in
 * `libvmaf/src/feature/cuda/integer_psnr_cuda.h`.
 */
extern const unsigned char psnr_score_hsaco[];
extern const unsigned int psnr_score_hsaco_len;
#endif /* HAVE_HIPCC */

#endif /* FEATURE_PSNR_HIP_H_ */
