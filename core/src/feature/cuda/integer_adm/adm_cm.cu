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

struct I4CmRows {
    const int32_t *top[3];
    const int32_t *middle[3];
    const int32_t *bottom[3];
};

__device__ __forceinline__ I4CmRows i4_cm_rows(int32_t *const *angles, int row_top, int row_middle,
                                               int row_bottom, int stride)
{
    I4CmRows rows;
#pragma unroll
    for (int theta = 0; theta < 3; ++theta) {
        rows.top[theta] = angles[theta] + row_top * stride;
        rows.middle[theta] = angles[theta] + row_middle * stride;
        rows.bottom[theta] = angles[theta] + row_bottom * stride;
    }
    return rows;
}

__device__ __forceinline__ int32_t i4_dlm_threshold(const cuda_i4_adm_dwt_band_t *ref,
                                                    const cuda_i4_adm_dwt_band_t *dis,
                                                    const I4CmRows &rows, int row, int column,
                                                    int column_left, int column_right, int stride,
                                                    const uint32_t *rfactor, double gain_limit,
                                                    int32_t add_before_shift)
{
    int32_t threshold = 0;
    for (int theta = 0; theta < 3; ++theta) {
        const int32_t csf_a =
            inline_i4_csf_a(ref, dis, row * stride + column, theta, rfactor, gain_limit);
        int32_t sum = rows.top[theta][column_left];
        sum += rows.top[theta][column];
        sum += rows.top[theta][column_right];
        sum += rows.middle[theta][column_left];
        sum += (int32_t)((((int64_t)I4_ONE_BY_15 * abs(csf_a)) + add_before_shift) >> 32);
        sum += rows.middle[theta][column_right];
        sum += rows.bottom[theta][column_left];
        sum += rows.bottom[theta][column];
        sum += rows.bottom[theta][column_right];
        threshold += sum;
    }
    return threshold;
}

__device__ __forceinline__ int64_t cubic_cm_term(int32_t value, int32_t add_square,
                                                 uint32_t shift_square, int32_t add_cube,
                                                 uint32_t shift_cube)
{
    const int32_t square = (int32_t)(((int64_t)value * value + add_square) >> shift_square);
    return (((int64_t)square * value) + add_cube) >> shift_cube;
}

__device__ __forceinline__ void reduce_i4_cm_row(int64_t thread_accum, int64_t *warp_sums, int row,
                                                 int end_row, int64_t *accum_global,
                                                 int32_t add_shift, uint32_t shift)
{
    const int64_t lane_accum = warp_reduce(thread_accum);
    if ((threadIdx.x % VMAF_CUDA_THREADS_PER_WARP) == 0)
        warp_sums[threadIdx.x / VMAF_CUDA_THREADS_PER_WARP] = lane_accum;
    __syncthreads();
    if (threadIdx.x != 0 || row >= end_row)
        return;
    int64_t total = 0;
    for (int index = 0; index < blockDim.x / VMAF_CUDA_THREADS_PER_WARP; ++index)
        total += warp_sums[index];
    atomicAdd_int64(&accum_global[blockIdx.z], (total + add_shift) >> shift);
}

__device__ __forceinline__ int64_t i4_dlm_row_accum(
    const cuda_i4_adm_dwt_band_t *ref, const cuda_i4_adm_dwt_band_t *dis, const I4CmRows &rows,
    int row, int width, int left, int right, int start_col, int end_col, int stride, int band,
    const uint32_t *rfactor, double gain_limit, int32_t add_filter, int32_t add_weight,
    int32_t add_square, uint32_t shift_square, int32_t add_cube, uint32_t shift_cube)
{
    int64_t total = 0;
    for (int column = start_col + (int)threadIdx.x; column < end_col; column += (int)blockDim.x) {
        int column_left = column - 1;
        int column_right = column + 1;
        if (column == 0 && left <= 0)
            column_left = 1;
        else if (column == width - 1 && right > width - 1)
            column_right = column;
        const int32_t threshold =
            i4_dlm_threshold(ref, dis, rows, row, column, column_left, column_right, stride,
                             rfactor, gain_limit, add_filter);
        const int32_t remodulated =
            inline_i4_decouple_r(ref, dis, row * stride + column, band, gain_limit);
        int32_t signal = (int32_t)((((int64_t)remodulated * rfactor[band]) + add_weight) >> 28);
        signal = abs(signal) - threshold;
        const int32_t masked = signal < 0 ? 0 : signal;
        total += cubic_cm_term(masked, add_square, shift_square, add_cube, shift_cube);
    }
    return total;
}

