/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
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
 *  ADM denominator (reference-band energy) reductions on HIP.
 *
 *  The CPU reference (integer_adm_kernels.h) adds the cube terms of one row
 *  into `inner` and folds the row into the band accumulator with one rounding
 *  shift, `accum += (inner + add_shift_accum) >> shift_accum`
 *  (adm_csf_den_fold()). The shift discards bits, so where it is applied is
 *  part of the result. Until ADR-1423 these kernels rounded every thread's
 *  partial sum of a row on its own, which gives a different accumulator: on a
 *  gfx1036 the scale-2 and scale-3 scores of a low-detail frame were 8e-8 and
 *  4e-7 from the CPU's, and scale 0 of a 3840x2160 frame 1e-7.
 *
 *  One block computes one row of one band (blockIdx.y = row inside the border
 *  region, blockIdx.z = band). Every thread adds the terms of its columns, the
 *  block adds the thread sums to the row total (uint64 addition, any order),
 *  and thread 0 folds it once through adm_csf_den_round_row_total(), the
 *  function the CPU's adm_csf_den_fold() calls (ADR-1416). The border and
 *  every rounding shift come from the host, which takes them from the CPU's
 *  own context initialisers; nothing here evaluates a logarithm (fp32
 *  `log2f(area) - 20` is off by one for 81 region areas just above a power of
 *  two, and a shift the host concludes with differently doubles the
 *  denominator).
 */

#include <hip/hip_runtime.h>
#include <cstdint>

#include "hip/integer_adm_hip.h"
#include "adm_cm_accumulator.h"
#include "common.h"

#define ADM_CSF_DEN_THREADS 128

/* AMD HIP atomicAdd on 64-bit unsigned: native on gfx90a/gfx10+, CAS
 * fallback on older GCN. */
using uint64_cu = unsigned long long int;

namespace {

/* adm_csf_den_fold(): the row total of the block, rounded once and added to
 * the band accumulator. Every thread of the block must call this, because it
 * synchronises the block. */
__device__ __forceinline__ void adm_csf_den_fold_row(uint64_cu thread_sum, uint32_t add_shift_accum,
                                                     uint32_t shift_accum, uint64_t *band_accum)
{
    __shared__ uint64_cu thread_sums[ADM_CSF_DEN_THREADS];
    thread_sums[threadIdx.x] = thread_sum;
    __syncthreads();

    if (threadIdx.x != 0)
        return;
    uint64_cu row_total = 0;
    for (const uint64_cu term : thread_sums)
        row_total += term;
    atomicAdd(reinterpret_cast<uint64_cu *>(band_accum),
              (uint64_cu)adm_csf_den_round_row_total(row_total, add_shift_accum, shift_accum));
}

/* Row blockIdx.y of the band blockIdx.z selects (h, v, d). */
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

} /* namespace */

extern "C" {

/* adm_csf_den_scale(): the cubes of the 16-bit scale-0 band. */
__global__ void adm_csf_den_scale_row_kernel(const hip_adm_dwt_band_t src, int top, int left,
                                             int right, int src_stride, uint32_t add_shift_accum,
                                             uint32_t shift_accum, uint64_t *accum)
{
    const int16_t *src_ptr = adm_csf_den_row<hip_adm_dwt_band_t, int16_t>(src, top, src_stride);

    uint64_cu thread_sum = 0;
    for (int j = left + (int)threadIdx.x; j < right; j += ADM_CSF_DEN_THREADS) {
        const uint16_t t = (uint16_t)abs((int)src_ptr[j]);
        thread_sum += ((uint64_t)t * t) * t;
    }
    adm_csf_den_fold_row(thread_sum, add_shift_accum, shift_accum, &accum[blockIdx.z]);
}

/* adm_csf_den_s123(): i4_cube_term() of the 32-bit bands of scales 1 to 3. */
__global__ void adm_csf_den_s123_row_kernel(const hip_i4_adm_dwt_band_t src, int top, int left,
                                            int right, int src_stride, uint32_t add_shift_sq,
                                            uint32_t shift_sq, uint32_t add_shift_cub,
                                            uint32_t shift_cub, uint32_t add_shift_accum,
                                            uint32_t shift_accum, uint64_t *accum)
{
    const int32_t *src_ptr = adm_csf_den_row<hip_i4_adm_dwt_band_t, int32_t>(src, top, src_stride);

    uint64_cu thread_sum = 0;
    for (int j = left + (int)threadIdx.x; j < right; j += ADM_CSF_DEN_THREADS) {
        const uint32_t t = (uint32_t)abs(src_ptr[j]);
        thread_sum +=
            (((((((uint64_t)t * t) + add_shift_sq) >> shift_sq) * t) + add_shift_cub) >> shift_cub);
    }
    adm_csf_den_fold_row(thread_sum, add_shift_accum, shift_accum, &accum[blockIdx.z]);
}

} /* extern "C" */
