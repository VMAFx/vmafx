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
#include "cuda_helper.cuh"
#include "adm_decouple_inline.cuh"

#include <algorithm>

//#define COMPARE_FUSED_SPLIT
#if defined(COMPARE_FUSED_SPLIT)
#include <iostream>
#endif

/* Inline helper: compute csf_a for a single band at a given pixel position
 * for scales 1-3 (i4 path). Returns the csf_a value = rfactor * a_val. */
__device__ __forceinline__ int32_t inline_i4_csf_a(const cuda_i4_adm_dwt_band_t *ref,
                                                   const cuda_i4_adm_dwt_band_t *dis, int idx,
                                                   int theta, /* theta 0..2 → band h/v/d */
                                                   const uint32_t *rfactor,
                                                   double adm_enhn_gain_limit)
{
    const uint32_t shift_dst = 28;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));

    /* F3: extract __restrict__ pointers so __ldg() routes reads through the
     * L1 read-only cache (ADR-0773). */
    const int32_t *__restrict__ rh = ref->band_h;
    const int32_t *__restrict__ rv = ref->band_v;
    const int32_t *__restrict__ rd = ref->band_d;
    const int32_t *__restrict__ dh = dis->band_h;
    const int32_t *__restrict__ dv = dis->band_v;
    const int32_t *__restrict__ dd = dis->band_d;
    int32_t oh = __ldg(&rh[idx]);
    int32_t ov = __ldg(&rv[idx]);
    int32_t od = __ldg(&rd[idx]);
    int32_t th = __ldg(&dh[idx]);
    int32_t tv = __ldg(&dv[idx]);
    int32_t td = __ldg(&dd[idx]);

    int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
    int32_t r_val = decouple_r_s123(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);

    int32_t t_val;
    if (theta == 0)
        t_val = th;
    else if (theta == 1)
        t_val = tv;
    else
        t_val = td;

    int32_t a_val = t_val - r_val;

    /* dst_val = rfactor * a_val — this is csf_a */
    return (int32_t)(((rfactor[theta] * int64_t(a_val)) + add_bef_shift_dst) >> shift_dst);
}

/* Inline helper: compute decouple_r for a single band at a given pixel position
 * for scales 1-3 (i4 path). */
__device__ __forceinline__ int32_t inline_i4_decouple_r(const cuda_i4_adm_dwt_band_t *ref,
                                                        const cuda_i4_adm_dwt_band_t *dis, int idx,
                                                        int band_idx, /* band_idx 0..2 → h/v/d */
                                                        double adm_enhn_gain_limit)
{
    /* F3: __restrict__ extraction + __ldg() (ADR-0773). */
    const int32_t *__restrict__ rh = ref->band_h;
    const int32_t *__restrict__ rv = ref->band_v;
    const int32_t *__restrict__ rd = ref->band_d;
    const int32_t *__restrict__ dh = dis->band_h;
    const int32_t *__restrict__ dv = dis->band_v;
    const int32_t *__restrict__ dd = dis->band_d;
    int32_t oh = __ldg(&rh[idx]);
    int32_t ov = __ldg(&rv[idx]);
    int32_t od = __ldg(&rd[idx]);
    int32_t th = __ldg(&dh[idx]);
    int32_t tv = __ldg(&dv[idx]);
    int32_t td = __ldg(&dd[idx]);

    int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
    return decouple_r_s123(oh, ov, od, th, tv, td, band_idx, angle_flag, adm_enhn_gain_limit);
}

/* Inline helper: compute csf_a for scale 0 (int16 path). */
__device__ __forceinline__ int16_t inline_s0_csf_a(const cuda_adm_dwt_band_t *ref,
                                                   const cuda_adm_dwt_band_t *dis, int idx,
                                                   int theta, /* theta 0..2 → band h/v/d */
                                                   const uint32_t *i_rfactor,
                                                   double adm_enhn_gain_limit)
{
    __constant__ static const uint8_t i_shifts_cm[4] = {0, 15, 15, 17};
    __constant__ static const uint16_t i_shiftsadd_cm[4] = {0, 16384, 16384, 65535};

    /* F3: __restrict__ extraction + __ldg() (ADR-0773). */
    const int16_t *__restrict__ rh = ref->band_h;
    const int16_t *__restrict__ rv = ref->band_v;
    const int16_t *__restrict__ rd = ref->band_d;
    const int16_t *__restrict__ dh = dis->band_h;
    const int16_t *__restrict__ dv = dis->band_v;
    const int16_t *__restrict__ dd = dis->band_d;
    int16_t oh = __ldg(&rh[idx]);
    int16_t ov = __ldg(&rv[idx]);
    int16_t od = __ldg(&rd[idx]);
    int16_t th = __ldg(&dh[idx]);
    int16_t tv = __ldg(&dv[idx]);
    int16_t td = __ldg(&dd[idx]);

    int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    int16_t r_val = decouple_r_s0(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);

    int16_t t_val;
    if (theta == 0)
        t_val = th;
    else if (theta == 1)
        t_val = tv;
    else
        t_val = td;

    int16_t a_val = t_val - r_val;

    int band = theta + 1; // band index 1..3
    int32_t dst_val = i_rfactor[theta] * (uint32_t)a_val;
    return (dst_val + i_shiftsadd_cm[band]) >> i_shifts_cm[band];
}

