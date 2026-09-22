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

/* Device helpers are looked up by nobody but the kernels of this translation
 * unit, so they live in anonymous namespaces (internal linkage). Only the
 * `extern "C"` kernels keep external linkage: the host resolves them by name
 * through cuModuleGetFunction (ADR-0747). */
namespace
{

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
    const int32_t oh = __ldg(&rh[idx]);
    const int32_t ov = __ldg(&rv[idx]);
    const int32_t od = __ldg(&rd[idx]);
    const int32_t th = __ldg(&dh[idx]);
    const int32_t tv = __ldg(&dv[idx]);
    const int32_t td = __ldg(&dd[idx]);

    const int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
    const int32_t r_val =
        decouple_r_s123(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);

    int32_t t_val;
    if (theta == 0) {
        t_val = th;
    } else if (theta == 1) {
        t_val = tv;
    } else {
        t_val = td;
    }

    const int32_t a_val = t_val - r_val;

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
    const int32_t oh = __ldg(&rh[idx]);
    const int32_t ov = __ldg(&rv[idx]);
    const int32_t od = __ldg(&rd[idx]);
    const int32_t th = __ldg(&dh[idx]);
    const int32_t tv = __ldg(&dv[idx]);
    const int32_t td = __ldg(&dd[idx]);

    const int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
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
    const int16_t oh = __ldg(&rh[idx]);
    const int16_t ov = __ldg(&rv[idx]);
    const int16_t od = __ldg(&rd[idx]);
    const int16_t th = __ldg(&dh[idx]);
    const int16_t tv = __ldg(&dv[idx]);
    const int16_t td = __ldg(&dd[idx]);

    const int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    const int16_t r_val =
        decouple_r_s0(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);

    int16_t t_val;
    if (theta == 0) {
        t_val = th;
    } else if (theta == 1) {
        t_val = tv;
    } else {
        t_val = td;
    }

    const int16_t a_val = t_val - r_val;

    const int band = theta + 1; // band index 1..3
    const int32_t dst_val = i_rfactor[theta] * (uint32_t)a_val;
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
    const int16_t oh = __ldg(&rh[idx]);
    const int16_t ov = __ldg(&rv[idx]);
    const int16_t od = __ldg(&rd[idx]);
    const int16_t th = __ldg(&dh[idx]);
    const int16_t tv = __ldg(&dv[idx]);
    const int16_t td = __ldg(&dd[idx]);

    const int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    return decouple_r_s0(oh, ov, od, th, tv, td, band_idx, angle_flag, adm_enhn_gain_limit);
}

/* The two neighbours of `pos` along one axis of the 3x3 CM window of scales
 * 1-3. When the CM region reaches the first sample of the axis, the low
 * neighbour mirrors to pos + 1; when it reaches the last one, the high
 * neighbour clamps to pos. */
struct CmNeighbours {
    int lo;
    int hi;
};

__device__ __forceinline__ CmNeighbours i4_cm_neighbours(int pos, int n, int lo_border,
                                                         int hi_border)
{
    /* Positional, not designated: nvcc's MSVC host frontend rejects C++20
     * designated initializers in device code (error: expected an expression).
     * Order is CmNeighbours{lo, hi}. */
    CmNeighbours nb = {pos - 1, pos + 1};
    if (pos == 0 && lo_border <= 0) {
        nb.lo = pos + 1;
    } else if (pos == (n - 1) && hi_border > (n - 1)) {
        nb.hi = pos;
    }
    return nb;
}

/* Rounding shifts of the cubic CM accumulation of one band:
 * x_sq = (x * x + add_shift_sq) >> shift_sq, then
 * term = (x_sq * x + add_shift_cub) >> shift_cub. */
struct CmCubeShifts {
    int32_t add_shift_sq;
    uint32_t shift_sq;
    int32_t add_shift_cub;
    uint32_t shift_cub;
};

__device__ __forceinline__ int64_t cm_cube(int32_t x, CmCubeShifts s)
{
    const int32_t x_sq = (int32_t)((((int64_t)x * x) + s.add_shift_sq) >> s.shift_sq);
    return (((int64_t)x_sq * x) + s.add_shift_cub) >> s.shift_cub;
}

/* Launch-wide inputs of a scale 1-3 CM kernel. */
struct I4CmParams {
    const cuda_i4_adm_dwt_band_t *ref;
    const cuda_i4_adm_dwt_band_t *dis;
    const uint32_t *rfactor;
    double adm_enhn_gain_limit;
    CmCubeShifts cube;
    uint32_t shift_inner_accum;
    int32_t add_shift_inner_accum;
};

