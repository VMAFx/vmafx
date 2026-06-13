/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#ifndef __CUDA_HELPER_H__
#define __CUDA_HELPER_H__

#ifdef DEVICE_CODE
#include <cstdint>
#ifndef __clang__
#include <cuda_runtime.h>
#endif
#endif

#include "assert.h"
#include "stdio.h"
#include <errno.h>
#ifdef DEVICE_CODE
#include <cuda.h>
#else
#include <ffnvcodec/dynlink_loader.h>
#endif

#define DIV_ROUND_UP(x, y) (((x) + (y) - 1) / (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/*
 * Netflix#1420 — CUDA error handling must return a clean errno rather
 * than abort()-ing the process. The legacy CHECK_CUDA macro that fired
 * assert(0) on any CUresult != CUDA_SUCCESS turned a recoverable
 * cuMemAlloc OOM (e.g. two concurrent VMAF-CUDA processes competing for
 * GPU memory) into a hard SIGABRT on the second caller. The two macros
 * below replace it across the fork's CUDA sources:
 *
 *   CHECK_CUDA_GOTO(funcs, CALL, label) — use when the enclosing
 *     function has pushed a CUDA context, allocated a CUDA buffer, or
 *     otherwise established cleanup state that must unwind before the
 *     function returns. Callers declare `int _cuda_err = 0;` once per
 *     function and put the unwind sequence under `label:`, then return
 *     `_cuda_err`.
 *
 *   CHECK_CUDA_RETURN(funcs, CALL) — use when no cleanup is pending
 *     (no pushed context, no allocations). Returns the mapped errno
 *     directly from the enclosing function.
 *
 * Both macros log the failure to stderr with file/line/CUDA error
 * string before returning. See ADR addressing Netflix#1420 for the
 * design rationale.
 */

/* Map CUresult → negative errno used across libvmaf. Takes int (not
 * CUresult) so host .c files that don't transitively include cuda.h can
 * still consume the mapping via a thin wrapper if ever needed.
 */
static inline int vmaf_cuda_result_to_errno(int cu_err_code)
{
    switch (cu_err_code) {
    case 0: /* CUDA_SUCCESS */
        return 0;
    case 2: /* CUDA_ERROR_OUT_OF_MEMORY */
        return -ENOMEM;
    case 3: /* CUDA_ERROR_NOT_INITIALIZED */
    case 4: /* CUDA_ERROR_DEINITIALIZED */
        return -ENODEV;
    case 1:   /* CUDA_ERROR_INVALID_VALUE */
    case 101: /* CUDA_ERROR_INVALID_DEVICE */
    case 201: /* CUDA_ERROR_INVALID_CONTEXT */
    case 400: /* CUDA_ERROR_INVALID_HANDLE */
        return -EINVAL;
    default:
        return -EIO;
    }
}

#define CHECK_CUDA_GOTO(funcs, CALL, label)                                                        \
    do {                                                                                           \
        const CUresult _cu_res = (funcs)->CALL;                                                    \
        if (CUDA_SUCCESS != _cu_res) {                                                             \
            const char *_err_txt = "?";                                                            \
            (funcs)->cuGetErrorName(_cu_res, &_err_txt);                                           \
            fprintf(stderr, "CUDA error at %s:%d: %s (%d) in %s\n", __FILE__, __LINE__, _err_txt,  \
                    (int)_cu_res, #CALL);                                                          \
            _cuda_err = vmaf_cuda_result_to_errno((int)_cu_res);                                   \
            goto label;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_CUDA_RETURN(funcs, CALL)                                                             \
    do {                                                                                           \
        const CUresult _cu_res = (funcs)->CALL;                                                    \
        if (CUDA_SUCCESS != _cu_res) {                                                             \
            const char *_err_txt = "?";                                                            \
            (funcs)->cuGetErrorName(_cu_res, &_err_txt);                                           \
            fprintf(stderr, "CUDA error at %s:%d: %s (%d) in %s\n", __FILE__, __LINE__, _err_txt,  \
                    (int)_cu_res, #CALL);                                                          \
            return vmaf_cuda_result_to_errno((int)_cu_res);                                        \
        }                                                                                          \
    } while (0)

#ifdef DEVICE_CODE
namespace
{
__forceinline__ __device__ int64_t warp_reduce(int64_t x)
{
#pragma unroll
    for (int i = 16; i > 0; i >>= 1) {
        x += int64_t(__shfl_down_sync(0xffffffff, x & 0xffffffff, i)) |
             int64_t(__shfl_down_sync(0xffffffff, x >> 32, i) << 32);
    }
    return x;
}

typedef unsigned long long int uint64_cu;
__forceinline__ __device__ int64_t atomicAdd_int64(int64_t *address, int64_t val)
{
    return atomicAdd(reinterpret_cast<uint64_cu *>(address), static_cast<uint64_cu>(val));
}
} // namespace
#endif

#endif