/* Inline helper: compute decouple_r for scale 0 (int16 path). */
__device__ __forceinline__ int16_t inline_s0_decouple_r(const cuda_adm_dwt_band_t *ref,
                                                        const cuda_adm_dwt_band_t *dis, int idx,
                                                        int band_idx, /* band_idx 0..2 → h/v/d */
                                                        double adm_enhn_gain_limit)
{
    /* F3: __restrict__ extraction + __ldg() (ADR-0773). */
    const int16_t *__restrict__ rh = ref->band_h;
    const int16_t *__restrict__ rv = ref->band_v;
    const int16_t *__restrict__ rd = ref->band_d;
    const int16_t *__restrict__ dh = dis->band_h;
    const int16_t *__restrict__ dv = dis->band_v;
    const int16_t *__restrict__ dd = dis->band_d;
    int16_t oh = __ldg(&rh[idx]);
    int16_t ov = __ldg(&rv[idx]);
    int16_t od = __ldg(&rd[idx]);
    int16_t th = __ldg(&dh[idx]);
    int16_t tv = __ldg(&dv[idx]);
    int16_t td = __ldg(&dd[idx]);

    int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    return decouple_r_s0(oh, ov, od, th, tv, td, band_idx, angle_flag, adm_enhn_gain_limit);
}

/* Fused compute + warp-reduce + atomicAdd kernel for ADM CM scales 1-3 (i4 path).
 * Eliminates the separate adm_cm_reduce_line_kernel_4 launch and the accum_per_thread
 * scratch buffer round-trip.  Scale 0 uses adm_cm_line_kernel_8 (int16 path);
 * this kernel mirrors its warp-reduce + atomicAdd_int64 pattern for int32. */