__device__ __forceinline__ I4CmParams i4_cm_params(const AdmBufferCuda &buf,
                                                   const AdmFixedParametersCuda &params, int scale,
                                                   int w, int h)
{
    /* Cubic-accumulation shifts — match adm_cm_reduce_line_kernel for scale != 0. */
    const uint32_t shift_cub = __float2uint_ru(__log2f((float)w));
    const uint32_t shift_inner_accum = __float2uint_ru(__log2f((float)h));
    const int rfactor_base = scale * 3;
    /* Positional, not designated, for the same reason as i4_cm_neighbours above.
     * Field order is I4CmParams{ref, dis, rfactor, adm_enhn_gain_limit, cube,
     * shift_inner_accum, add_shift_inner_accum} and
     * CmCubeShifts{add_shift_sq, shift_sq, add_shift_cub, shift_cub}. */
    return {
        &buf.i4_ref_dwt2,                /* ref */
        &buf.i4_dis_dwt2,                /* dis */
        &params.i_rfactor[rfactor_base], /* rfactor */
        params.adm_enhn_gain_limit,      /* adm_enhn_gain_limit */
        {
            536870912,                        /* cube.add_shift_sq = 1 << 29 */
            30,                               /* cube.shift_sq */
            (int32_t)(1u << (shift_cub - 1)), /* cube.add_shift_cub */
            shift_cub,                        /* cube.shift_cub */
        },
        shift_inner_accum,                        /* shift_inner_accum */
        (int32_t)(1u << (shift_inner_accum - 1)), /* add_shift_inner_accum */
    };
}

/* coeff * |v| in the 2^32 fixed point of the scale 1-3 CM threshold. */
__device__ __forceinline__ int32_t i4_cm_weight(uint32_t coeff, int32_t v)
{
    const uint32_t shift_flt = 32;
    /* 1u << 31 wraps to INT32_MIN, so the rounding term is subtracted rather
     * than added. The CPU reference rounds the same way and the Netflix golden
     * scores encode it (ADR-0155); do not widen it. INT32_MIN names that value
     * directly: the unsigned-to-signed conversion NVCC 13.4 reports as
     * diagnostic #68-D is gone and the emitted bits are unchanged
     * (docs/research/2076-cuda-adm-signbit-warning.md). */
    const int32_t add_bef_shift_flt = INT32_MIN;
    return (int32_t)((((int64_t)coeff * abs(v)) + add_bef_shift_flt) >> shift_flt);
}

/* Reduce the per-thread sums of one scale 1-3 row across the block and add the
 * row's rounded total to the band accumulator. Every thread of the block must
 * call this, because it synchronises the block. */
__device__ __forceinline__ void i4_cm_flush_row(int64_t thread_accum, bool row_in_range,
                                                int64_t *band_accum, const I4CmParams &p)
{
    const int64_t lane_accum = warp_reduce(thread_accum);
    __shared__ int64_t warp_sums[8];
    if ((threadIdx.x % VMAF_CUDA_THREADS_PER_WARP) == 0) {
        warp_sums[threadIdx.x / VMAF_CUDA_THREADS_PER_WARP] = lane_accum;
    }
    __syncthreads();

    if (threadIdx.x == 0 && row_in_range) {
        int64_t row_total = 0;
        for (int w_idx = 0; w_idx < (blockDim.x / VMAF_CUDA_THREADS_PER_WARP); ++w_idx) {
            row_total += warp_sums[w_idx];
        }
        atomicAdd_int64(band_accum, (row_total + p.add_shift_inner_accum) >> p.shift_inner_accum);
    }
}

/* The csf_f rows above, at and below the current scale 1-3 row, per angle. */
struct I4FltRows {
    const int32_t *top[3];
    const int32_t *mid[3];
    const int32_t *bot[3];
};

/* DLM CM threshold of one scale 1-3 pixel: the 3x3 csf_f neighbourhood of
 * every angle, its centre replaced by the pixel's own I4_ONE_BY_15 * |csf_a|. */
__device__ __forceinline__ int32_t i4_dlm_threshold(const I4CmParams &p, const I4FltRows &flt,
                                                    CmNeighbours cols, int j, int idx)
{
    int32_t thr = 0;
    for (int theta = 0; theta < 3; ++theta) {
        int32_t sum = 0;
        /* Inline csf_a at center pixel [i, j] */
        const int32_t csf_a_val =
            inline_i4_csf_a(p.ref, p.dis, idx, theta, p.rfactor, p.adm_enhn_gain_limit);

        sum += flt.top[theta][cols.lo];
        sum += flt.top[theta][j];
        sum += flt.top[theta][cols.hi];

        sum += flt.mid[theta][cols.lo];
        sum += i4_cm_weight(I4_ONE_BY_15, csf_a_val);
        sum += flt.mid[theta][cols.hi];

        sum += flt.bot[theta][cols.lo];
        sum += flt.bot[theta][j];
        sum += flt.bot[theta][cols.hi];

        thr += sum;
    }
    return thr;
}