/* Fused compute + warp-reduce + atomicAdd kernel for ADM CM scales 1-3 (i4 path).
 * Eliminates the separate adm_cm_reduce_line_kernel_4 launch and the accum_per_thread
 * scratch buffer round-trip.  Scale 0 uses adm_cm_line_kernel_8 (int16 path);
 * this kernel mirrors its warp-reduce + atomicAdd_int64 pattern for int32. */
extern "C" __global__ void
i4_adm_cm_line_kernel_fused(AdmBufferCuda buf, int h, int w, int top, int bottom, int left,
                            int right, int start_row, int end_row, int start_col, int end_col,
                            int src_stride, int csf_a_stride, int scale, int64_t *accum_global,
                            AdmFixedParametersCuda params)
{
    const cuda_i4_adm_dwt_band_t *ref = &buf.i4_ref_dwt2;
    const cuda_i4_adm_dwt_band_t *dis = &buf.i4_dis_dwt2;
    int32_t *const *flt_angles = buf.i4_csf_f.bands + 1;
    const int band = (int)blockIdx.z;
    const uint32_t *rfactor = &params.i_rfactor[scale * 3];
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;
    const int32_t add_bef_shift_flt = (int32_t)(1u << 31);
    const int32_t add_bef_shift_dst = (1u << 27);
    const int32_t add_shift_sq = 536870912; /* 1 << 29 */
    const uint32_t shift_cub = __float2uint_ru(__log2f((float)w));
    const int32_t add_shift_cub = (int32_t)(1u << (shift_cub - 1));
    const uint32_t shift_inner_accum = __float2uint_ru(__log2f((float)h));
    const int32_t add_shift_inner_accum = (int32_t)(1u << (shift_inner_accum - 1));
    const int row = start_row + (int)blockIdx.y;
    int64_t thread_accum = 0;
    if (row < end_row) {
        int row_top = row - 1;
        int row_bottom = row + 1;
        if (row == 0 && top <= 0)
            row_top = 1;
        else if (row == h - 1 && bottom > h - 1)
            row_bottom = row;
        const I4CmRows rows = i4_cm_rows(flt_angles, row_top, row, row_bottom, src_stride);
        thread_accum =
            i4_dlm_row_accum(ref, dis, rows, row, w, left, right, start_col, end_col, src_stride,
                             band, rfactor, adm_enhn_gain_limit, add_bef_shift_flt,
                             add_bef_shift_dst, add_shift_sq, 30, add_shift_cub, shift_cub);
    }
    __shared__ int64_t warp_sums[8];
    reduce_i4_cm_row(thread_accum, warp_sums, row, end_row, accum_global, add_shift_inner_accum,
                     shift_inner_accum);
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
s0_dlm_thresholds(int32_t (&threshold)[rows_per_thread], const cuda_adm_dwt_band_t *ref,
                  const cuda_adm_dwt_band_t *dis, int16_t *const *angles, int y, int x, int width,
                  int height, int stride, uint32_t *rfactor, double gain_limit)
{
    int positions_x[3] = {x - 1, x, x + 1};
    positions_x[0] = abs(positions_x[0]);
    positions_x[2] -= max(0, 2 * (x - width) + 1);
    constexpr int total_rows = 3 + rows_per_thread - 1;
#pragma unroll
    for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
        for (int row = 0; row < total_rows; ++row) {
            int position_y = abs(y - 1 + row);
            position_y -= max(0, 2 * (y - height) + 1);
            const int16_t csf_a =
                inline_s0_csf_a(ref, dis, position_y * stride + x, theta, rfactor, gain_limit);
            int16_t *filtered = angles[theta] + position_y * stride;
            const int16_t values[3] = {filtered[positions_x[0]], filtered[positions_x[1]],
                                       filtered[positions_x[2]]};
#pragma unroll
            for (int item = 0; item < rows_per_thread; ++item) {
                const int relative_row = row - item;
                if (relative_row < 0 || relative_row >= 3)
                    continue;
                threshold[item] += values[0] + values[2];
                threshold[item] += relative_row != 1 ?
                                       values[1] :
                                       (int16_t)(((ONE_BY_15 * abs((int32_t)csf_a)) + 2048) >> 12);
            }
        }
    }
}

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
    int y = start_row + cta_y;

    const int band2 = blockIdx.z;

    int32_t add_shift_cub = ws.add_shift_cub[band2];
    int32_t shift_cub = ws.shift_cub[band2];
    int32_t add_shift_sq = ws.add_shift_sq[band2];
    int32_t shift_sq = ws.shift_sq[band2];
    int32_t shift_sub_block = shift_sub[blockIdx.z];

    int64_t accum_row[rows_per_thread] = {0};

    for (int x = start_col + (int)threadIdx.x; x < end_col; x += (int)blockDim.x) {
        int32_t thr[rows_per_thread] = {0};
        s0_dlm_thresholds(thr, ref, dis, flt_angles, y, x, w, h, src_stride, i_rfactor,
                          adm_enhn_gain_limit);

        for (int row = 0; row < rows_per_thread; ++row) {
            int16_t sb = 0;
            if ((y + row) < end_row) {
                /* Inline decouple_r at pixel [y + row, x] */
                sb = inline_s0_decouple_r(ref, dis, (y + row) * src_stride + x, band - 1,
                                          adm_enhn_gain_limit);
            }
            int32_t val = abs(int32_t(i_rfactor[blockIdx.z] * sb)) - (thr[row] << shift_sub_block);
            int32_t accum_thread = max(0, val);
            accum_row[row] +=
                cubic_cm_term(accum_thread, add_shift_sq, shift_sq, add_shift_cub, shift_cub);
        }
    }

