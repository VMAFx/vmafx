/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
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
 *  HIP port of cuda/integer_adm/adm_csf_den.cu.
 *
 *  Algorithmic correspondence to the CUDA twin (ADR-0539):
 *
 *    1. Same per-pixel arithmetic — sums `|src[j]|**3` (scale 0) or the
 *       cube-after-square-shift accumulator (scales 1-3) over the
 *       `[left, right) x [top, bottom)` ROI of each H/V/D band.
 *
 *    2. The CUDA twin performs a per-warp reduction via `__shfl_down_sync`
 *       (`cuda_helper.cuh::warp_reduce`) before a single per-warp atomicAdd.
 *       That helper requires CUDA-only intrinsics (warpSize=32 baked into
 *       the mask) that don't compile under hipcc, and AMD wavefronts are
 *       64 wide so the shuffle pattern wouldn't be correct anyway.
 *
 *       This port issues one atomicAdd-per-thread on the 64-bit unsigned
 *       accumulator. Bit-exact w.r.t. the CUDA twin — only the reduction
 *       *order* changes, and 64-bit unsigned integer addition is
 *       associative and commutative. Mirrors the pattern adopted in
 *       `integer_vif/vif_statistics.hip` (ADR-0537).
 *
 *    3. Block layout: `cta_size=128, val_per_thread=8` matches the CUDA
 *       grid the host TU uses (`integer_adm_hip.c::run_adm_extract`).
 *       That preserves the launch grid (no host changes needed).
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include "hip/integer_adm_hip.h"
#include "common.h"

/* AMD HIP atomicAdd on 64-bit unsigned: native on gfx90a/gfx10+, CAS
 * fallback on older GCN. Same alias the CUDA helper uses, kept here so
 * the kernel body matches the CUDA twin line-for-line. */
typedef unsigned long long int uint64_cu;

template <int val_per_thread, int cta_size>
__device__ __forceinline__ static void
adm_csf_den_scale_line_kernel_impl(const hip_adm_dwt_band_t src, int /*h*/, int top, int bottom,
                                   int left, int right, int src_stride, uint64_t *accum)
{
    const int band = blockIdx.z + 1;
    int16_t *src_ptr;
    switch (band) {
    case 1:
        src_ptr = src.band_h + (top + blockIdx.y) * src_stride;
        break;
    case 2:
        src_ptr = src.band_v + (top + blockIdx.y) * src_stride;
        break;
    case 3:
        src_ptr = src.band_d + (top + blockIdx.y) * src_stride;
        break;
    default:
        return;
    }
    int j = left + (cta_size * (int)blockIdx.x) * val_per_thread + (int)threadIdx.x;

    uint64_cu temp_value = 0;
#pragma unroll
    for (int i = 0; i < val_per_thread; ++i) {
        if (j < right) {
            uint16_t t = (uint16_t)abs((int)src_ptr[j]);
            temp_value += ((uint64_t)t * t) * t;
        }
        j += cta_size;
    }

    /* Per-thread atomicAdd into the band accumulator — no warp reduce.
     * `__log2f`/`__float2uint_ru` are device-only; on HIP we use the
     * host-callable `log2f` / explicit cast to keep the formula bit-exact. */
    uint32_t shift_accum = (uint32_t)ceilf(log2f((float)((bottom - top) * (right - left))) - 20.f);
    if ((int32_t)shift_accum < 0)
        shift_accum = 0;
    int32_t add_shift_accum = shift_accum > 0 ? (1 << (shift_accum - 1)) : 0;
    atomicAdd((uint64_cu *)&accum[band - 1],
              (uint64_cu)((temp_value + (uint64_cu)add_shift_accum) >> shift_accum));
}

template <int val_per_thread, int cta_size>
__device__ __forceinline__ static void
adm_csf_den_s123_line_kernel_impl(const hip_i4_adm_dwt_band_t src, int /*h*/, int top, int bottom,
                                  int left, int right, int src_stride, uint32_t add_shift_sq,
                                  uint32_t shift_sq, uint64_t *accum)
{
    const uint32_t shift_cub = (uint32_t)ceilf(log2f((float)(right - left)));
    const uint32_t add_shift_cub = (uint32_t)1 << (shift_cub - 1);

    const int band = blockIdx.z + 1;
    int32_t *src_ptr;
    switch (band) {
    case 1:
        src_ptr = src.band_h + (top + blockIdx.y) * src_stride;
        break;
    case 2:
        src_ptr = src.band_v + (top + blockIdx.y) * src_stride;
        break;
    case 3:
        src_ptr = src.band_d + (top + blockIdx.y) * src_stride;
        break;
    default:
        return;
    }
    int j = left + (cta_size * (int)blockIdx.x) * val_per_thread + (int)threadIdx.x;

    uint64_cu temp_value = 0;
#pragma unroll
    for (int i = 0; i < val_per_thread; ++i) {
        if (j < right) {
            uint32_t t = (uint32_t)abs(src_ptr[j]);
            temp_value +=
                (((((((uint64_t)t * t) + add_shift_sq) >> shift_sq) * t) + add_shift_cub) >>
                 shift_cub);
        }
        j += cta_size;
    }

    uint32_t shift_accum = (uint32_t)ceilf(log2f((float)(bottom - top)));
    uint32_t add_shift_accum = (uint32_t)1 << (shift_accum - 1);
    atomicAdd((uint64_cu *)&accum[band - 1],
              (uint64_cu)((temp_value + (uint64_cu)add_shift_accum) >> shift_accum));
}

#define ADM_CSF_SCALE_LINE(val_per_thread, cta_size)                                               \
    __global__ void adm_csf_den_scale_line_kernel_##val_per_thread##_##cta_size(                   \
        const hip_adm_dwt_band_t src, int h, int top, int bottom, int left, int right,             \
        int src_stride, uint64_t *accum)                                                           \
    {                                                                                              \
        adm_csf_den_scale_line_kernel_impl<val_per_thread, cta_size>(                              \
            src, h, top, bottom, left, right, src_stride, accum);                                  \
    }

#define ADM_CSF_DEN_S123_LINE(val_per_thread, cta_size)                                            \
    __global__ void adm_csf_den_s123_line_kernel_##val_per_thread##_##cta_size(                    \
        const hip_i4_adm_dwt_band_t src, int h, int top, int bottom, int left, int right,          \
        int src_stride, uint32_t add_shift_sq, uint32_t shift_sq, uint64_t *accum)                 \
    {                                                                                              \
        adm_csf_den_s123_line_kernel_impl<val_per_thread, cta_size>(                               \
            src, h, top, bottom, left, right, src_stride, add_shift_sq, shift_sq, accum);          \
    }

extern "C" {
/* Same launch shape as the CUDA twin: cta_size=128, val_per_thread=8.
 * Per-thread atomicAdd replaces the warp-reduce + per-warp atomicAdd
 * pattern (ADR-0539) — bit-exact since uint64 add is associative. */
ADM_CSF_SCALE_LINE(8, 128);    /* adm_csf_den_scale_line_kernel_8_128 */
ADM_CSF_DEN_S123_LINE(8, 128); /* adm_csf_den_s123_line_kernel_8_128 */
}