/* DLM CM contribution of scale 1-3 pixel [i, j] (flat index idx) to the
 * band blockIdx.z. */
__device__ __forceinline__ int64_t i4_dlm_pixel(const I4CmParams &p, const I4FltRows &flt,
                                                CmNeighbours cols, int j, int idx)
{
    const uint32_t shift_dst = 28;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));
    const int32_t shift_sub = 0;

    const int32_t thr = i4_dlm_threshold(p, flt, cols, j, idx);
    /* Inline decouple_r at pixel [i, j] */
    const int32_t r_val =
        inline_i4_decouple_r(p.ref, p.dis, idx, (int)blockIdx.z, p.adm_enhn_gain_limit);
    int32_t x =
        (int32_t)((((int64_t)r_val * p.rfactor[blockIdx.z]) + add_bef_shift_dst) >> shift_dst);
    x = abs(x) - (thr >> shift_sub);
    const int32_t accum_thread = x < 0 ? 0 : x;
    return cm_cube(accum_thread, p.cube);
}

} // namespace

/* Fused compute + warp-reduce + atomicAdd kernel for ADM CM scales 1-3 (i4 path).
 * Eliminates the separate adm_cm_reduce_line_kernel_4 launch and the accum_per_thread
 * scratch buffer round-trip.  Scale 0 uses adm_cm_line_kernel_8 (int16 path);
 * this kernel mirrors its warp-reduce + atomicAdd_int64 pattern for int32. */
extern "C" {
__global__ void i4_adm_cm_line_kernel_fused(AdmBufferCuda buf, int h, int w, int top, int bottom,
                                            int left, int right, int start_row, int end_row,
                                            int start_col, int end_col, int src_stride,
                                            int /* csf_a_stride */, int scale,
                                            int64_t *accum_global, AdmFixedParametersCuda params)
{
    const I4CmParams p = i4_cm_params(buf, params, scale, w, h);
    int32_t *const *flt_angles = buf.i4_csf_f.bands + 1;

    const int i = start_row + (int)blockIdx.y;
    int64_t thread_accum = 0;

    if (i < end_row) {
        const CmNeighbours rows = i4_cm_neighbours(i, h, top, bottom);
        const int top_offset = rows.lo * src_stride;
        const int mid_offset = i * src_stride;
        const int bot_offset = rows.hi * src_stride;
        I4FltRows flt;
        for (int theta = 0; theta < 3; ++theta) {
            flt.top[theta] = flt_angles[theta] + top_offset;
            flt.mid[theta] = flt_angles[theta] + mid_offset;
            flt.bot[theta] = flt_angles[theta] + bot_offset;
        }

        for (int j = start_col + (int)threadIdx.x; j < end_col; j += (int)blockDim.x) {
            thread_accum +=
                i4_dlm_pixel(p, flt, i4_cm_neighbours(j, w, left, right), j, mid_offset + j);
        }
    }

    i4_cm_flush_row(thread_accum, i < end_row, &accum_global[blockIdx.z], p);
}
}
__constant__ const int32_t shift_sub[3] = {10, 10, 12};

