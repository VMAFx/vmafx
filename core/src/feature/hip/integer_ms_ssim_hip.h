/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP host glue for the integer_ms_ssim feature extractor — ninth
 *  kernel-template consumer (ADR-0285).
 *
 *  Mirrors libvmaf/src/feature/cuda/integer_ms_ssim_cuda.h. The HIP
 *  kernel artefact (`ms_ssim_score.hip`) is compiled by hipcc to a
 *  HSACO fat binary and embedded as a C byte array when
 *  `enable_hipcc=true`. The host code loads it via `hipModuleLoadData`
 *  + `hipModuleLaunchKernel`.
 *
 *  When `enable_hipcc=false` (default, e.g. on a non-ROCm CI agent),
 *  the symbol is not available and the feature init() returns -ENOSYS,
 *  identical to the pre-runtime scaffold posture.
 */
#ifndef FEATURE_INTEGER_MS_SSIM_HIP_H_
#define FEATURE_INTEGER_MS_SSIM_HIP_H_

#include <stdint.h>

#ifdef HAVE_HIPCC
/* HSACO fat binary embedded by xxd -i (analogous to `ms_ssim_score_ptx`
 * in the CUDA twin). The array is defined in the generated
 * `ms_ssim_score_hsaco.c` custom_target output. */
extern const unsigned char ms_ssim_score_hsaco[];
extern const unsigned int ms_ssim_score_hsaco_len;
#endif /* HAVE_HIPCC */

#endif /* FEATURE_INTEGER_MS_SSIM_HIP_H_ */