extern "C" {
__global__ void i4_adm_cm_line_kernel_fused(AdmBufferCuda buf, int h, int w, int top, int bottom,
                                            int left, int right, int start_row, int end_row,
                                            int start_col, int end_col, int src_stride,
                                            int csf_a_stride, int scale, int64_t *accum_global,
                                            AdmFixedParametersCuda params)
{
    const cuda_i4_adm_dwt_band_t *ref = &buf.i4_ref_dwt2;
    const cuda_i4_adm_dwt_band_t *dis = &buf.i4_dis_dwt2;
    const cuda_i4_adm_dwt_band_t *csf_f = &buf.i4_csf_f;
    const int band = blockIdx.z + 1;
    int32_t *const *flt_angles = csf_f->bands + 1;

    const uint32_t *rfactor = &params.i_rfactor[scale * 3];
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    const uint32_t shift_flt = 32;
    const int32_t add_bef_shift_flt = (1u << (shift_flt - 1));
    const uint32_t shift_dst = 28;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));

    /* Cubic-accumulation shifts — match adm_cm_reduce_line_kernel for scale != 0. */
    const uint32_t shift_sq = 30;
    const int32_t add_shift_sq = 536870912; /* 1 << 29 */
    const uint32_t shift_cub = __float2uint_ru(__log2f((float)w));
    const int32_t add_shift_cub = (int32_t)(1u << (shift_cub - 1));
    const uint32_t shift_inner_accum = __float2uint_ru(__log2f((float)h));
    const int32_t add_shift_inner_accum = (int32_t)(1u << (shift_inner_accum - 1));

    const int32_t shift_sub = 0;

    int i = start_row + blockIdx.y;
    int j = start_col + blockIdx.x * blockDim.x + threadIdx.x;

    /* Per-pixel compute (identical to the retired i4_adm_cm_line_kernel). */
    int32_t accum_thread = 0;
    if (i < end_row && j < end_col) {

        int16_t offset_i[2] = {-1, 1};
        if (i == 0 && top <= 0) {
            offset_i[0] = 1;
        } else if (i == (h - 1) && bottom > (h - 1)) {
            offset_i[1] = 0;
        }

        int16_t offset_j[2] = {-1, 1};
        if (j == 0 && left <= 0) {
            offset_j[0] = 1;
        } else if (j == (w - 1) && right > (w - 1)) {
            offset_j[1] = 0;
        }

        int32_t thr = 0;
        for (int theta = 0; theta < 3; ++theta) {
            int32_t sum = 0;
            /* Inline csf_a at center pixel [i, j] */
            int32_t csf_a_val = inline_i4_csf_a(ref, dis, src_stride * (i + offset_i[0] + 1) + j,
                                                theta, rfactor, adm_enhn_gain_limit);

            int32_t *flt_ptr_line = flt_angles[theta];
            flt_ptr_line += (src_stride * (i + offset_i[0]));
            sum += flt_ptr_line[j + offset_j[0]];
            sum += flt_ptr_line[j];
            sum += flt_ptr_line[j + offset_j[1]];
            flt_ptr_line += src_stride;
            sum += flt_ptr_line[j + offset_j[0]];
            sum += (int32_t)((((int64_t)I4_ONE_BY_15 * abs(csf_a_val)) + add_bef_shift_flt) >>
                             shift_flt);
            sum += flt_ptr_line[j + offset_j[1]];
            flt_ptr_line += src_stride * offset_i[1];
            sum += flt_ptr_line[j + offset_j[0]];
            sum += flt_ptr_line[j];
            sum += flt_ptr_line[j + offset_j[1]];
            thr += sum;
        }
        /* Inline decouple_r at pixel [i, j] */
        int32_t r_val =
            inline_i4_decouple_r(ref, dis, i * src_stride + j, band - 1, adm_enhn_gain_limit);
        int32_t x =
            (int32_t)((((int64_t)r_val * rfactor[blockIdx.z]) + add_bef_shift_dst) >> shift_dst);
        x = abs(x) - (thr >> shift_sub);
        accum_thread = x < 0 ? 0 : x;
    }

    /* Warp-level cubic accumulation + reduction — eliminates the separate reduce kernel
     * and the accum_per_thread scratch buffer round-trip (mirrors adm_cm_line_kernel_8). */
    const int32_t x_sq =
        (int32_t)(((int64_t)accum_thread * accum_thread + add_shift_sq) >> shift_sq);
    int64_t lane_accum = (((int64_t)x_sq * accum_thread) + add_shift_cub) >> shift_cub;
    lane_accum = warp_reduce(lane_accum);

    if ((threadIdx.x % VMAF_CUDA_THREADS_PER_WARP) == 0) {
        atomicAdd_int64(&accum_global[blockIdx.z],
                        (lane_accum + add_shift_inner_accum) >> shift_inner_accum);
    }
}
}
__constant__ const int32_t shift_sub[3] = {10, 10, 12};
// HACK: the 256 byte alignment is required to ensure that the struct is not moved to lmem
struct WarpShift {
    uint32_t shift_cub[3];
    uint32_t add_shift_cub[3];
    uint32_t shift_sq[3];
    uint32_t add_shift_sq[3];
};

