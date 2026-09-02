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

#ifndef DEVICE_CODE
#include "feature_collector.h"
#endif
#include "cuda/integer_adm_cuda.h"

#include "common.h"

#include <assert.h>

#include "cuda_helper.cuh"
#include "adm_decouple_inline.cuh"

template <int cols_per_thread>
static __device__ __forceinline__ void copy_vec_4(const int32_t *__restrict__ in,
                                                  int32_t *__restrict__ out)
{
    //__builtin_assume_aligned(in, 16);
    //__builtin_assume_aligned(out, 16);

    static_assert(cols_per_thread % 4 == 0, "implemented only for a multiple of 4");
#pragma unroll
    for (int col = 0; col < cols_per_thread; col += 4) {
        *reinterpret_cast<uint4 *>(out + col) = *reinterpret_cast<const uint4 *>(in + col);
    }
}

template <int cols_per_thread>
static __device__ __forceinline__ void copy_vec_4(const int16_t *__restrict__ in,
                                                  int16_t *__restrict__ out)
{
    // __builtin_assume_aligned(in, 8);
    // __builtin_assume_aligned(out, 8);

    static_assert(cols_per_thread % 4 == 0, "implemented only for a multiple of 4");
#pragma unroll
    for (int col = 0; col < cols_per_thread; col += 4) {
        *reinterpret_cast<ushort4 *>(out + col) = *reinterpret_cast<const ushort4 *>(in + col);
    }
}

/* Scales 1-3 CSF with inline decouple — reads ref/dis DWT2, writes only csf_f */
template <int rows_per_thread, int cols_per_thread>
__device__ __forceinline__ void i4_adm_csf_kernel(AdmBufferCuda buf, int scale, int top, int bottom,
                                                  int left, int right, int stride,
                                                  AdmFixedParametersCuda params)
{

    const int band = blockIdx.z + 1;
    const cuda_i4_adm_dwt_band_t *ref = &buf.i4_ref_dwt2;
    const cuda_i4_adm_dwt_band_t *dis = &buf.i4_dis_dwt2;
    int32_t *flt_ptr = buf.i4_csf_f.bands[band];

    /* F3: extract __restrict__ read-only band pointers before the hot inner
     * loop so the compiler can route all loads through the L1 read-only cache
     * via __ldg().  AdmBufferCuda is passed by value, hiding the sub-struct
     * pointers from ptxas alias analysis; the one-time extraction here makes
     * the alias-free invariant visible (ADR-0773, mirrors ADR-0763/ADR-0754). */
    const int32_t *__restrict__ ref_h = ref->band_h;
    const int32_t *__restrict__ ref_v = ref->band_v;
    const int32_t *__restrict__ ref_d = ref->band_d;
    const int32_t *__restrict__ dis_h = dis->band_h;
    const int32_t *__restrict__ dis_v = dis->band_v;
    const int32_t *__restrict__ dis_d = dis->band_d;

    int y = top + (blockIdx.y * blockDim.y + threadIdx.y) * rows_per_thread;
    int x = left + (blockIdx.x * blockDim.x + threadIdx.x) * cols_per_thread;

    const uint32_t i_rfactor = params.i_rfactor[scale * 3 + blockIdx.z];
    const uint32_t FIX_ONE_BY_30 = 143165577;
    const uint32_t shift_dst = 28;
    const uint32_t shift_flt = 32;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));
    const int32_t add_bef_shift_flt = (1u << (shift_flt - 1));

    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    const int offset = y * stride + x;

    if (y < bottom && x < right) {
        __align__(16) int32_t flt_vec[cols_per_thread];

        for (int row = 0; row < rows_per_thread; ++row) {
            const int row_off = offset + row * stride;

            for (int col = 0; col < cols_per_thread; ++col) {
                const int idx = row_off + col;
                int32_t oh = __ldg(&ref_h[idx]);
                int32_t ov = __ldg(&ref_v[idx]);
                int32_t od = __ldg(&ref_d[idx]);
                int32_t th = __ldg(&dis_h[idx]);
                int32_t tv = __ldg(&dis_v[idx]);
                int32_t td = __ldg(&dis_d[idx]);

                int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
                int32_t r_val = decouple_r_s123(oh, ov, od, th, tv, td, band - 1, angle_flag,
                                                adm_enhn_gain_limit);

                int32_t t_val;
                if (band == 1)
                    t_val = th;
                else if (band == 2)
                    t_val = tv;
                else
                    t_val = td;

                int32_t a_val = t_val - r_val;

                int32_t dst_val =
                    (int32_t)(((i_rfactor * int64_t(a_val)) + add_bef_shift_dst) >> shift_dst);
                flt_vec[col] =
                    (int32_t)((((int64_t)FIX_ONE_BY_30 * abs(dst_val)) + add_bef_shift_flt) >>
                              shift_flt);
            }
            copy_vec_4<cols_per_thread>(flt_vec, flt_ptr + row_off);
        }
    }
}

