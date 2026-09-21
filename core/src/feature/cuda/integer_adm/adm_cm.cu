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
#include <cstdint>

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

struct I4AdmCmContext {
    const cuda_i4_adm_dwt_band_t *ref;
    const cuda_i4_adm_dwt_band_t *dis;
    int32_t *const *flt_angles;
    const uint32_t *rfactor;
    int h;
    int w;
    int top;
    int bottom;
    int left;
    int right;
    int src_stride;
    int band;
    double gain_limit;
    uint32_t shift_flt;
    int32_t add_bef_shift_flt;
    uint32_t shift_dst;
    int32_t add_bef_shift_dst;
    uint32_t shift_sq;
    int32_t add_shift_sq;
    uint32_t shift_cub;
    int32_t add_shift_cub;
    int32_t shift_sub;
};

__device__ __forceinline__ int32_t i4_adm_cm_threshold(const I4AdmCmContext *ctx, int i, int j)
{
    int16_t row_offset[2] = {-1, 1};
    int16_t col_offset[2] = {-1, 1};
    if (i == 0 && ctx->top <= 0)
        row_offset[0] = 1;
    else if (i == ctx->h - 1 && ctx->bottom > ctx->h - 1)
        row_offset[1] = 0;
    if (j == 0 && ctx->left <= 0)
        col_offset[0] = 1;
    else if (j == ctx->w - 1 && ctx->right > ctx->w - 1)
        col_offset[1] = 0;

    const int row_top = i + row_offset[0];
    const int row_bottom = i + row_offset[1];
    const int col_left = j + col_offset[0];
    const int col_right = j + col_offset[1];
    int32_t threshold = 0;
    for (int theta = 0; theta < 3; ++theta) {
        const int32_t *flt_top = ctx->flt_angles[theta] + row_top * ctx->src_stride;
        const int32_t *flt_mid = ctx->flt_angles[theta] + i * ctx->src_stride;
        const int32_t *flt_bottom = ctx->flt_angles[theta] + row_bottom * ctx->src_stride;
        const int32_t csf_a = inline_i4_csf_a(ctx->ref, ctx->dis, i * ctx->src_stride + j, theta,
                                              ctx->rfactor, ctx->gain_limit);
        int32_t sum = 0;
        sum += flt_top[col_left];
        sum += flt_top[j];
        sum += flt_top[col_right];
        sum += flt_mid[col_left];
        sum += (int32_t)((((int64_t)I4_ONE_BY_15 * abs(csf_a)) + ctx->add_bef_shift_flt) >>
                         ctx->shift_flt);
        sum += flt_mid[col_right];
        sum += flt_bottom[col_left];
        sum += flt_bottom[j];
        sum += flt_bottom[col_right];
        threshold += sum;
    }
    return threshold;
}

__device__ __forceinline__ int64_t i4_adm_cm_pixel(const I4AdmCmContext *ctx, int i, int j)
{
    const int32_t threshold = i4_adm_cm_threshold(ctx, i, j);
    const int32_t r_val = inline_i4_decouple_r(ctx->ref, ctx->dis, i * ctx->src_stride + j,
                                               ctx->band - 1, ctx->gain_limit);
    int32_t x = (int32_t)((((int64_t)r_val * ctx->rfactor[blockIdx.z]) + ctx->add_bef_shift_dst) >>
                          ctx->shift_dst);
    x = abs(x) - (threshold >> ctx->shift_sub);
    const int32_t accum = x < 0 ? 0 : x;
    const int32_t squared =
        (int32_t)(((int64_t)accum * accum + ctx->add_shift_sq) >> ctx->shift_sq);
    return (((int64_t)squared * accum) + ctx->add_shift_cub) >> ctx->shift_cub;
}

__device__ __forceinline__ int64_t i4_adm_cm_thread_accum(const I4AdmCmContext *ctx, int i,
                                                          int start_col, int end_col)
{
    int64_t accum = 0;
    for (int j = start_col + (int)threadIdx.x; j < end_col; j += (int)blockDim.x)
        accum += i4_adm_cm_pixel(ctx, i, j);
    return accum;
}