template <int rows_per_thread>
__device__ __forceinline__ void
adm_cm_line_kernel(AdmBufferCuda buf, int h, int w, int top, int bottom, int left, int right,
                   int start_row, int end_row, int start_col, int end_col, int src_stride,
                   int csf_a_stride, int buffer_h, int buffer_stride, int32_t *accum_per_block,
                   AdmFixedParametersCuda params,
                   // reduce
                   int scale, int64_t *accum_global,

                   // shift warp
                   WarpShift ws,
                   // shift global
                   const uint32_t shift_inner_accum, const uint32_t add_shift_inner_accum)
{
    const cuda_adm_dwt_band_t *ref = &buf.ref_dwt2;
    const cuda_adm_dwt_band_t *dis = &buf.dis_dwt2;
    const cuda_adm_dwt_band_t *csf_f = &buf.csf_f;
    const int band = blockIdx.z + 1;
    int16_t *const *flt_angles = csf_f->bands + 1;

    uint32_t *i_rfactor = params.i_rfactor;
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    int cta_y = (blockDim.y * blockIdx.y + threadIdx.y) * rows_per_thread;
    int cta_x = blockDim.x * blockIdx.x + threadIdx.x;
    int y = start_row + cta_y;
    int x = start_col + cta_x;

    const int total_rows = (3 + rows_per_thread - 1);
    int32_t thr[rows_per_thread] = {0};

    if (y < end_row && x < end_col) {
        int pos_x[3] = {x - 1, x, x + 1};
        pos_x[0] = abs(pos_x[0]);
        pos_x[2] = pos_x[2] - max(0, 2 * (x - w) + 1);

#pragma unroll
        for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
            for (int row = 0; row < total_rows; ++row) {
                int pos_y = y - 1 + row;
                pos_y = abs(pos_y);
                pos_y = pos_y - max(0, 2 * (y - h) + 1);

                /* Inline csf_a at center pixel */
                int16_t csf_a_val = inline_s0_csf_a(ref, dis, pos_y * src_stride + x, theta,
                                                    i_rfactor, adm_enhn_gain_limit);
                int16_t *flt_ptr = flt_angles[theta] + pos_y * src_stride;
                int16_t flt_row[3] = {flt_ptr[pos_x[0]], flt_ptr[pos_x[1]], flt_ptr[pos_x[2]]};

#pragma unroll
                for (int thread_item = 0; thread_item < rows_per_thread; ++thread_item) {
                    int thread_row = row - thread_item;
                    if (thread_row >= 0 && thread_row < 3) {
                        thr[thread_item] += flt_row[0] + flt_row[2];
                        if (thread_row != 1) {
                            thr[thread_item] += flt_row[1];
                        } else {
                            thr[thread_item] +=
                                (int16_t)(((ONE_BY_15 * abs((int32_t)csf_a_val)) + 2048) >> 12);
                            ;
                        }
                    }
                }
            }
        }
    }

    int32_t shift_sub_block = shift_sub[blockIdx.z];
    int32_t accum_thread_reg[rows_per_thread] = {0};
    for (int row = 0; row < rows_per_thread; ++row) {
        int16_t sb = 0;
        if ((y + row) < end_row && x < end_col) {
            /* Inline decouple_r at pixel [y + row, x] */
            sb = inline_s0_decouple_r(ref, dis, (y + row) * src_stride + x, band - 1,
                                      adm_enhn_gain_limit);
        }
        int32_t val = abs(int32_t(i_rfactor[blockIdx.z] * sb)) - (thr[row] << shift_sub_block);
        accum_thread_reg[row] = max(0, val);
    }

    const int band2 = blockIdx.z;
    int64_t accum = 0;

    // the compiler does not assume that parameters are constant, move them to local variables to give the compiler
    // a hint that those values have to be loaded only once from constant memory.
    int32_t add_shift_cub = ws.add_shift_cub[band2];
    int32_t shift_cub = ws.shift_cub[band2];
    int32_t add_shift_sq = ws.add_shift_sq[band2];
    int32_t shift_sq = ws.shift_sq[band2];

    // accumulate per thread
    for (int row = 0; row < rows_per_thread; ++row) {
        int32_t accum_thread = accum_thread_reg[row];
        const int32_t x_sq =
            (int32_t)(((((int64_t)accum_thread * accum_thread) + add_shift_sq) >> shift_sq));
        accum += (((int64_t)x_sq * accum_thread) + add_shift_cub) >> shift_cub;
    }

    // accumulate warp
    accum = warp_reduce(accum);

    if (threadIdx.x % 32 == 0) {
        accum = (accum + add_shift_inner_accum) >> shift_inner_accum;
        atomicAdd_int64(&accum_global[band2], accum);
    }
}

/* adm_cm_reduce_line_kernel template and ADM_CM_REDUCE_LINE macro removed.
 * The two-kernel reduce pattern (compute → global scratch, then reduce separately)
 * has been superseded by i4_adm_cm_line_kernel_fused which integrates the
 * warp-reduce + atomicAdd_int64 step directly into the compute kernel.
 * Scale 0 already used the fused pattern (adm_cm_line_kernel_8); scales 1-3
 * were migrated in PR perf/adm-cm-cuda-warp-reduce-fusion. */

#define ADM_CM_LINE(rows_per_thread)                                                               \
    __global__ void adm_cm_line_kernel_##rows_per_thread(                                          \
        AdmBufferCuda buf, int h, int w, int top, int bottom, int left, int right, int start_row,  \
        int end_row, int start_col, int end_col, int src_stride, int csf_a_stride, int buffer_h,   \
        int buffer_stride, int32_t *accum_per_block, AdmFixedParametersCuda params, int scale,     \
        int64_t *accum_global, WarpShift ws, const uint32_t shift_inner_accum,                     \
        const uint32_t add_shift_inner_accum)                                                      \
    {                                                                                              \
        adm_cm_line_kernel<rows_per_thread>(                                                       \
            buf, h, w, top, bottom, left, right, start_row, end_row, start_col, end_col,           \
            src_stride, csf_a_stride, buffer_h, buffer_stride, accum_per_block, params, scale,     \
            accum_global, ws, shift_inner_accum, add_shift_inner_accum);                           \
    }

extern "C" {
// 128 = warps_per_thread * val_per_thread = 32 * 4 -- assuming 32 threads per warp, this might change in the future
/* adm_cm_reduce_line_kernel_4 removed: fused into i4_adm_cm_line_kernel_fused (scales 1-3). */
ADM_CM_LINE(8); // adm_cm_line_kernel_8
}