namespace
{

// HACK: the 256 byte alignment is required to ensure that the struct is not moved to lmem
struct WarpShift {
    uint32_t shift_cub[3];
    uint32_t add_shift_cub[3];
    uint32_t shift_sq[3];
    uint32_t add_shift_sq[3];
};

/* Launch-wide inputs of a scale-0 CM kernel. */
struct S0CmParams {
    const cuda_adm_dwt_band_t *ref;
    const cuda_adm_dwt_band_t *dis;
    const uint32_t *i_rfactor;
    double adm_enhn_gain_limit;
    int h;
    int src_stride;
};

__device__ __forceinline__ S0CmParams s0_cm_params(const AdmBufferCuda &buf,
                                                   const AdmFixedParametersCuda &params, int h,
                                                   int src_stride)
{
    /* Positional: see i4_cm_neighbours. Field order is
     * S0CmParams{ref, dis, i_rfactor, adm_enhn_gain_limit, h, src_stride}. */
    return {
        &buf.ref_dwt2,              /* ref */
        &buf.dis_dwt2,              /* dis */
        params.i_rfactor,           /* i_rfactor */
        params.adm_enhn_gain_limit, /* adm_enhn_gain_limit */
        h,                          /* h */
        src_stride,                 /* src_stride */
    };
}

/* The host-computed cubic-accumulation shifts of scale-0 band `band`. */
__device__ __forceinline__ CmCubeShifts s0_cm_cube_shifts(const WarpShift &ws, int band)
{
    /* Positional: see i4_cm_neighbours. Field order is
     * CmCubeShifts{add_shift_sq, shift_sq, add_shift_cub, shift_cub}. */
    return {
        (int32_t)ws.add_shift_sq[band],  /* add_shift_sq */
        ws.shift_sq[band],               /* shift_sq */
        (int32_t)ws.add_shift_cub[band], /* add_shift_cub */
        ws.shift_cub[band],              /* shift_cub */
    };
}

/* Row `pos` of a scale-0 band reflected into the band: -1 mirrors to 1, h
 * clamps to h - 1. */
__device__ __forceinline__ int s0_cm_row(int pos, int h)
{
    return min(abs(pos), h - 1);
}

/* Add input row `row` of a column's 3x3 windows to the thresholds of the
 * output rows it borders. A thread owns rows_per_thread consecutive output
 * rows, so input row `row` is the top, centre or bottom row of the windows of
 * outputs row - 2 .. row. The centre sample of a window takes `center`
 * instead of `mid`. */
template <int rows_per_thread>
__device__ __forceinline__ void s0_cm_add_row(int32_t (&thr)[rows_per_thread], int row,
                                              int16_t left, int16_t mid, int16_t right,
                                              int16_t center)
{
#pragma unroll
    for (int thread_item = 0; thread_item < rows_per_thread; ++thread_item) {
        const int thread_row = row - thread_item;
        if (thread_row >= 0 && thread_row < 3) {
            thr[thread_item] += left + right;
            if (thread_row != 1) {
                thr[thread_item] += mid;
            } else {
                thr[thread_item] += center;
            }
        }
    }
}

/* DLM CM thresholds of the rows_per_thread scale-0 pixels of column pos_x[1]
 * from row y: per angle, the 3x3 csf_f neighbourhood with the centre replaced
 * by the pixel's own ONE_BY_15 * |csf_a|. */
template <int rows_per_thread>
__device__ __forceinline__ void s0_dlm_thresholds(const S0CmParams &p, int16_t *const *flt_angles,
                                                  const int (&pos_x)[3], int y,
                                                  int32_t (&thr)[rows_per_thread])
{
    const int total_rows = (3 + rows_per_thread - 1);

#pragma unroll
    for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
        for (int row = 0; row < total_rows; ++row) {
            const int row_offset = s0_cm_row(y - 1 + row, p.h) * p.src_stride;

            /* Inline csf_a at center pixel */
            const int16_t csf_a_val = inline_s0_csf_a(p.ref, p.dis, row_offset + pos_x[1], theta,
                                                      p.i_rfactor, p.adm_enhn_gain_limit);
            const int16_t *flt_ptr = flt_angles[theta] + row_offset;
            const int16_t center = (int16_t)(((ONE_BY_15 * abs((int32_t)csf_a_val)) + 2048) >> 12);
            s0_cm_add_row<rows_per_thread>(thr, row, flt_ptr[pos_x[0]], flt_ptr[pos_x[1]],
                                           flt_ptr[pos_x[2]], center);
        }
    }
}

/* Warp-reduce each output row of a scale-0 thread and add the row's rounded
 * total to the band accumulator. */
template <int rows_per_thread>
__device__ __forceinline__ void s0_cm_flush_rows(const int64_t (&accum_row)[rows_per_thread], int y,
                                                 int end_row, int64_t *band_accum,
                                                 const uint32_t shift_inner_accum,
                                                 const uint32_t add_shift_inner_accum)
{
#pragma unroll
    for (int row = 0; row < rows_per_thread; ++row) {
        const int64_t row_total = warp_reduce(accum_row[row]);
        if (threadIdx.x == 0 && (y + row) < end_row) {
            const int64_t shifted = (row_total + add_shift_inner_accum) >> shift_inner_accum;
            atomicAdd_int64(band_accum, shifted);
        }
    }
}