#pragma unroll
    for (int row = 0; row < rows_per_thread; ++row) {
        int64_t row_total = warp_reduce(accum_row[row]);
        if (threadIdx.x == 0 && (y + row) < end_row) {
            int64_t shifted = (row_total + add_shift_inner_accum) >> shift_inner_accum;
            atomicAdd_int64(&accum_global[band2], shifted);
        }
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

__device__ __forceinline__ int32_t i4_aim_threshold(const cuda_i4_adm_dwt_band_t *ref,
                                                    const cuda_i4_adm_dwt_band_t *dis, int row_top,
                                                    int row_middle, int row_bottom, int column_left,
                                                    int column, int column_right, int stride,
                                                    const uint32_t *rfactor, double gain_limit,
                                                    int32_t add_before_shift)
{
    const int rows[3] = {row_top, row_middle, row_bottom};
    const int columns[3] = {column_left, column, column_right};
    int32_t threshold = 0;
    for (int theta = 0; theta < 3; ++theta) {
        int32_t sum = 0;
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                const int32_t csf_r = inline_i4_csf_r(ref, dis, rows[y] * stride + columns[x],
                                                      theta, rfactor, gain_limit);
                const int64_t coefficient =
                    (x == 1 && y == 1) ? (int64_t)I4_ONE_BY_15 : (int64_t)I4_FIX_ONE_BY_30;
                sum += (int32_t)((coefficient * abs(csf_r) + add_before_shift) >> 32);
            }
        }
        threshold += sum;
    }
    return threshold;
}

__device__ __forceinline__ int64_t i4_aim_row_accum(
    const cuda_i4_adm_dwt_band_t *ref, const cuda_i4_adm_dwt_band_t *dis, int row, int row_top,
    int row_bottom, int width, int left, int right, int start_col, int end_col, int stride,
    const uint32_t *rfactor, double gain_limit, int32_t add_filter, int32_t add_square,
    uint32_t shift_square, int32_t add_cube, uint32_t shift_cube)
{
    int64_t total = 0;
    for (int column = start_col + (int)threadIdx.x; column < end_col; column += (int)blockDim.x) {
        int column_left = column - 1;
        int column_right = column + 1;
        if (column == 0 && left <= 0)
            column_left = 1;
        else if (column == width - 1 && right > width - 1)
            column_right = column;
        const int32_t threshold =
            i4_aim_threshold(ref, dis, row_top, row, row_bottom, column_left, column, column_right,
                             stride, rfactor, gain_limit, add_filter);
        int32_t signal =
            inline_i4_csf_a(ref, dis, row * stride + column, (int)blockIdx.z, rfactor, gain_limit);
        signal = abs(signal) - threshold;
        const int32_t masked = signal < 0 ? 0 : signal;
        total += cubic_cm_term(masked, add_square, shift_square, add_cube, shift_cube);
    }
    return total;
}