__device__ __forceinline__ I4AdmCmContext make_i4_adm_context(AdmBufferCuda *buf, int h, int w,
                                                              int top, int bottom, int left,
                                                              int right, int src_stride, int scale,
                                                              const AdmFixedParametersCuda *params)
{
    const uint32_t shift_flt = 32;
    /* ADR-0155: preserve Netflix's negative signed rounding bias directly. */
    const int32_t add_bef_shift_flt = INT32_MIN;
    const uint32_t shift_dst = 28;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));
    const uint32_t shift_sq = 30;
    const int32_t add_shift_sq = 536870912;
    const uint32_t shift_cub = __float2uint_ru(__log2f((float)w));
    const int32_t add_shift_cub = (int32_t)(1u << (shift_cub - 1));
    return I4AdmCmContext{&buf->i4_ref_dwt2,
                          &buf->i4_dis_dwt2,
                          buf->i4_csf_f.bands + 1,
                          &params->i_rfactor[scale * 3],
                          h,
                          w,
                          top,
                          bottom,
                          left,
                          right,
                          src_stride,
                          (int)blockIdx.z + 1,
                          params->adm_enhn_gain_limit,
                          shift_flt,
                          add_bef_shift_flt,
                          shift_dst,
                          add_bef_shift_dst,
                          shift_sq,
                          add_shift_sq,
                          shift_cub,
                          add_shift_cub,
                          0};
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
    (void)csf_a_stride;
    const I4AdmCmContext ctx =
        make_i4_adm_context(&buf, h, w, top, bottom, left, right, src_stride, scale, &params);
    const uint32_t shift_inner_accum = __float2uint_ru(__log2f((float)h));
    const int32_t add_shift_inner_accum = (int32_t)(1u << (shift_inner_accum - 1));
    const int i = start_row + (int)blockIdx.y;
    const int64_t thread_accum =
        i < end_row ? i4_adm_cm_thread_accum(&ctx, i, start_col, end_col) : 0;

    const int64_t lane_accum = warp_reduce(thread_accum);
    __shared__ int64_t warp_sums[8];
    if ((threadIdx.x % VMAF_CUDA_THREADS_PER_WARP) == 0)
        warp_sums[threadIdx.x / VMAF_CUDA_THREADS_PER_WARP] = lane_accum;
    __syncthreads();

    if (threadIdx.x == 0 && i < end_row) {
        int64_t row_total = 0;
        for (int warp = 0; warp < (blockDim.x / VMAF_CUDA_THREADS_PER_WARP); ++warp)
            row_total += warp_sums[warp];
        atomicAdd_int64(&accum_global[blockIdx.z],
                        (row_total + add_shift_inner_accum) >> shift_inner_accum);
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

struct S0AdmCmContext {
    const cuda_adm_dwt_band_t *ref;
    const cuda_adm_dwt_band_t *dis;
    const int16_t *dist_h;
    const int16_t *dist_v;
    const int16_t *dist_d;
    int16_t *const *flt_angles;
    uint32_t *rfactor;
    double gain_limit;
    int y;
    int end_row;
    int h;
    int w;
    int src_stride;
    int band;
    int band_index;
    int32_t add_shift_cub;
    int32_t shift_cub;
    int32_t add_shift_sq;
    int32_t shift_sq;
    int32_t threshold_shift;
};

template <int rows_per_thread>
__device__ __forceinline__ S0AdmCmContext make_s0_adm_cm_context(AdmBufferCuda *buf, int h, int w,
                                                                 int start_row, int end_row,
                                                                 int src_stride,
                                                                 AdmFixedParametersCuda *params,
                                                                 const WarpShift *ws)
{
    const int band_index = (int)blockIdx.z;
    const int cta_y = ((int)blockDim.y * (int)blockIdx.y + (int)threadIdx.y) * rows_per_thread;
    return S0AdmCmContext{&buf->ref_dwt2,
                          &buf->dis_dwt2,
                          buf->dis_dwt2.band_h,
                          buf->dis_dwt2.band_v,
                          buf->dis_dwt2.band_d,
                          buf->csf_f.bands + 1,
                          params->i_rfactor,
                          params->adm_enhn_gain_limit,
                          start_row + cta_y,
                          end_row,
                          h,
                          w,
                          src_stride,
                          band_index + 1,
                          band_index,
                          (int32_t)ws->add_shift_cub[band_index],
                          (int32_t)ws->shift_cub[band_index],
                          (int32_t)ws->add_shift_sq[band_index],
                          (int32_t)ws->shift_sq[band_index],
                          shift_sub[band_index]};
}

template <int rows_per_thread>
__device__ __forceinline__ void s0_adm_cm_thresholds(const S0AdmCmContext *ctx, int x,
                                                     int32_t *thresholds)
{
    int pos_x[3] = {x - 1, x, x + 1};
    pos_x[0] = abs(pos_x[0]);
    pos_x[2] -= max(0, 2 * (x - ctx->w) + 1);
    const int total_rows = 3 + rows_per_thread - 1;
#pragma unroll
    for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
        for (int row = 0; row < total_rows; ++row) {
            int pos_y = ctx->y - 1 + row;
            pos_y = abs(pos_y);
            pos_y -= max(0, 2 * (ctx->y - ctx->h) + 1);
            const int16_t csf_a = inline_s0_csf_a(ctx->ref, ctx->dis, pos_y * ctx->src_stride + x,
                                                  theta, ctx->rfactor, ctx->gain_limit);
            const int16_t *flt = ctx->flt_angles[theta] + pos_y * ctx->src_stride;
            const int16_t flt_row[3] = {flt[pos_x[0]], flt[pos_x[1]], flt[pos_x[2]]};
#pragma unroll
            for (int item = 0; item < rows_per_thread; ++item) {
                const int thread_row = row - item;
                if (thread_row >= 0 && thread_row < 3) {
                    thresholds[item] += flt_row[0] + flt_row[2];
                    thresholds[item] +=
                        thread_row != 1 ?
                            flt_row[1] :
                            (int16_t)(((ONE_BY_15 * abs((int32_t)csf_a)) + 2048) >> 12);
                }
            }
        }
    }
}