__constant__ const uint8_t i_shifts[4] = {0, 15, 15, 17};
__constant__ const uint16_t i_shiftsadd[4] = {0, 16384, 16384, 65535};

/* Scale-0 CSF with inline decouple — reads ref/dis DWT2, writes only csf_f */
template <int rows_per_thread, int cols_per_thread>
__device__ __forceinline__ void adm_csf_kernel(AdmBufferCuda buf, int top, int bottom, int left,
                                               int right, int stride, AdmFixedParametersCuda params)
{
    const int band = blockIdx.z + 1;

    const cuda_adm_dwt_band_t *ref = &buf.ref_dwt2;
    const cuda_adm_dwt_band_t *dis = &buf.dis_dwt2;
    int16_t *flt_ptr = buf.csf_f.bands[band];

    /* F3: extract __restrict__ read-only band pointers before the hot inner
     * loop (ADR-0773, mirrors ADR-0763/ADR-0754). */
    const int16_t *__restrict__ ref_h = ref->band_h;
    const int16_t *__restrict__ ref_v = ref->band_v;
    const int16_t *__restrict__ ref_d = ref->band_d;
    const int16_t *__restrict__ dis_h = dis->band_h;
    const int16_t *__restrict__ dis_v = dis->band_v;
    const int16_t *__restrict__ dis_d = dis->band_d;

    int y = top + (blockIdx.y * blockDim.y + threadIdx.y) * rows_per_thread;
    int x = left + (blockIdx.x * blockDim.x + threadIdx.x) * cols_per_thread;

    const uint32_t i_rfactor = params.i_rfactor[blockIdx.z];
    const uint16_t FIX_ONE_BY_30 = 4369; //(1/30)*2^17

    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    if (y < bottom && x < right) {
        __align__(8) int16_t flt_vec[cols_per_thread];

        const int offset = y * stride + x;

        for (int row = 0; row < rows_per_thread; ++row) {
            const int row_off = offset + row * stride;

            for (int col = 0; col < cols_per_thread; ++col) {
                const int idx = row_off + col;
                int16_t oh = __ldg(&ref_h[idx]);
                int16_t ov = __ldg(&ref_v[idx]);
                int16_t od = __ldg(&ref_d[idx]);
                int16_t th = __ldg(&dis_h[idx]);
                int16_t tv = __ldg(&dis_v[idx]);
                int16_t td = __ldg(&dis_d[idx]);

                int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
                int16_t r_val = decouple_r_s0(oh, ov, od, th, tv, td, band - 1, angle_flag,
                                              adm_enhn_gain_limit);

                int16_t t_val;
                if (band == 1)
                    t_val = th;
                else if (band == 2)
                    t_val = tv;
                else
                    t_val = td;

                int16_t a_val = t_val - r_val;

                int32_t dst_val = i_rfactor * (uint32_t)a_val;
                int16_t i16_dst_val = (dst_val + i_shiftsadd[band]) >> i_shifts[band];
                flt_vec[col] = ((FIX_ONE_BY_30 * abs((int32_t)i16_dst_val)) + 2048) >> 12;
            }
            copy_vec_4<cols_per_thread>(flt_vec, flt_ptr + row_off);
        }
    }
}

#define ADM_CSF_KERNEL(rows_per_thread, cols_per_thread)                                           \
    __global__ void adm_csf_kernel_##rows_per_thread##_##cols_per_thread(                          \
        AdmBufferCuda buf, int top, int bottom, int left, int right, int stride,                   \
        AdmFixedParametersCuda params)                                                             \
    {                                                                                              \
        adm_csf_kernel<rows_per_thread, cols_per_thread>(buf, top, bottom, left, right, stride,    \
                                                         params);                                  \
    }

#define I4_ADM_CSF_KERNEL(rows_per_thread, cols_per_thread)                                        \
    __global__ void i4_adm_csf_kernel_##rows_per_thread##_##cols_per_thread(                       \
        AdmBufferCuda buf, int scale, int top, int bottom, int left, int right, int stride,        \
        AdmFixedParametersCuda params)                                                             \
    {                                                                                              \
        i4_adm_csf_kernel<rows_per_thread, cols_per_thread>(buf, scale, top, bottom, left, right,  \
                                                            stride, params);                       \
    }

extern "C" {
ADM_CSF_KERNEL(1, 4);    // adm_csf_kernel_1_4
I4_ADM_CSF_KERNEL(1, 4); // i4_adm_csf_kernel_1_4
}