template <int rows_per_thread>
__device__ __forceinline__ void
adm_cm_line_kernel(const AdmBufferCuda &buf, int h, int w, int start_row, int end_row,
                   int start_col, int end_col, int src_stride, const AdmFixedParametersCuda &params,
                   int64_t *accum_global, const WarpShift &ws, const uint32_t shift_inner_accum,
                   const uint32_t add_shift_inner_accum)
{
    const S0CmParams p = s0_cm_params(buf, params, h, src_stride);
    int16_t *const *flt_angles = buf.csf_f.bands + 1;

    const int cta_y = (blockDim.y * blockIdx.y + threadIdx.y) * rows_per_thread;
    const int y = start_row + cta_y;

    const int band2 = blockIdx.z;
    const CmCubeShifts cube = s0_cm_cube_shifts(ws, band2);
    const int32_t shift_sub_block = shift_sub[blockIdx.z];

    int64_t accum_row[rows_per_thread] = {0};

    for (int x = start_col + (int)threadIdx.x; x < end_col; x += (int)blockDim.x) {
        /* ADR-1210's asymmetric rule, as the CPU's adm_cm_thresh(): x - 1 mirrors
         * to 1 at the left edge, x + 1 clamps to w - 1 at the right edge. Both
         * edges are inside the CM region only for bands of 14 samples or less. */
        const int pos_x[3] = {abs(x - 1), x, min(x + 1, w - 1)};

        int32_t thr[rows_per_thread] = {0};
        s0_dlm_thresholds<rows_per_thread>(p, flt_angles, pos_x, y, thr);

        for (int row = 0; row < rows_per_thread; ++row) {
            int16_t sb = 0;
            if ((y + row) < end_row) {
                /* Inline decouple_r at pixel [y + row, x] */
                sb = inline_s0_decouple_r(p.ref, p.dis, (y + row) * src_stride + x, band2,
                                          p.adm_enhn_gain_limit);
            }
            const int32_t val =
                abs(int32_t(p.i_rfactor[blockIdx.z] * sb)) - (thr[row] << shift_sub_block);
            const int32_t accum_thread = max(0, val);
            accum_row[row] += cm_cube(accum_thread, cube);
        }
    }

    s0_cm_flush_rows<rows_per_thread>(accum_row, y, end_row, &accum_global[band2],
                                      shift_inner_accum, add_shift_inner_accum);
}

} // namespace

/* adm_cm_reduce_line_kernel template and ADM_CM_REDUCE_LINE macro removed.
 * The two-kernel reduce pattern (compute → global scratch, then reduce separately)
 * has been superseded by i4_adm_cm_line_kernel_fused which integrates the
 * warp-reduce + atomicAdd_int64 step directly into the compute kernel.
 * Scale 0 already used the fused pattern (adm_cm_line_kernel_8); scales 1-3
 * were migrated in PR perf/adm-cm-cuda-warp-reduce-fusion.
 *
 * The unnamed parameters are unused by the kernel but stay in the signature:
 * the host launches it with a fixed `void *args[]` layout. */

/* The ADM_CM_LINE(rows_per_thread) generator macro that used to stand here had
 * exactly one expansion, ADM_CM_LINE(8). It is written out instead, for two
 * reasons. The HIP twin (../../hip/integer_adm/adm_cm.hip) spells
 * `adm_cm_line_kernel_8` out, and a three-way diff of the ports is what makes
 * them reviewable. And the HISS-04 scanner reads `ADM_CM_LINE(8);` at file
 * scope as a function signature whose body is the next `{` in the file -- the
 * anonymous namespace below -- and reports that namespace as one 127-line
 * function. The expansion is token-for-token what the macro produced. */