/* ============================================================================
 * AIM (Anchored Impairment Metric) CM kernels — PR #84 / ADR-0746
 *
 * AIM CM swaps the signal and threshold roles relative to normal DLM CM:
 *   signal    = rfactor * a_val    (CSF-weighted decouple_a)
 *   threshold = neighborhood of csf_r values (inline), where
 *               csf_r = rfactor * r_val  (CSF of decouple_r)
 *               8 neighbors: I4_FIX_ONE_BY_30 * |csf_r|
 *               center pixel: I4_ONE_BY_15   * |csf_r|  (= 2× neighbor)
 * noise_weight = 0 (no powf_add), matching CPU adm_cm(measure_aim=true).
 *
 * Root-cause fix for PR #84 verifier divergence: the AIM kernels were
 * missing from adm_cm.cu despite being described in the commit message.
 * Coefficient analysis confirms I4_ONE_BY_15 (center) / I4_FIX_ONE_BY_30
 * (neighbors) matches the CPU I4_ADM_CM_THRESH_* macro convention.
 * ============================================================================
 */

/* Fixed-point 1/30 × 2^32 = 143165577 — AIM neighbor threshold coefficient.
 * I4_ONE_BY_15 = 2 × I4_FIX_ONE_BY_30 (already defined in integer_adm.h). */
#define I4_FIX_ONE_BY_30 143165577u

/* Inline helper: CSF-scaled r_val for a single pixel, i4 path (scales 1-3).
 * Returns rfactor[theta] * r_val — used as the per-pixel AIM threshold. */
__device__ __forceinline__ int32_t inline_i4_csf_r(const cuda_i4_adm_dwt_band_t *ref,
                                                   const cuda_i4_adm_dwt_band_t *dis, int idx,
                                                   int theta, const uint32_t *rfactor,
                                                   double adm_enhn_gain_limit)
{
    const uint32_t shift_dst = 28;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));

    /* F3: __restrict__ extraction + __ldg() (ADR-0773). */
    const int32_t *__restrict__ rh = ref->band_h;
    const int32_t *__restrict__ rv = ref->band_v;
    const int32_t *__restrict__ rd = ref->band_d;
    const int32_t *__restrict__ dh = dis->band_h;
    const int32_t *__restrict__ dv = dis->band_v;
    const int32_t *__restrict__ dd = dis->band_d;
    int32_t oh = __ldg(&rh[idx]);
    int32_t ov = __ldg(&rv[idx]);
    int32_t od = __ldg(&rd[idx]);
    int32_t th = __ldg(&dh[idx]);
    int32_t tv = __ldg(&dv[idx]);
    int32_t td = __ldg(&dd[idx]);

    int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
    int32_t r_val = decouple_r_s123(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);
    return (int32_t)(((rfactor[theta] * (int64_t)r_val) + add_bef_shift_dst) >> shift_dst);
}

/* Inline helper: CSF-scaled r_val for a single pixel, scale 0 (int16 path). */
__device__ __forceinline__ int16_t inline_s0_csf_r(const cuda_adm_dwt_band_t *ref,
                                                   const cuda_adm_dwt_band_t *dis, int idx,
                                                   int theta, const uint32_t *i_rfactor,
                                                   double adm_enhn_gain_limit)
{
    __constant__ static const uint8_t i_shifts_cm[4] = {0, 15, 15, 17};
    __constant__ static const uint16_t i_shiftsadd_cm[4] = {0, 16384, 16384, 65535};

    /* F3: __restrict__ extraction + __ldg() (ADR-0773). */
    const int16_t *__restrict__ rh = ref->band_h;
    const int16_t *__restrict__ rv = ref->band_v;
    const int16_t *__restrict__ rd = ref->band_d;
    const int16_t *__restrict__ dh = dis->band_h;
    const int16_t *__restrict__ dv = dis->band_v;
    const int16_t *__restrict__ dd = dis->band_d;
    int16_t oh = __ldg(&rh[idx]);
    int16_t ov = __ldg(&rv[idx]);
    int16_t od = __ldg(&rd[idx]);
    int16_t th = __ldg(&dh[idx]);
    int16_t tv = __ldg(&dv[idx]);
    int16_t td = __ldg(&dd[idx]);

    int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    int16_t r_val = decouple_r_s0(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);
    int band = theta + 1;
    int32_t dst_val = i_rfactor[theta] * (uint32_t)r_val;
    return (dst_val + i_shiftsadd_cm[band]) >> i_shifts_cm[band];
}

