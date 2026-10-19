/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Device pointer from the 64-bit address a kernel argument struct carries.
 *
 *  The host fills the argument structs of the CUDA feature kernels with
 *  `CUdeviceptr` values (unsigned 64-bit integers), because the structs are
 *  plain C. A kernel turns each address back into a typed pointer once, at
 *  its top. clang-tidy's performance-no-int-to-ptr rejects that conversion
 *  (an integer-to-pointer cast hides the pointer's provenance from the
 *  optimiser); there is no integer-free way to spell it, so the conversion
 *  lives in this one macro, with its one lint suppression.
 *
 *  It is a macro and not a function on purpose. Measured on sm_89 with nvcc
 *  13.4 on core/src/feature/cuda/speed/speed_score.cu: the same conversion
 *  inside a `__forceinline__` function template (reinterpret_cast or
 *  __builtin_bit_cast alike) turns 134 of the file's 378 LDG.E.CONSTANT
 *  (ld.global.nc) loads into plain loads, which gives up the compiler's
 *  freedom to treat those loads as invariant. The macro keeps the SASS of the
 *  direct cast.
 */

#ifndef VMAF_SRC_CUDA_CUDA_DEVICE_PTR_CUH_
#define VMAF_SRC_CUDA_CUDA_DEVICE_PTR_CUH_

#include <stdint.h>

/* `T` may be const-qualified; `address` is an unsigned 64-bit device address. */
/* NOLINTBEGIN(performance-no-int-to-ptr): a CUdeviceptr is an integer by the C ABI of the kernel argument structs and a function wrapper costs ld.global.nc loads (measured above); ADR-1142 allows a cited NOLINT for a load-bearing invariant. */
#define VMAF_CUDA_DPTR(T, address) (reinterpret_cast<T *>(address))
/* NOLINTEND(performance-no-int-to-ptr) */

#endif /* VMAF_SRC_CUDA_CUDA_DEVICE_PTR_CUH_ */