extern "C" {
// 128 = warps_per_thread * val_per_thread = 32 * 4 -- assuming 32 threads per warp, this might change in the future
/* adm_cm_reduce_line_kernel_4 removed: fused into i4_adm_cm_line_kernel_fused (scales 1-3). */
__global__ void adm_cm_line_kernel_8(AdmBufferCuda buf, int h, int w, int /* top */,
                                     int /* bottom */, int /* left */, int /* right */,
                                     int start_row, int end_row, int start_col, int end_col,
                                     int src_stride, int /* csf_a_stride */, int /* buffer_h */,
                                     int /* buffer_stride */, int32_t * /* accum_per_block */,
                                     AdmFixedParametersCuda params, int /* scale */,
                                     int64_t *accum_global, WarpShift ws,
                                     const uint32_t shift_inner_accum,
                                     const uint32_t add_shift_inner_accum)
{
    adm_cm_line_kernel<8>(buf, h, w, start_row, end_row, start_col, end_col, src_stride, params,
                          accum_global, ws, shift_inner_accum, add_shift_inner_accum);
}
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

namespace
{

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
    const int32_t oh = __ldg(&rh[idx]);
    const int32_t ov = __ldg(&rv[idx]);
    const int32_t od = __ldg(&rd[idx]);
    const int32_t th = __ldg(&dh[idx]);
    const int32_t tv = __ldg(&dv[idx]);
    const int32_t td = __ldg(&dd[idx]);

    const int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
    const int32_t r_val =
        decouple_r_s123(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);
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
    const int16_t oh = __ldg(&rh[idx]);
    const int16_t ov = __ldg(&rv[idx]);
    const int16_t od = __ldg(&rd[idx]);
    const int16_t th = __ldg(&dh[idx]);
    const int16_t tv = __ldg(&dv[idx]);
    const int16_t td = __ldg(&dd[idx]);

    const int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    const int16_t r_val =
        decouple_r_s0(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);
    const int band = theta + 1;
    const int32_t dst_val = i_rfactor[theta] * (uint32_t)r_val;
    return (dst_val + i_shiftsadd_cm[band]) >> i_shifts_cm[band];
}

/* AIM CM threshold of one scale 1-3 pixel: sum across theta of the 3×3 csf_r
 * neighbourhood. Each position is computed inline to avoid extra device
 * buffers. Convention matches CPU I4_ADM_CM_THRESH_S_I_J macro:
 *   neighbors → I4_FIX_ONE_BY_30 * |csf_r|
 *   center    → I4_ONE_BY_15     * |csf_r|  (stored as angles[] in CPU)
 * row_offset holds the flat offsets of the rows above, at and below. */
__device__ __forceinline__ int32_t i4_aim_threshold(const I4CmParams &p, const int (&row_offset)[3],
                                                    CmNeighbours cols, int j)
{
    const double egl = p.adm_enhn_gain_limit;
    int32_t thr = 0;
    for (int theta = 0; theta < 3; ++theta) {
        /* Compute csf_r for all 9 positions in the 3×3 window. */
        const int32_t cr_tl =
            inline_i4_csf_r(p.ref, p.dis, row_offset[0] + cols.lo, theta, p.rfactor, egl);
        const int32_t cr_tc =
            inline_i4_csf_r(p.ref, p.dis, row_offset[0] + j, theta, p.rfactor, egl);
        const int32_t cr_tr =
            inline_i4_csf_r(p.ref, p.dis, row_offset[0] + cols.hi, theta, p.rfactor, egl);
        const int32_t cr_ml =
            inline_i4_csf_r(p.ref, p.dis, row_offset[1] + cols.lo, theta, p.rfactor, egl);
        const int32_t cr_mc =
            inline_i4_csf_r(p.ref, p.dis, row_offset[1] + j, theta, p.rfactor, egl);
        const int32_t cr_mr =
            inline_i4_csf_r(p.ref, p.dis, row_offset[1] + cols.hi, theta, p.rfactor, egl);
        const int32_t cr_bl =
            inline_i4_csf_r(p.ref, p.dis, row_offset[2] + cols.lo, theta, p.rfactor, egl);
        const int32_t cr_bc =
            inline_i4_csf_r(p.ref, p.dis, row_offset[2] + j, theta, p.rfactor, egl);
        const int32_t cr_br =
            inline_i4_csf_r(p.ref, p.dis, row_offset[2] + cols.hi, theta, p.rfactor, egl);

        int32_t sum = 0;
        sum += i4_cm_weight(I4_FIX_ONE_BY_30, cr_tl);
        sum += i4_cm_weight(I4_FIX_ONE_BY_30, cr_tc);
        sum += i4_cm_weight(I4_FIX_ONE_BY_30, cr_tr);
        sum += i4_cm_weight(I4_FIX_ONE_BY_30, cr_ml);
        sum += i4_cm_weight(I4_ONE_BY_15, cr_mc); /* center: I4_ONE_BY_15 */
        sum += i4_cm_weight(I4_FIX_ONE_BY_30, cr_mr);
        sum += i4_cm_weight(I4_FIX_ONE_BY_30, cr_bl);
        sum += i4_cm_weight(I4_FIX_ONE_BY_30, cr_bc);
        sum += i4_cm_weight(I4_FIX_ONE_BY_30, cr_br);

        thr += sum;
    }
    return thr;
}

/* AIM CM contribution of scale 1-3 pixel [i, j] to the band blockIdx.z. */
__device__ __forceinline__ int64_t i4_aim_pixel(const I4CmParams &p, const int (&row_offset)[3],
                                                CmNeighbours cols, int j)
{
    const int32_t shift_sub = 0;

    const int32_t thr = i4_aim_threshold(p, row_offset, cols, j);
    /* Signal: rfactor * a_val = CSF of decouple_a. */
    int32_t x = inline_i4_csf_a(p.ref, p.dis, row_offset[1] + j, (int)blockIdx.z, p.rfactor,
                                p.adm_enhn_gain_limit);
    x = abs(x) - (thr >> shift_sub);
    const int32_t accum_thread = x < 0 ? 0 : x;
    return cm_cube(accum_thread, p.cube);
}

} // namespace