extern "C" {

/* AIM CM fused kernel for scales 1-3 (i4 path).
 * Signal/threshold roles are swapped vs i4_adm_cm_line_kernel_fused.
 * Signal = rfactor * a_val; threshold = csf_r 3×3 neighbourhood (fully inline). */
__global__ void i4_adm_cm_aim_line_kernel_fused(AdmBufferCuda buf, int h, int w, int top,
                                                int bottom, int left, int right, int start_row,
                                                int end_row, int start_col, int end_col,
                                                int src_stride, int csf_a_stride, int scale,
                                                int64_t *accum_global,
                                                AdmFixedParametersCuda params)
{
    const cuda_i4_adm_dwt_band_t *ref = &buf.i4_ref_dwt2;
    const cuda_i4_adm_dwt_band_t *dis = &buf.i4_dis_dwt2;

    const uint32_t *rfactor = &params.i_rfactor[scale * 3];
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    const uint32_t shift_flt = 32;
    const int32_t add_bef_shift_flt = (1u << (shift_flt - 1));
    const uint32_t shift_sq = 30;
    const int32_t add_shift_sq = 536870912; /* 1 << 29 */
    const uint32_t shift_cub = __float2uint_ru(__log2f((float)w));
    const int32_t add_shift_cub = (int32_t)(1u << (shift_cub - 1));
    const uint32_t shift_inner_accum = __float2uint_ru(__log2f((float)h));
    const int32_t add_shift_inner_accum = (int32_t)(1u << (shift_inner_accum - 1));
    const int32_t shift_sub = 0;

    int i = start_row + blockIdx.y;
    int j = start_col + blockIdx.x * blockDim.x + threadIdx.x;

    int32_t accum_thread = 0;
    if (i < end_row && j < end_col) {

        int16_t offset_i[2] = {-1, 1};
        if (i == 0 && top <= 0)
            offset_i[0] = 1;
        else if (i == (h - 1) && bottom > (h - 1))
            offset_i[1] = 0;

        int16_t offset_j[2] = {-1, 1};
        if (j == 0 && left <= 0)
            offset_j[0] = 1;
        else if (j == (w - 1) && right > (w - 1))
            offset_j[1] = 0;

        int row_top = i + offset_i[0];
        int row_bot = i + offset_i[1];
        int col_l = j + offset_j[0];
        int col_r = j + offset_j[1];

        /* Threshold: sum across theta of the 3×3 csf_r neighbourhood.
         * Each position is computed inline to avoid extra device buffers.
         * Convention matches CPU I4_ADM_CM_THRESH_S_I_J macro:
         *   neighbors → I4_FIX_ONE_BY_30 * |csf_r|
         *   center    → I4_ONE_BY_15     * |csf_r|  (stored as angles[] in CPU) */
        int32_t thr = 0;
        for (int theta = 0; theta < 3; ++theta) {
            /* Compute csf_r for all 9 positions in the 3×3 window. */
            int32_t cr_tl = inline_i4_csf_r(ref, dis, row_top * src_stride + col_l, theta, rfactor,
                                            adm_enhn_gain_limit);
            int32_t cr_tc = inline_i4_csf_r(ref, dis, row_top * src_stride + j, theta, rfactor,
                                            adm_enhn_gain_limit);
            int32_t cr_tr = inline_i4_csf_r(ref, dis, row_top * src_stride + col_r, theta, rfactor,
                                            adm_enhn_gain_limit);
            int32_t cr_ml = inline_i4_csf_r(ref, dis, i * src_stride + col_l, theta, rfactor,
                                            adm_enhn_gain_limit);
            int32_t cr_mc =
                inline_i4_csf_r(ref, dis, i * src_stride + j, theta, rfactor, adm_enhn_gain_limit);
            int32_t cr_mr = inline_i4_csf_r(ref, dis, i * src_stride + col_r, theta, rfactor,
                                            adm_enhn_gain_limit);
            int32_t cr_bl = inline_i4_csf_r(ref, dis, row_bot * src_stride + col_l, theta, rfactor,
                                            adm_enhn_gain_limit);
            int32_t cr_bc = inline_i4_csf_r(ref, dis, row_bot * src_stride + j, theta, rfactor,
                                            adm_enhn_gain_limit);
            int32_t cr_br = inline_i4_csf_r(ref, dis, row_bot * src_stride + col_r, theta, rfactor,
                                            adm_enhn_gain_limit);

            /* Inline I4_FIX_ONE_BY_30 * |csf_r| for each neighbor position. */
#define AIM_NEIGHBOR(cr)                                                                           \
    ((int32_t)((((int64_t)I4_FIX_ONE_BY_30 * abs(cr)) + add_bef_shift_flt) >> shift_flt))
#define AIM_CENTER(cr)                                                                             \
    ((int32_t)((((int64_t)I4_ONE_BY_15 * abs(cr)) + add_bef_shift_flt) >> shift_flt))

            int32_t sum = 0;
            sum += AIM_NEIGHBOR(cr_tl);
            sum += AIM_NEIGHBOR(cr_tc);
            sum += AIM_NEIGHBOR(cr_tr);
            sum += AIM_NEIGHBOR(cr_ml);
            sum += AIM_CENTER(cr_mc); /* center: I4_ONE_BY_15 */
            sum += AIM_NEIGHBOR(cr_mr);
            sum += AIM_NEIGHBOR(cr_bl);
            sum += AIM_NEIGHBOR(cr_bc);
            sum += AIM_NEIGHBOR(cr_br);

#undef AIM_NEIGHBOR
#undef AIM_CENTER

            thr += sum;
        }

        /* Signal: rfactor * a_val = CSF of decouple_a. */
        int32_t x = inline_i4_csf_a(ref, dis, i * src_stride + j, (int)blockIdx.z, rfactor,
                                    adm_enhn_gain_limit);
        x = abs(x) - (thr >> shift_sub);
        accum_thread = x < 0 ? 0 : x;
    }

    const int32_t x_sq =
        (int32_t)(((int64_t)accum_thread * accum_thread + add_shift_sq) >> shift_sq);
    int64_t lane_accum = (((int64_t)x_sq * accum_thread) + add_shift_cub) >> shift_cub;
    lane_accum = warp_reduce(lane_accum);

    if ((threadIdx.x % VMAF_CUDA_THREADS_PER_WARP) == 0) {
        atomicAdd_int64(&accum_global[blockIdx.z],
                        (lane_accum + add_shift_inner_accum) >> shift_inner_accum);
    }
}

} /* extern "C" */

