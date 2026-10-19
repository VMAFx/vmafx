/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_adm on CUDA: the spelling CUDA device code needs for the arithmetic
 *  and the argument blocks of feature/float_adm_gpu_common.h, which the host
 *  (float_adm_cuda.c), the kernels (float_adm_score.cu) and the device-free
 *  host test (core/test/test_float_adm_device_math.c) compile through this
 *  header (ADR-1420). The arithmetic moved to the backend-neutral header when
 *  float_adm_hip started to run it as well (ADR-1458); nothing in it changed.
 *
 *  Numerical contract: float_adm_cuda returns the CPU extractor's values bit
 *  for bit. On the device every rounding is an explicit round-to-nearest
 *  intrinsic, which neither nvcc nor clang's CUDA driver contracts; the
 *  division is `__fdiv_rn()`, the IEEE fp32 quotient whatever the compiler's
 *  division flags (ADR-1442). The fatbin is built without contraction as well
 *  (ADR-1403). A host TU that includes this header gets the common header's
 *  plain operators and must be built with contraction off.
 */

#ifndef VMAF_SRC_FEATURE_CUDA_FLOAT_ADM_FLOAT_ADM_DEVICE_H_
#define VMAF_SRC_FEATURE_CUDA_FLOAT_ADM_FLOAT_ADM_DEVICE_H_

#if defined(DEVICE_CODE)
#define FADM_HD __device__ __forceinline__
#define FADM_FMUL(a, b) __fmul_rn((a), (b))
#define FADM_FADD(a, b) __fadd_rn((a), (b))
#define FADM_FSUB(a, b) __fsub_rn((a), (b))
#define FADM_FDIV(a, b) __fdiv_rn((a), (b))
#define FADM_DMUL(a, b) __dmul_rn((a), (b))
#define FADM_DADD(a, b) __dadd_rn((a), (b))
#define FADM_BITS(x) __float_as_uint(x)
#define FADM_FROM_BITS(u) __uint_as_float(u)
#define FADM_DEVICE_ONLY
#endif

#include "feature/float_adm_gpu_common.h"

/* The argument blocks under the names the CUDA sources use. */
/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no `using`. ADR-1138. */
typedef FloatAdmGpuBands FloatAdmCudaBands;
typedef FloatAdmGpuDecoupleArgs FloatAdmCudaDecoupleArgs;
typedef FloatAdmGpuTermArgs FloatAdmCudaTermArgs;
typedef FloatAdmGpuRowArgs FloatAdmCudaRowArgs;
/* NOLINTEND(modernize-use-using) */

#endif /* VMAF_SRC_FEATURE_CUDA_FLOAT_ADM_FLOAT_ADM_DEVICE_H_ */
