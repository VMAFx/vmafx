/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

/*
 * ADM denominator (reference-band energy) reductions on CUDA.
 *
 * The CPU reference (integer_adm_kernels.h) adds the cube terms of one row
 * into `inner` and folds the row into the band accumulator with one rounding
 * shift, `accum += (inner + add_shift_accum) >> shift_accum`
 * (adm_csf_den_fold()). The shift discards bits, so where it is applied is
 * part of the result: rounding each warp of a row on its own, as these kernels
 * did until ADR-1416, gives a different accumulator.
 *
 * One block computes one row of one band (blockIdx.y = row inside the border
 * region, blockIdx.z = band). Every thread adds the terms of its columns, the
 * block reduces them to the row total, and thread 0 folds it once. The
 * rounding shifts come from the host, which takes them from the CPU's own
 * context initialisers; nothing here evaluates a logarithm.
 */

#ifndef DEVICE_CODE
#include "feature_collector.h"
#endif
#include "cuda/integer_adm_cuda.h"

#include "adm_cm_accumulator.h"
#include "common.h"
#include "cuda_helper.cuh"

#define ADM_CSF_DEN_THREADS 128
#define ADM_CSF_DEN_WARPS (ADM_CSF_DEN_THREADS / VMAF_CUDA_THREADS_PER_WARP)

/* adm_csf_den_fold(): the row total of the block, rounded once and added to
 * the band accumulator. Every thread of the block must call this, because it
 * synchronises the block. */
__device__ __forceinline__ void adm_csf_den_fold_row(uint64_cu thread_sum, uint32_t add_shift_accum,
                                                     uint32_t shift_accum, uint64_t *band_accum)
{
    const uint64_cu lane_sum = (uint64_cu)warp_reduce((int64_t)thread_sum);
    __shared__ uint64_cu warp_sums[ADM_CSF_DEN_WARPS];
    if ((threadIdx.x % VMAF_CUDA_THREADS_PER_WARP) == 0) {
        warp_sums[threadIdx.x / VMAF_CUDA_THREADS_PER_WARP] = lane_sum;
    }
    __syncthreads();

    if (threadIdx.x == 0) {
        uint64_cu row_total = 0;
        for (int w_idx = 0; w_idx < ADM_CSF_DEN_WARPS; ++w_idx) {
            row_total += warp_sums[w_idx];
        }
        atomicAdd(reinterpret_cast<uint64_cu *>(band_accum),
                  (uint64_cu)adm_csf_den_round_row_total(row_total, add_shift_accum, shift_accum));
    }
}

/* Row blockIdx.y of the band blockIdx.z selects (h, v, d). The switch keeps
 * the three band pointers in registers; indexing `bands[]` reloads them. */
template <typename Band, typename Sample>
__device__ __forceinline__ const Sample *adm_csf_den_row(const Band &src, int top, int src_stride)
{
    const ptrdiff_t offset = (ptrdiff_t)(top + (int)blockIdx.y) * src_stride;
    switch (blockIdx.z) {
    case 0:
        return src.band_h + offset;
    case 1:
        return src.band_v + offset;
    default:
        return src.band_d + offset;
    }
}

extern "C" {

/* adm_csf_den_scale(): the cubes of the 16-bit scale-0 band. */
__global__ void __launch_bounds__(ADM_CSF_DEN_THREADS)
    adm_csf_den_scale_row_kernel(const cuda_adm_dwt_band_t src, int top, int left, int right,
                                 int src_stride, uint32_t add_shift_accum, uint32_t shift_accum,
                                 uint64_t *accum)
{
    const int16_t *src_ptr = adm_csf_den_row<cuda_adm_dwt_band_t, int16_t>(src, top, src_stride);

    uint64_cu thread_sum = 0;
    for (int j = left + (int)threadIdx.x; j < right; j += ADM_CSF_DEN_THREADS) {
        const uint16_t t = (uint16_t)abs(src_ptr[j]);
        thread_sum += ((uint64_t)t * t) * t;
    }
    adm_csf_den_fold_row(thread_sum, add_shift_accum, shift_accum, &accum[blockIdx.z]);
}

/* adm_csf_den_s123(): i4_cube_term() of the 32-bit bands of scales 1 to 3. */
__global__ void __launch_bounds__(ADM_CSF_DEN_THREADS)
    adm_csf_den_s123_row_kernel(const cuda_i4_adm_dwt_band_t src, int top, int left, int right,
                                int src_stride, uint32_t add_shift_sq, uint32_t shift_sq,
                                uint32_t add_shift_cub, uint32_t shift_cub,
                                uint32_t add_shift_accum, uint32_t shift_accum, uint64_t *accum)
{
    const int32_t *src_ptr = adm_csf_den_row<cuda_i4_adm_dwt_band_t, int32_t>(src, top, src_stride);

    uint64_cu thread_sum = 0;
    for (int j = left + (int)threadIdx.x; j < right; j += ADM_CSF_DEN_THREADS) {
        const uint32_t t = (uint32_t)abs(src_ptr[j]);
        thread_sum +=
            (((((((uint64_t)t * t) + add_shift_sq) >> shift_sq) * t) + add_shift_cub) >> shift_cub);
    }
    adm_csf_den_fold_row(thread_sum, add_shift_accum, shift_accum, &accum[blockIdx.z]);
}

} /* extern "C" */