template <int rows_per_thread>
__device__ __forceinline__ void s0_adm_cm_accumulate_x(const S0AdmCmContext *ctx, int x,
                                                       int64_t *accum_rows)
{
    int32_t thresholds[rows_per_thread] = {0};
    s0_adm_cm_thresholds<rows_per_thread>(ctx, x, thresholds);
    for (int row = 0; row < rows_per_thread; ++row) {
        int16_t signal = 0;
        if (ctx->y + row < ctx->end_row) {
            signal = inline_s0_decouple_r(ctx->ref, ctx->dis, (ctx->y + row) * ctx->src_stride + x,
                                          ctx->band - 1, ctx->gain_limit);
        }
        const int32_t value = abs(int32_t(ctx->rfactor[blockIdx.z] * signal)) -
                              (thresholds[row] << ctx->threshold_shift);
        const int32_t accum = max(0, value);
        const int32_t squared =
            (int32_t)((((int64_t)accum * accum) + ctx->add_shift_sq) >> ctx->shift_sq);
        accum_rows[row] += (((int64_t)squared * accum) + ctx->add_shift_cub) >> ctx->shift_cub;
    }
}

template <int rows_per_thread>
__device__ __forceinline__ void
adm_cm_line_kernel(AdmBufferCuda buf, int h, int w, int top, int bottom, int left, int right,
                   int start_row, int end_row, int start_col, int end_col, int src_stride,
                   int csf_a_stride, int buffer_h, int buffer_stride, int32_t *accum_per_block,
                   AdmFixedParametersCuda params, int scale, int64_t *accum_global, WarpShift ws,
                   const uint32_t shift_inner_accum, const uint32_t add_shift_inner_accum)
{
    (void)top;
    (void)bottom;
    (void)left;
    (void)right;
    (void)csf_a_stride;
    (void)buffer_h;
    (void)buffer_stride;
    (void)accum_per_block;
    (void)scale;
    const S0AdmCmContext ctx = make_s0_adm_cm_context<rows_per_thread>(
        &buf, h, w, start_row, end_row, src_stride, &params, &ws);
    int64_t accum_rows[rows_per_thread] = {0};
    for (int x = start_col + (int)threadIdx.x; x < end_col; x += (int)blockDim.x)
        s0_adm_cm_accumulate_x<rows_per_thread>(&ctx, x, accum_rows);

#pragma unroll
    for (int row = 0; row < rows_per_thread; ++row) {
        const int64_t row_total = warp_reduce(accum_rows[row]);
        if (threadIdx.x == 0 && ctx.y + row < end_row) {
            const int64_t shifted = (row_total + add_shift_inner_accum) >> shift_inner_accum;
            atomicAdd_int64(&accum_global[ctx.band_index], shifted);
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
    extern "C" __global__ void adm_cm_line_kernel_##rows_per_thread(                               \
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

// 128 = warps_per_thread * val_per_thread = 32 * 4 -- assuming 32 threads per warp, this might change in the future
/* adm_cm_reduce_line_kernel_4 removed: fused into i4_adm_cm_line_kernel_fused (scales 1-3). */
ADM_CM_LINE(8); // adm_cm_line_kernel_8

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

struct I4AdmWindow {
    int row_top;
    int row_bottom;
    int col_left;
    int col_right;
};

__device__ __forceinline__ I4AdmWindow make_i4_adm_window(const I4AdmCmContext *ctx, int i, int j)
{
    int16_t row_offset[2] = {-1, 1};
    int16_t col_offset[2] = {-1, 1};
    if (i == 0 && ctx->top <= 0)
        row_offset[0] = 1;
    else if (i == ctx->h - 1 && ctx->bottom > ctx->h - 1)
        row_offset[1] = 0;
    if (j == 0 && ctx->left <= 0)
        col_offset[0] = 1;
    else if (j == ctx->w - 1 && ctx->right > ctx->w - 1)
        col_offset[1] = 0;
    return I4AdmWindow{i + row_offset[0], i + row_offset[1], j + col_offset[0], j + col_offset[1]};
}

__device__ __forceinline__ int32_t i4_adm_aim_weight(const I4AdmCmContext *ctx, int32_t value,
                                                     uint32_t coefficient)
{
    return (int32_t)((((int64_t)coefficient * abs(value)) + ctx->add_bef_shift_flt) >>
                     ctx->shift_flt);
}

__device__ __forceinline__ int32_t i4_adm_aim_neighbor_sum(const I4AdmCmContext *ctx,
                                                           const I4AdmWindow *window, int i, int j,
                                                           int theta)
{
    const int stride = ctx->src_stride;
    const int32_t top_left =
        inline_i4_csf_r(ctx->ref, ctx->dis, window->row_top * stride + window->col_left, theta,
                        ctx->rfactor, ctx->gain_limit);
    const int32_t top_center = inline_i4_csf_r(ctx->ref, ctx->dis, window->row_top * stride + j,
                                               theta, ctx->rfactor, ctx->gain_limit);
    const int32_t top_right =
        inline_i4_csf_r(ctx->ref, ctx->dis, window->row_top * stride + window->col_right, theta,
                        ctx->rfactor, ctx->gain_limit);
    const int32_t middle_left = inline_i4_csf_r(ctx->ref, ctx->dis, i * stride + window->col_left,
                                                theta, ctx->rfactor, ctx->gain_limit);
    const int32_t middle_center =
        inline_i4_csf_r(ctx->ref, ctx->dis, i * stride + j, theta, ctx->rfactor, ctx->gain_limit);
    const int32_t middle_right = inline_i4_csf_r(ctx->ref, ctx->dis, i * stride + window->col_right,
                                                 theta, ctx->rfactor, ctx->gain_limit);
    const int32_t bottom_left =
        inline_i4_csf_r(ctx->ref, ctx->dis, window->row_bottom * stride + window->col_left, theta,
                        ctx->rfactor, ctx->gain_limit);
    const int32_t bottom_center = inline_i4_csf_r(
        ctx->ref, ctx->dis, window->row_bottom * stride + j, theta, ctx->rfactor, ctx->gain_limit);
    const int32_t bottom_right =
        inline_i4_csf_r(ctx->ref, ctx->dis, window->row_bottom * stride + window->col_right, theta,
                        ctx->rfactor, ctx->gain_limit);
    int32_t sum = i4_adm_aim_weight(ctx, top_left, I4_FIX_ONE_BY_30);
    sum += i4_adm_aim_weight(ctx, top_center, I4_FIX_ONE_BY_30);
    sum += i4_adm_aim_weight(ctx, top_right, I4_FIX_ONE_BY_30);
    sum += i4_adm_aim_weight(ctx, middle_left, I4_FIX_ONE_BY_30);
    sum += i4_adm_aim_weight(ctx, middle_center, I4_ONE_BY_15);
    sum += i4_adm_aim_weight(ctx, middle_right, I4_FIX_ONE_BY_30);
    sum += i4_adm_aim_weight(ctx, bottom_left, I4_FIX_ONE_BY_30);
    sum += i4_adm_aim_weight(ctx, bottom_center, I4_FIX_ONE_BY_30);
    sum += i4_adm_aim_weight(ctx, bottom_right, I4_FIX_ONE_BY_30);
    return sum;
}

__device__ __forceinline__ int32_t i4_adm_aim_threshold(const I4AdmCmContext *ctx, int i, int j)
{
    const I4AdmWindow window = make_i4_adm_window(ctx, i, j);
    int32_t threshold = 0;
    for (int theta = 0; theta < 3; ++theta)
        threshold += i4_adm_aim_neighbor_sum(ctx, &window, i, j, theta);
    return threshold;
}

__device__ __forceinline__ int64_t i4_adm_aim_pixel(const I4AdmCmContext *ctx, int i, int j)
{
    const int32_t threshold = i4_adm_aim_threshold(ctx, i, j);
    int32_t signal = inline_i4_csf_a(ctx->ref, ctx->dis, i * ctx->src_stride + j, (int)blockIdx.z,
                                     ctx->rfactor, ctx->gain_limit);
    signal = abs(signal) - (threshold >> ctx->shift_sub);
    const int32_t accum = signal < 0 ? 0 : signal;
    const int32_t squared =
        (int32_t)(((int64_t)accum * accum + ctx->add_shift_sq) >> ctx->shift_sq);
    return (((int64_t)squared * accum) + ctx->add_shift_cub) >> ctx->shift_cub;
}

__device__ __forceinline__ int64_t i4_adm_aim_thread_accum(const I4AdmCmContext *ctx, int i,
                                                           int start_col, int end_col)
{
    int64_t accum = 0;
    for (int j = start_col + (int)threadIdx.x; j < end_col; j += (int)blockDim.x)
        accum += i4_adm_aim_pixel(ctx, i, j);
    return accum;
}

/* AIM CM fused kernel for scales 1-3 (i4 path).
 * Signal is rfactor * a_val; threshold is the inline csf_r neighborhood. */
extern "C" __global__ void
i4_adm_cm_aim_line_kernel_fused(AdmBufferCuda buf, int h, int w, int top, int bottom, int left,
                                int right, int start_row, int end_row, int start_col, int end_col,
                                int src_stride, int csf_a_stride, int scale, int64_t *accum_global,
                                AdmFixedParametersCuda params)
{
    (void)csf_a_stride;
    const I4AdmCmContext ctx =
        make_i4_adm_context(&buf, h, w, top, bottom, left, right, src_stride, scale, &params);
    const uint32_t shift_inner_accum = __float2uint_ru(__log2f((float)h));
    const int32_t add_shift_inner_accum = (int32_t)(1u << (shift_inner_accum - 1));
    const int i = start_row + (int)blockIdx.y;
    const int64_t thread_accum =
        i < end_row ? i4_adm_aim_thread_accum(&ctx, i, start_col, end_col) : 0;

    const int64_t lane_accum = warp_reduce(thread_accum);
    __shared__ int64_t warp_sums[8];
    if ((threadIdx.x % VMAF_CUDA_THREADS_PER_WARP) == 0)
        warp_sums[threadIdx.x / VMAF_CUDA_THREADS_PER_WARP] = lane_accum;
    __syncthreads();

    if (threadIdx.x == 0 && i < end_row) {
        int64_t row_total = 0;
        for (int warp = 0; warp < (blockDim.x / VMAF_CUDA_THREADS_PER_WARP); ++warp)
            row_total += warp_sums[warp];
        atomicAdd_int64(&accum_global[blockIdx.z],
                        (row_total + add_shift_inner_accum) >> shift_inner_accum);
    }
}

template <int rows_per_thread>
__device__ __forceinline__ void s0_adm_aim_thresholds(const S0AdmCmContext *ctx, int x,
                                                      int32_t *thresholds)
{
    const uint16_t fix_one_by_30 = 4369u;
    int pos_x[3] = {x - 1, x, x + 1};
    pos_x[0] = abs(pos_x[0]);
    pos_x[2] -= max(0, 2 * (x - ctx->w) + 1);
    const int total_rows = 3 + rows_per_thread - 1;
#pragma unroll
    for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
        for (int row = 0; row < total_rows; ++row) {
            int pos_y = ctx->y - 1 + row;
            pos_y = abs(pos_y);
            pos_y -= max(0, 2 * (ctx->y - ctx->h) + 1);
            const int16_t csf_r0 =
                inline_s0_csf_r(ctx->ref, ctx->dis, pos_y * ctx->src_stride + pos_x[0], theta,
                                ctx->rfactor, ctx->gain_limit);
            const int16_t csf_r1 =
                inline_s0_csf_r(ctx->ref, ctx->dis, pos_y * ctx->src_stride + pos_x[1], theta,
                                ctx->rfactor, ctx->gain_limit);
            const int16_t csf_r2 =
                inline_s0_csf_r(ctx->ref, ctx->dis, pos_y * ctx->src_stride + pos_x[2], theta,
                                ctx->rfactor, ctx->gain_limit);
            const int16_t flt0 = (int16_t)(((fix_one_by_30 * abs((int32_t)csf_r0)) + 2048) >> 12);
            const int16_t flt1_neighbor =
                (int16_t)(((fix_one_by_30 * abs((int32_t)csf_r1)) + 2048) >> 12);
            const int16_t flt1_center =
                (int16_t)(((ONE_BY_15 * abs((int32_t)csf_r1)) + 2048) >> 12);
            const int16_t flt2 = (int16_t)(((fix_one_by_30 * abs((int32_t)csf_r2)) + 2048) >> 12);
#pragma unroll
            for (int item = 0; item < rows_per_thread; ++item) {
                const int thread_row = row - item;
                if (thread_row >= 0 && thread_row < 3) {
                    thresholds[item] += flt0 + flt2;
                    thresholds[item] += thread_row != 1 ? flt1_neighbor : flt1_center;
                }
            }
        }
    }
}

__device__ __forceinline__ int16_t s0_adm_aim_distorted_value(const S0AdmCmContext *ctx, int index)
{
    if (blockIdx.z == 0)
        return __ldg(&ctx->dist_h[index]);
    if (blockIdx.z == 1)
        return __ldg(&ctx->dist_v[index]);
    return __ldg(&ctx->dist_d[index]);
}

template <int rows_per_thread>
__device__ __forceinline__ void s0_adm_aim_accumulate_x(const S0AdmCmContext *ctx, int x,
                                                        int64_t *accum_rows)
{
    int32_t thresholds[rows_per_thread] = {0};
    s0_adm_aim_thresholds<rows_per_thread>(ctx, x, thresholds);
    for (int row = 0; row < rows_per_thread; ++row) {
        int32_t signal = 0;
        if (ctx->y + row < ctx->end_row) {
            const int index = (ctx->y + row) * ctx->src_stride + x;
            const int16_t r_val =
                inline_s0_decouple_r(ctx->ref, ctx->dis, index, (int)blockIdx.z, ctx->gain_limit);
            const int16_t a_val = s0_adm_aim_distorted_value(ctx, index) - r_val;
            signal = abs(int32_t(ctx->rfactor[blockIdx.z] * (uint32_t)a_val));
        }
        const int32_t value = signal - (thresholds[row] << ctx->threshold_shift);
        const int32_t accum = max(0, value);
        const int32_t squared =
            (int32_t)((((int64_t)accum * accum) + ctx->add_shift_sq) >> ctx->shift_sq);
        accum_rows[row] += (((int64_t)squared * accum) + ctx->add_shift_cub) >> ctx->shift_cub;
    }
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
    (void)top;
    (void)bottom;
    (void)left;
    (void)right;
    (void)csf_a_stride;
    (void)buffer_h;
    (void)buffer_stride;
    (void)accum_per_block;
    (void)scale;
    const S0AdmCmContext ctx = make_s0_adm_cm_context<rows_per_thread>(
        &buf, h, w, start_row, end_row, src_stride, &params, &ws);
    int64_t accum_rows[rows_per_thread] = {0};
    for (int x = start_col + (int)threadIdx.x; x < end_col; x += (int)blockDim.x)
        s0_adm_aim_accumulate_x<rows_per_thread>(&ctx, x, accum_rows);

#pragma unroll
    for (int row = 0; row < rows_per_thread; ++row) {
        const int64_t row_total = warp_reduce(accum_rows[row]);
        if (threadIdx.x == 0 && ctx.y + row < end_row) {
            const int64_t shifted = (row_total + add_shift_inner_accum) >> shift_inner_accum;
            atomicAdd_int64(&accum_global[ctx.band_index], shifted);
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
    extern "C" __global__ void adm_cm_aim_line_kernel_##rows_per_thread(                           \
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
ADM_CM_AIM_LINE(2); /* adm_cm_aim_line_kernel_2 */
ADM_CM_AIM_LINE(4); /* adm_cm_aim_line_kernel_4 */
