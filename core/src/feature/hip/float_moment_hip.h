/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP host glue for the float_moment feature extractor — fourth
 *  kernel-template consumer (T7-10b batch-3 / ADR-0374).
 *
 *  Mirrors libvmaf/src/feature/cuda/integer_moment_cuda.h. The HIP
 *  kernel artefact (`moment_score.hip`) is compiled by hipcc to a HSACO
 *  fat binary and embedded as a C byte array when `enable_hipcc=true`.
 *  The host code loads it via `hipModuleLoadData` +
 *  `hipModuleLaunchKernel`.
 *
 *  When `enable_hipcc=false` (default, e.g. on a non-ROCm CI agent),
 *  the symbol is not available and the feature init() returns -ENOSYS,
 *  identical to the pre-runtime scaffold posture.
 */
#ifndef FEATURE_FLOAT_MOMENT_HIP_H_
#define FEATURE_FLOAT_MOMENT_HIP_H_

#include <stdint.h>

#ifdef HAVE_HIPCC
/* HSACO fat binary embedded by xxd -i (analogous to `moment_score_ptx`
 * in the CUDA twin). The array is defined in the generated
 * `moment_score_hsaco.c` custom_target output. */
extern const unsigned char moment_score_hsaco[];
extern const unsigned int moment_score_hsaco_len;
#endif /* HAVE_HIPCC */

#endif /* FEATURE_FLOAT_MOMENT_HIP_H_ */