/* AIM CM device function for scale 0 (int16 path).
 * Mirrors adm_cm_line_kernel<rows_per_thread> with signal/threshold swapped.
 * Signal = inline_s0_csf_a; threshold = inline_s0_csf_r 3×3 neighbourhood. */
template <int rows_per_thread>
__device__ __forceinline__ void
adm_cm_aim_line_kernel(AdmBufferCuda buf, int h, int w, int top, int bottom, int left, int right,
                       int start_row, int end_row, int start_col, int end_col, int src_stride,
                       int csf_a_stride, int buffer_h, int buffer_stride, int32_t *accum_per_block,
                       AdmFixedParametersCuda params, int scale, int64_t *accum_global,
                       WarpShift ws, const uint32_t shift_inner_accum,
                       const uint32_t add_shift_inner_accum)
{
    const cuda_adm_dwt_band_t *ref = &buf.ref_dwt2;
    const cuda_adm_dwt_band_t *dis = &buf.dis_dwt2;
    uint32_t *i_rfactor = params.i_rfactor;
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    /* FIX_ONE_BY_30 for scale-0: (1/30) * 2^17 = 4369, applied with >>12 shift. */
    const uint16_t FIX_ONE_BY_30_S0 = 4369u;

    int cta_y = (blockDim.y * blockIdx.y + threadIdx.y) * rows_per_thread;
    int cta_x = blockDim.x * blockIdx.x + threadIdx.x;
    int y = start_row + cta_y;
    int x = start_col + cta_x;

    const int total_rows = (3 + rows_per_thread - 1);
    int32_t thr[rows_per_thread] = {0};

    if (y < end_row && x < end_col) {
        /* Reflected x-positions for the 3 columns (matches adm_cm_line_kernel). */
        int pos_x[3] = {x - 1, x, x + 1};
        pos_x[0] = abs(pos_x[0]);
        pos_x[2] = pos_x[2] - max(0, 2 * (x - w) + 1);

#pragma unroll
        for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
            for (int row = 0; row < total_rows; ++row) {
                int pos_y = y - 1 + row;
                pos_y = abs(pos_y);
                pos_y = pos_y - max(0, 2 * (y - h) + 1);

                /* Compute csf_r at each of the 3 column positions for this row. */
                int16_t csf_r0 = inline_s0_csf_r(ref, dis, pos_y * src_stride + pos_x[0], theta,
                                                 i_rfactor, adm_enhn_gain_limit);
                int16_t csf_r1 = inline_s0_csf_r(ref, dis, pos_y * src_stride + pos_x[1], theta,
                                                 i_rfactor, adm_enhn_gain_limit);
                int16_t csf_r2 = inline_s0_csf_r(ref, dis, pos_y * src_stride + pos_x[2], theta,
                                                 i_rfactor, adm_enhn_gain_limit);

                /* flt values: neighbor columns use FIX_ONE_BY_30, center column uses
                 * ONE_BY_15 only for the center row (thread_row == 1). */
                int16_t flt0 = (int16_t)(((FIX_ONE_BY_30_S0 * abs((int32_t)csf_r0)) + 2048) >> 12);
                int16_t flt1_neighbor =
                    (int16_t)(((FIX_ONE_BY_30_S0 * abs((int32_t)csf_r1)) + 2048) >> 12);
                int16_t flt1_center = (int16_t)(((ONE_BY_15 * abs((int32_t)csf_r1)) + 2048) >> 12);
                int16_t flt2 = (int16_t)(((FIX_ONE_BY_30_S0 * abs((int32_t)csf_r2)) + 2048) >> 12);

#pragma unroll
                for (int thread_item = 0; thread_item < rows_per_thread; ++thread_item) {
                    int thread_row = row - thread_item;
                    if (thread_row >= 0 && thread_row < 3) {
                        thr[thread_item] += flt0 + flt2; /* left + right columns always neighbor */
                        if (thread_row != 1) {
                            thr[thread_item] += flt1_neighbor; /* top/bot center: FIX_ONE_BY_30 */
                        } else {
                            thr[thread_item] += flt1_center; /* center pixel: ONE_BY_15 */
                        }
                    }
                }
            }
        }
    }

    int32_t shift_sub_block = shift_sub[blockIdx.z];
    int32_t accum_thread_reg[rows_per_thread] = {0};

    /* Pre-load dis band pointers for a_val computation (mirrors DLM signal path). */
    const int16_t *__restrict__ dh_aim = dis->band_h;
    const int16_t *__restrict__ dv_aim = dis->band_v;
    const int16_t *__restrict__ dd_aim = dis->band_d;

    for (int row = 0; row < rows_per_thread; ++row) {
        /* Signal: rfactor * a_val where a_val = t_val - decouple_r.
         * Use inline_s0_decouple_r + unshifted rfactor multiplication to match the
         * signal scale expected by conclude_adm_cm (constant_offset calibrated for
         * the unshifted product, not the >>15-pre-shifted inline_s0_csf_a result).
         * Bug: the old path called inline_s0_csf_a which applies >>15 before abs(),
         * producing a signal ~32768x smaller than the threshold, yielding near-zero
         * AIM scores. Fix mirrors the DLM kernel pattern (lines 349-355). */
        int32_t aim_signal = 0;
        if ((y + row) < end_row && x < end_col) {
            int aim_idx = (y + row) * src_stride + x;
            int16_t r_val =
                inline_s0_decouple_r(ref, dis, aim_idx, (int)blockIdx.z, adm_enhn_gain_limit);
            /* Select distorted band value for this theta (blockIdx.z = 0:h, 1:v, 2:d). */
            int16_t t_val;
            if (blockIdx.z == 0)
                t_val = __ldg(&dh_aim[aim_idx]);
            else if (blockIdx.z == 1)
                t_val = __ldg(&dv_aim[aim_idx]);
            else
                t_val = __ldg(&dd_aim[aim_idx]);
            int16_t a_val = t_val - r_val;
            aim_signal = abs(int32_t(i_rfactor[blockIdx.z] * (uint32_t)a_val));
        }
        int32_t val = aim_signal - (thr[row] << shift_sub_block);
        accum_thread_reg[row] = max(0, val);
    }

    const int band2 = blockIdx.z;
    int64_t accum = 0;
    int32_t add_shift_cub = ws.add_shift_cub[band2];
    int32_t shift_cub = ws.shift_cub[band2];
    int32_t add_shift_sq = ws.add_shift_sq[band2];
    int32_t shift_sq = ws.shift_sq[band2];

    for (int row = 0; row < rows_per_thread; ++row) {
        int32_t accum_thread_val = accum_thread_reg[row];
        const int32_t x_sq = (int32_t)((
            (((int64_t)accum_thread_val * accum_thread_val) + add_shift_sq) >> shift_sq));
        accum += (((int64_t)x_sq * accum_thread_val) + add_shift_cub) >> shift_cub;
    }

    accum = warp_reduce(accum);

    if (threadIdx.x % 32 == 0) {
        accum = (accum + add_shift_inner_accum) >> shift_inner_accum;
        atomicAdd_int64(&accum_global[band2], accum);
    }
}

#define ADM_CM_AIM_LINE(rows_per_thread)                                                           \
    __global__ void adm_cm_aim_line_kernel_##rows_per_thread(                                      \
        AdmBufferCuda buf, int h, int w, int top, int bottom, int left, int right, int start_row,  \
        int end_row, int start_col, int end_col, int src_stride, int csf_a_stride, int buffer_h,   \
        int buffer_stride, int32_t *accum_per_block, AdmFixedParametersCuda params, int scale,     \
        int64_t *accum_global, WarpShift ws, const uint32_t shift_inner_accum,                     \
        const uint32_t add_shift_inner_accum)                                                      \
    {                                                                                              \
        adm_cm_aim_line_kernel<rows_per_thread>(                                                   \
            buf, h, w, top, bottom, left, right, start_row, end_row, start_col, end_col,           \
            src_stride, csf_a_stride, buffer_h, buffer_stride, accum_per_block, params, scale,     \
            accum_global, ws, shift_inner_accum, add_shift_inner_accum);                           \
    }

extern "C" {
ADM_CM_AIM_LINE(8); /* adm_cm_aim_line_kernel_8 */
}
