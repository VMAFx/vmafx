/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_vif on CUDA: the spelling CUDA device code needs for the arithmetic
 *  and the argument blocks of feature/float_vif_gpu_common.h, which the host
 *  (float_vif_cuda.c), the kernels (float_vif_score.cu) and the device-free
 *  host test (core/test/test_float_vif_device_math.c) compile through this
 *  header (ADR-1412). The arithmetic moved to the backend-neutral header when
 *  float_vif_hip started to run it as well (ADR-1444); nothing in it changed.
 *
 *  Numerical contract: float_vif_cuda returns the CPU extractor's values bit
 *  for bit. On the device every rounding is an explicit round-to-nearest
 *  intrinsic, which neither nvcc nor clang's CUDA driver contracts; the fatbin
 *  is built without contraction as well (ADR-1403). A host TU that includes
 *  this header gets the common header's plain operators and must be built
 *  with contraction off.
 */

#ifndef VMAF_SRC_FEATURE_CUDA_FLOAT_VIF_FLOAT_VIF_DEVICE_H_
#define VMAF_SRC_FEATURE_CUDA_FLOAT_VIF_FLOAT_VIF_DEVICE_H_

#if defined(DEVICE_CODE)
#define FVIF_HD __device__ __forceinline__
#define FVIF_FMUL(a, b) __fmul_rn((a), (b))
#define FVIF_FADD(a, b) __fadd_rn((a), (b))
#define FVIF_FSUB(a, b) __fsub_rn((a), (b))
#define FVIF_FDIV(a, b) __fdiv_rn((a), (b))
#define FVIF_DADD(a, b) __dadd_rn((a), (b))
#define FVIF_DDIV(a, b) __ddiv_rn((a), (b))
#define FVIF_FLOAT_AS_UINT(value) __float_as_uint(value)
#define FVIF_UINT_AS_FLOAT(bits) __uint_as_float(bits)
#define FVIF_DEVICE_ONLY
#endif

#include "feature/float_vif_gpu_common.h"

/* The argument blocks under the names the CUDA sources use. */
/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no using. ADR-1138. */
typedef FloatVifGpuTaps FloatVifCudaTaps;
typedef FloatVifGpuInput FloatVifCudaInput;
typedef FloatVifGpuComputeArgs FloatVifCudaComputeArgs;
typedef FloatVifGpuDecimateArgs FloatVifCudaDecimateArgs;
typedef FloatVifGpuRowArgs FloatVifCudaRowArgs;
/* NOLINTEND(modernize-use-using) */

#endif /* VMAF_SRC_FEATURE_CUDA_FLOAT_VIF_FLOAT_VIF_DEVICE_H_ */
