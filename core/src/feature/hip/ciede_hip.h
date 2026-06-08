/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP host glue for the ciede2000 feature extractor — third
 *  kernel-template consumer (T7-10b follow-up / ADR-0259).
 *  Real kernel promotion: T7-10b batch-4 / ADR-0377.
 *
 *  Mirrors libvmaf/src/feature/cuda/integer_ciede_cuda.h. The HIP
 *  kernel artefact (`ciede_score.hip`) is compiled by hipcc to a HSACO
 *  fat binary and embedded as a C byte array when `enable_hipcc=true`.
 *  The host code loads it via `hipModuleLoadData` + `hipModuleLaunchKernel`
 *  -- the direct HIP module-API analog of the CUDA path's
 *  `cuModuleLoadData` + `cuLaunchKernel`.
 *
 *  When `enable_hipcc=false` (default on non-ROCm CI agents),
 *  the symbol is not available and the feature init() returns -ENOSYS,
 *  identical to the pre-runtime scaffold posture.
 */
#ifndef FEATURE_CIEDE_HIP_H_
#define FEATURE_CIEDE_HIP_H_

#include <stdint.h>

#ifdef HAVE_HIPCC
/* HSACO fat binary embedded by xxd -i (analogous to `ciede_score_ptx`
 * in the CUDA twin). The array is defined in the generated
 * `ciede_score_hsaco.c` custom_target output. */
extern const unsigned char ciede_score_hsaco[];
extern const unsigned int ciede_score_hsaco_len;
#endif /* HAVE_HIPCC */

#endif /* FEATURE_CIEDE_HIP_H_ */