extern "C" {

/* AIM CM fused kernel for scales 1-3 (i4 path).
 * Signal/threshold roles are swapped vs i4_adm_cm_line_kernel_fused.
 * Signal = rfactor * a_val; threshold = csf_r 3×3 neighbourhood (fully inline). */
__global__ void i4_adm_cm_aim_line_kernel_fused(AdmBufferCuda buf, int h, int w, int top,
                                                int bottom, int left, int right, int start_row,
                                                int end_row, int start_col, int end_col,
                                                int src_stride, int /* csf_a_stride */, int scale,
                                                int64_t *accum_global,
                                                AdmFixedParametersCuda params)
{
    const I4CmParams p = i4_cm_params(buf, params, scale, w, h);

    const int i = start_row + (int)blockIdx.y;
    int64_t thread_accum = 0;

    if (i < end_row) {
        const CmNeighbours rows = i4_cm_neighbours(i, h, top, bottom);
        const int row_offset[3] = {rows.lo * src_stride, i * src_stride, rows.hi * src_stride};

        for (int j = start_col + (int)threadIdx.x; j < end_col; j += (int)blockDim.x) {
            thread_accum += i4_aim_pixel(p, row_offset, i4_cm_neighbours(j, w, left, right), j);
        }
    }

    i4_cm_flush_row(thread_accum, i < end_row, &accum_global[blockIdx.z], p);
}

} /* extern "C" */

namespace
{

/* AIM signal of scale-0 pixel idx in the band blockIdx.z: |rfactor * a_val|,
 * the CSF of decouple_a. */
__device__ __forceinline__ int32_t s0_aim_signal(const S0CmParams &p, int idx)
{
    /* Pre-load dis band pointers for a_val computation (mirrors DLM signal path). */
    const int16_t *__restrict__ dh_aim = p.dis->band_h;
    const int16_t *__restrict__ dv_aim = p.dis->band_v;
    const int16_t *__restrict__ dd_aim = p.dis->band_d;

    const int16_t r_val =
        inline_s0_decouple_r(p.ref, p.dis, idx, (int)blockIdx.z, p.adm_enhn_gain_limit);
    /* Select distorted band value for this theta (blockIdx.z = 0:h, 1:v, 2:d). */
    int16_t t_val;
    if (blockIdx.z == 0) {
        t_val = __ldg(&dh_aim[idx]);
    } else if (blockIdx.z == 1) {
        t_val = __ldg(&dv_aim[idx]);
    } else {
        t_val = __ldg(&dd_aim[idx]);
    }
    const int16_t a_val = t_val - r_val;
    return abs(int32_t(p.i_rfactor[blockIdx.z] * (uint32_t)a_val));
}

/* AIM CM thresholds of the rows_per_thread scale-0 pixels of column pos_x[1]
 * from row y: per angle, the 3x3 csf_r neighbourhood. */
template <int rows_per_thread>
__device__ __forceinline__ void s0_aim_thresholds(const S0CmParams &p, const int (&pos_x)[3], int y,
                                                  int32_t (&thr)[rows_per_thread])
{
    /* FIX_ONE_BY_30 for scale-0: (1/30) * 2^17 = 4369, applied with >>12 shift. */
    const uint16_t FIX_ONE_BY_30_S0 = 4369u;
    const int total_rows = (3 + rows_per_thread - 1);
    const double egl = p.adm_enhn_gain_limit;

#pragma unroll
    for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
        for (int row = 0; row < total_rows; ++row) {
            const int row_offset = s0_cm_row(y - 1 + row, p.h) * p.src_stride;

            /* Compute csf_r at each of the 3 column positions for this row. */
            const int16_t csf_r0 =
                inline_s0_csf_r(p.ref, p.dis, row_offset + pos_x[0], theta, p.i_rfactor, egl);
            const int16_t csf_r1 =
                inline_s0_csf_r(p.ref, p.dis, row_offset + pos_x[1], theta, p.i_rfactor, egl);
            const int16_t csf_r2 =
                inline_s0_csf_r(p.ref, p.dis, row_offset + pos_x[2], theta, p.i_rfactor, egl);

            /* flt values: neighbor columns use FIX_ONE_BY_30, center column uses
             * ONE_BY_15 only for the center row (thread_row == 1). */
            const int16_t flt0 =
                (int16_t)(((FIX_ONE_BY_30_S0 * abs((int32_t)csf_r0)) + 2048) >> 12);
            const int16_t flt1_neighbor =
                (int16_t)(((FIX_ONE_BY_30_S0 * abs((int32_t)csf_r1)) + 2048) >> 12);
            const int16_t flt1_center =
                (int16_t)(((ONE_BY_15 * abs((int32_t)csf_r1)) + 2048) >> 12);
            const int16_t flt2 =
                (int16_t)(((FIX_ONE_BY_30_S0 * abs((int32_t)csf_r2)) + 2048) >> 12);

            s0_cm_add_row<rows_per_thread>(thr, row, flt0, flt1_neighbor, flt2, flt1_center);
        }
    }
}