/* AIM CM fused kernel for scales 1-3 (i4 path).
 * Signal/threshold roles are swapped vs i4_adm_cm_line_kernel_fused.
 * Signal = rfactor * a_val; threshold = csf_r 3×3 neighbourhood (fully inline). */
extern "C" __global__ void
i4_adm_cm_aim_line_kernel_fused(AdmBufferCuda buf, int h, int w, int top, int bottom, int left,
                                int right, int start_row, int end_row, int start_col, int end_col,
                                int src_stride, int csf_a_stride, int scale, int64_t *accum_global,
                                AdmFixedParametersCuda params)
{
    const cuda_i4_adm_dwt_band_t *ref = &buf.i4_ref_dwt2;
    const cuda_i4_adm_dwt_band_t *dis = &buf.i4_dis_dwt2;
    const uint32_t *rfactor = &params.i_rfactor[scale * 3];
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;
    const int32_t add_bef_shift_flt = (int32_t)(1u << 31);
    const int32_t add_shift_sq = 536870912; /* 1 << 29 */
    const uint32_t shift_cub = __float2uint_ru(__log2f((float)w));
    const int32_t add_shift_cub = (int32_t)(1u << (shift_cub - 1));
    const uint32_t shift_inner_accum = __float2uint_ru(__log2f((float)h));
    const int32_t add_shift_inner_accum = (int32_t)(1u << (shift_inner_accum - 1));
    const int row = start_row + (int)blockIdx.y;
    int64_t thread_accum = 0;
    if (row < end_row) {
        int row_top = row - 1;
        int row_bottom = row + 1;
        if (row == 0 && top <= 0)
            row_top = 1;
        else if (row == h - 1 && bottom > h - 1)
            row_bottom = row;
        thread_accum =
            i4_aim_row_accum(ref, dis, row, row_top, row_bottom, w, left, right, start_col, end_col,
                             src_stride, rfactor, adm_enhn_gain_limit, add_bef_shift_flt,
                             add_shift_sq, 30, add_shift_cub, shift_cub);
    }
    __shared__ int64_t warp_sums[8];
    reduce_i4_cm_row(thread_accum, warp_sums, row, end_row, accum_global, add_shift_inner_accum,
                     shift_inner_accum);
}

template <int rows_per_thread>
__device__ __forceinline__ void
s0_aim_thresholds(int32_t (&threshold)[rows_per_thread], const cuda_adm_dwt_band_t *ref,
                  const cuda_adm_dwt_band_t *dis, int y, int x, int width, int height, int stride,
                  uint32_t *rfactor, double gain_limit)
{
    int positions_x[3] = {x - 1, x, x + 1};
    positions_x[0] = abs(positions_x[0]);
    positions_x[2] -= max(0, 2 * (x - width) + 1);
    constexpr int total_rows = 3 + rows_per_thread - 1;
#pragma unroll
    for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
        for (int row = 0; row < total_rows; ++row) {
            int position_y = abs(y - 1 + row);
            position_y -= max(0, 2 * (y - height) + 1);
            int16_t csf_values[3];
            int16_t filtered[3];
#pragma unroll
            for (int column = 0; column < 3; ++column) {
                csf_values[column] =
                    inline_s0_csf_r(ref, dis, position_y * stride + positions_x[column], theta,
                                    rfactor, gain_limit);
                filtered[column] =
                    (int16_t)(((4369u * abs((int32_t)csf_values[column])) + 2048) >> 12);
            }
            const int16_t center_weighted =
                (int16_t)(((ONE_BY_15 * abs((int32_t)csf_values[1])) + 2048) >> 12);
#pragma unroll
            for (int item = 0; item < rows_per_thread; ++item) {
                const int relative_row = row - item;
                if (relative_row < 0 || relative_row >= 3)
                    continue;
                threshold[item] += filtered[0] + filtered[2];
                threshold[item] += relative_row == 1 ? center_weighted : filtered[1];
            }
        }
    }
}

__device__ __forceinline__ int32_t s0_aim_signal(const cuda_adm_dwt_band_t *ref,
                                                 const cuda_adm_dwt_band_t *dis, int index,
                                                 int band, uint32_t *rfactor, double gain_limit)
{
    const int16_t remodulated = inline_s0_decouple_r(ref, dis, index, band, gain_limit);
    const int16_t *__restrict__ distorted = band == 0 ? dis->band_h :
                                            band == 1 ? dis->band_v :
                                                        dis->band_d;
    const int16_t anomaly = __ldg(&distorted[index]) - remodulated;
    return abs(int32_t(rfactor[band] * (uint32_t)anomaly));
}

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
    int cta_y = (blockDim.y * blockIdx.y + threadIdx.y) * rows_per_thread;
    int y = start_row + cta_y;
    const int band2 = blockIdx.z;

    int32_t add_shift_cub = ws.add_shift_cub[band2];
    int32_t shift_cub = ws.shift_cub[band2];
    int32_t add_shift_sq = ws.add_shift_sq[band2];
    int32_t shift_sq = ws.shift_sq[band2];
    int32_t shift_sub_block = shift_sub[blockIdx.z];
    int64_t accum_row[rows_per_thread] = {0};

    for (int x = start_col + (int)threadIdx.x; x < end_col; x += (int)blockDim.x) {
        int32_t thr[rows_per_thread] = {0};
        s0_aim_thresholds(thr, ref, dis, y, x, w, h, src_stride, i_rfactor, adm_enhn_gain_limit);

        for (int row = 0; row < rows_per_thread; ++row) {
            int32_t aim_signal = 0;
            if ((y + row) < end_row)
                aim_signal = s0_aim_signal(ref, dis, (y + row) * src_stride + x, band2, i_rfactor,
                                           adm_enhn_gain_limit);
            int32_t val = aim_signal - (thr[row] << shift_sub_block);
            int32_t accum_thread_val = max(0, val);
            accum_row[row] +=
                cubic_cm_term(accum_thread_val, add_shift_sq, shift_sq, add_shift_cub, shift_cub);
        }
    }

#pragma unroll
    for (int row = 0; row < rows_per_thread; ++row) {
        int64_t row_total = warp_reduce(accum_row[row]);
        if (threadIdx.x == 0 && (y + row) < end_row) {
            int64_t shifted = (row_total + add_shift_inner_accum) >> shift_inner_accum;
            atomicAdd_int64(&accum_global[band2], shifted);
        }
    }
}

/* The AIM CM kernel is the fork's most register-hungry CUDA kernel: three
 * theta planes x ten reflected rows of `inline_s0_csf_r` results, live across
 * an int64 accumulator per row. At `rows_per_thread = 8` ptxas takes all 255
 * registers and still spills 412/404 bytes.
 *
 * `__launch_bounds__` is deliberately absent. It was measured: `(128, 5)` caps
 * the allocator at 96 registers and — counter-intuitively — spills *less*
 * (8 B stack vs 344 B), lifting theoretical occupancy from 16.7% to 41.7% on
 * sm_89. It bought nothing (0.808 ms vs 0.803 ms per call), because this
 * kernel is not occupancy-limited but grid-limited, and stacking it on top of
 * the `rows_per_thread` fix below made things slower (0.572 ms vs 0.553 ms).
 * See ADR-1226 for the full sweep before re-adding it. */
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

/* Two instantiations, picked at launch by SM occupancy (see
 * `integer_adm_cuda.c::adm_cm_aim_line`). The kernel emits one block per
 * `BLOCKY * rows_per_thread` rows of a `buffer_h`-row band, times three
 * orientation bands, and nothing else parallelises it — so `rows_per_thread`
 * is what decides whether the launch fills the GPU. At the previous fixed
 * value of 8, a 1080p frame produced 42 blocks total against an RTX 4090's
 * 128 SMs: two thirds of the device idle by construction. See ADR-1226. */
extern "C" {
ADM_CM_AIM_LINE(2); /* adm_cm_aim_line_kernel_2 */
ADM_CM_AIM_LINE(4); /* adm_cm_aim_line_kernel_4 */
}