/* AIM CM device function for scale 0 (int16 path).
 * Mirrors adm_cm_line_kernel<rows_per_thread> with signal/threshold swapped.
 * Signal = inline_s0_csf_a; threshold = inline_s0_csf_r 3×3 neighbourhood. */
template <int rows_per_thread>
__device__ __forceinline__ void adm_cm_aim_line_kernel(
    const AdmBufferCuda &buf, int h, int w, int start_row, int end_row, int start_col, int end_col,
    int src_stride, const AdmFixedParametersCuda &params, int64_t *accum_global,
    const WarpShift &ws, const uint32_t shift_inner_accum, const uint32_t add_shift_inner_accum)
{
    const S0CmParams p = s0_cm_params(buf, params, h, src_stride);

    const int cta_y = (blockDim.y * blockIdx.y + threadIdx.y) * rows_per_thread;
    const int y = start_row + cta_y;

    const int band2 = blockIdx.z;
    const CmCubeShifts cube = s0_cm_cube_shifts(ws, band2);
    const int32_t shift_sub_block = shift_sub[blockIdx.z];

    int64_t accum_row[rows_per_thread] = {0};

    for (int x = start_col + (int)threadIdx.x; x < end_col; x += (int)blockDim.x) {
        /* Reflected x-positions for the 3 columns (matches adm_cm_line_kernel).
         * ADR-1210's asymmetric rule, as the CPU's adm_cm_thresh(): x - 1 mirrors
         * to 1 at the left edge, x + 1 clamps to w - 1 at the right edge. Both
         * edges are inside the CM region only for bands of 14 samples or less. */
        const int pos_x[3] = {abs(x - 1), x, min(x + 1, w - 1)};

        int32_t thr[rows_per_thread] = {0};
        s0_aim_thresholds<rows_per_thread>(p, pos_x, y, thr);

        for (int row = 0; row < rows_per_thread; ++row) {
            int32_t aim_signal = 0;
            if ((y + row) < end_row) {
                aim_signal = s0_aim_signal(p, (y + row) * src_stride + x);
            }
            const int32_t val = aim_signal - (thr[row] << shift_sub_block);
            const int32_t accum_thread_val = max(0, val);
            accum_row[row] += cm_cube(accum_thread_val, cube);
        }
    }

    s0_cm_flush_rows<rows_per_thread>(accum_row, y, end_row, &accum_global[band2],
                                      shift_inner_accum, add_shift_inner_accum);
}

} // namespace

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
 * See ADR-1226 for the full sweep before re-adding it.
 *
 * The unnamed parameters are unused by the kernel but stay in the signature:
 * the host launches it with a fixed `void *args[]` layout. */
#define ADM_CM_AIM_LINE(rows_per_thread)                                                           \
    __global__ void adm_cm_aim_line_kernel_##rows_per_thread(                                      \
        AdmBufferCuda buf, int h, int w, int /* top */, int /* bottom */, int /* left */,          \
        int /* right */, int start_row, int end_row, int start_col, int end_col, int src_stride,   \
        int /* csf_a_stride */, int /* buffer_h */, int /* buffer_stride */,                       \
        int32_t * /* accum_per_block */, AdmFixedParametersCuda params, int /* scale */,           \
        int64_t *accum_global, WarpShift ws, const uint32_t shift_inner_accum,                     \
        const uint32_t add_shift_inner_accum)                                                      \
    {                                                                                              \
        adm_cm_aim_line_kernel<rows_per_thread>(buf, h, w, start_row, end_row, start_col, end_col, \
                                                src_stride, params, accum_global, ws,              \
                                                shift_inner_accum, add_shift_inner_accum);         \
    }

/* Two instantiations, picked at launch by SM occupancy (see
 * `integer_adm_cuda.c::adm_cm_aim_rows_per_thread`). The kernel emits one block per
 * `BLOCKY * rows_per_thread` rows of a `buffer_h`-row band, times three
 * orientation bands, and nothing else parallelises it — so `rows_per_thread`
 * is what decides whether the launch fills the GPU. At the previous fixed
 * value of 8, a 1080p frame produced 42 blocks total against an RTX 4090's
 * 128 SMs: two thirds of the device idle by construction. See ADR-1226. */
extern "C" {
ADM_CM_AIM_LINE(2); /* adm_cm_aim_line_kernel_2 */
ADM_CM_AIM_LINE(4); /* adm_cm_aim_line_kernel_4 */
}
