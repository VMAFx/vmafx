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
 *  HIP port of cuda/integer_adm/adm_cm.cu — Contrast Masking (CM) compute
 *  and reduce kernels.
 *
 *  Algorithmic correspondence to the CUDA twin (ADR-0539):
 *
 *    1. Three kernels exported (matches `integer_adm_hip.c::GET_FN` calls):
 *         - `i4_adm_cm_line_kernel`          — scales 1-3 compute (i4 path),
 *                                              writes per-thread scratch.
 *         - `adm_cm_line_kernel_8`           — scale 0 compute (int16 path),
 *                                              fused warp-reduce-equivalent
 *                                              atomicAdd into accum_global.
 *         - `adm_cm_reduce_line_kernel_4`    — reduce kernel that consumes
 *                                              i4_adm_cm_line_kernel's scratch
 *                                              and accumulates into the
 *                                              per-band int64 accumulator.
 *
 *    2. The CUDA twin uses `cuda_helper.cuh::warp_reduce` (warpSize=32
 *       `__shfl_down_sync`) + per-warp `atomicAdd_int64`. That helper
 *       doesn't compile under hipcc and AMD wavefronts are 64 wide so the
 *       shuffle pattern isn't portable. This port does a per-thread
 *       atomicAdd into the int64 accumulator. Bit-exact since int64
 *       addition is associative + commutative — only the order changes.
 *       Same approach as `vif_statistics.hip` (ADR-0537) and
 *       `adm_csf_den.hip` (ADR-0539 sibling kernel).
 *
 *    3. `i4_adm_cm_line_kernel` writes a per-thread int32 scratch
 *       (`accum_per_thread`), which `adm_cm_reduce_line_kernel_4` then
 *       reduces. Preserves the two-kernel pattern the host TU launches.
 *       (The CUDA upstream perf branch fuses the two; the HIP host TU
 *       hasn't been migrated yet so we keep both kernels alive here.)
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include "hip/integer_adm_hip.h"
#include "common.h"

/* Inline decouple helpers (shared with adm_csf.hip). */
#include "adm_decouple_inline.hip"

/* AMD-portable alias for the CUDA uint64_cu atomicAdd target type. */
typedef unsigned long long int uint64_cu;

/* Per-band shifts for the int16 compute kernel (matches CUDA twin). */
__device__ __constant__ const int32_t cm_shift_sub_const[3] = {10, 10, 12};
__device__ __constant__ const int cm_fixed_shift_const[3]   = {4, 4, 3};
__device__ __constant__ const int32_t cm_shift_xsq_const[3] = {29, 29, 30};
__device__ __constant__ const int32_t cm_add_shift_xsq_const[3] = {268435456, 268435456, 536870912};

/* WarpShift mirrors the CUDA `WarpShift` struct — same layout so the host
 * `hipModuleLaunchKernel` argument blob stays byte-for-byte compatible. */
struct WarpShift {
    uint32_t shift_cub[3];
    uint32_t add_shift_cub[3];
    uint32_t shift_sq[3];
    uint32_t add_shift_sq[3];
};

/* ----------------------------------------------------------------------
 * Inline helpers — i4 (scales 1-3) and s0 (scale 0). Mirror the CUDA
 * twin's `inline_*` helpers line-for-line, with cuda_*_band_t renamed to
 * hip_*_band_t. Required by both compute kernels.
 * -------------------------------------------------------------------- */

__device__ __forceinline__ static int32_t
inline_i4_csf_a(const hip_i4_adm_dwt_band_t *ref, const hip_i4_adm_dwt_band_t *dis, int idx,
                int theta, const uint32_t *rfactor, double adm_enhn_gain_limit)
{
    const uint32_t shift_dst       = 28;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));

    int32_t oh = ref->band_h[idx];
    int32_t ov = ref->band_v[idx];
    int32_t od = ref->band_d[idx];
    int32_t th = dis->band_h[idx];
    int32_t tv = dis->band_v[idx];
    int32_t td = dis->band_d[idx];

    int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
    int32_t r_val =
        decouple_r_s123(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);

    int32_t t_val;
    if (theta == 0)
        t_val = th;
    else if (theta == 1)
        t_val = tv;
    else
        t_val = td;

    int32_t a_val = t_val - r_val;
    return (int32_t)(((rfactor[theta] * int64_t(a_val)) + add_bef_shift_dst) >> shift_dst);
}

__device__ __forceinline__ static int32_t
inline_i4_decouple_r(const hip_i4_adm_dwt_band_t *ref, const hip_i4_adm_dwt_band_t *dis, int idx,
                     int band_idx, double adm_enhn_gain_limit)
{
    int32_t oh = ref->band_h[idx];
    int32_t ov = ref->band_v[idx];
    int32_t od = ref->band_d[idx];
    int32_t th = dis->band_h[idx];
    int32_t tv = dis->band_v[idx];
    int32_t td = dis->band_d[idx];

    int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
    return decouple_r_s123(oh, ov, od, th, tv, td, band_idx, angle_flag, adm_enhn_gain_limit);
}

__device__ __forceinline__ static int16_t
inline_s0_csf_a(const hip_adm_dwt_band_t *ref, const hip_adm_dwt_band_t *dis, int idx, int theta,
                const uint32_t *i_rfactor, double adm_enhn_gain_limit)
{
    /* Match CUDA twin's per-band scale-0 shifts. */
    const uint8_t  i_shifts_cm[4]    = {0, 15, 15, 17};
    const uint16_t i_shiftsadd_cm[4] = {0, 16384, 16384, 65535};

    int16_t oh = ref->band_h[idx];
    int16_t ov = ref->band_v[idx];
    int16_t od = ref->band_d[idx];
    int16_t th = dis->band_h[idx];
    int16_t tv = dis->band_v[idx];
    int16_t td = dis->band_d[idx];

    int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    int16_t r_val =
        decouple_r_s0(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);

    int16_t t_val;
    if (theta == 0)
        t_val = th;
    else if (theta == 1)
        t_val = tv;
    else
        t_val = td;

    int16_t a_val = t_val - r_val;
    int band = theta + 1;
    int32_t dst_val = i_rfactor[theta] * (uint32_t)a_val;
    return (int16_t)((dst_val + i_shiftsadd_cm[band]) >> i_shifts_cm[band]);
}

__device__ __forceinline__ static int16_t
inline_s0_decouple_r(const hip_adm_dwt_band_t *ref, const hip_adm_dwt_band_t *dis, int idx,
                     int band_idx, double adm_enhn_gain_limit)
{
    int16_t oh = ref->band_h[idx];
    int16_t ov = ref->band_v[idx];
    int16_t od = ref->band_d[idx];
    int16_t th = dis->band_h[idx];
    int16_t tv = dis->band_v[idx];
    int16_t td = dis->band_d[idx];

    int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    return decouple_r_s0(oh, ov, od, th, tv, td, band_idx, angle_flag, adm_enhn_gain_limit);
}

extern "C" {

/* ----------------------------------------------------------------------
 * i4_adm_cm_line_kernel — scales 1-3 compute. Writes accum_per_thread
 * scratch consumed by adm_cm_reduce_line_kernel_4. Same launch shape as
 * the CUDA twin (host TU controls grid dims).
 *
 * Pure __global__; no template helpers required.
 * -------------------------------------------------------------------- */
__global__ void i4_adm_cm_line_kernel(AdmBufferHip buf, int h, int w, int top, int bottom,
                                      int left, int right, int start_row, int end_row,
                                      int start_col, int end_col, int src_stride, int csf_a_stride,
                                      int scale, int buffer_h, int buffer_stride,
                                      int32_t *accum_per_thread, AdmFixedParametersHip params)
{
    (void)csf_a_stride;

    const hip_i4_adm_dwt_band_t *ref   = &buf.i4_ref_dwt2;
    const hip_i4_adm_dwt_band_t *dis   = &buf.i4_dis_dwt2;
    const hip_i4_adm_dwt_band_t *csf_f = &buf.i4_csf_f;
    const int band = blockIdx.z + 1;
    int32_t *const *flt_angles = csf_f->bands + 1;

    const uint32_t *rfactor          = &params.i_rfactor[scale * 3];
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    const uint32_t shift_flt        = 32;
    const int32_t add_bef_shift_flt = (1u << (shift_flt - 1));
    const uint32_t shift_dst        = 28;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));

    const int32_t shift_sub_local = 0;

    int i = start_row + (int)blockIdx.y;
    int j = start_col + (int)(blockIdx.x * blockDim.x + threadIdx.x);

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

        int row_top = i + offset_i[0];
        int row_bot = i + offset_i[1];
        int col_l = j + offset_j[0];
        int col_r = j + offset_j[1];

        int32_t thr = 0;
        for (int theta = 0; theta < 3; ++theta) {
            int32_t sum = 0;
            int32_t csf_a_val = inline_i4_csf_a(ref, dis,
                                                i * src_stride + j, theta,
                                                rfactor, adm_enhn_gain_limit);

            const int32_t *flt_top = flt_angles[theta] + row_top * src_stride;
            const int32_t *flt_mid = flt_angles[theta] + i * src_stride;
            const int32_t *flt_bot = flt_angles[theta] + row_bot * src_stride;

            sum += flt_top[col_l];
            sum += flt_top[j];
            sum += flt_top[col_r];

            sum += flt_mid[col_l];
            sum += (int32_t)((((int64_t)I4_ONE_BY_15 * abs(csf_a_val)) + add_bef_shift_flt) >>
                             shift_flt);
            sum += flt_mid[col_r];

            sum += flt_bot[col_l];
            sum += flt_bot[j];
            sum += flt_bot[col_r];

            thr += sum;
        }
        int32_t r_val =
            inline_i4_decouple_r(ref, dis, i * src_stride + j, band - 1, adm_enhn_gain_limit);
        int32_t x =
            (int32_t)((((int64_t)r_val * rfactor[blockIdx.z]) + add_bef_shift_dst) >> shift_dst);
        x = abs(x) - (thr >> shift_sub_local);
        accum_thread = x < 0 ? 0 : x;
    }

    int tid_x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (tid_x < buffer_stride)
        accum_per_thread[(blockIdx.z * buffer_h + blockIdx.y) * buffer_stride + tid_x] =
            accum_thread;
}

} /* extern "C" — template helpers below need C++ linkage */

/* ----------------------------------------------------------------------
 * adm_cm_line_kernel_8 — scale-0 compute. Fused warp-equivalent atomicAdd
 * pattern: every thread atomicAdd-s its lane contribution into accum_global
 * (instead of CUDA's per-warp shuffle reduce + per-warp atomicAdd).
 * -------------------------------------------------------------------- */

template <int rows_per_thread>
__device__ __forceinline__ static void
adm_cm_line_kernel_body(AdmBufferHip buf, int h, int w, int top, int bottom, int left, int right,
                        int start_row, int end_row, int start_col, int end_col, int src_stride,
                        int csf_a_stride, int buffer_h, int buffer_stride,
                        int32_t *accum_per_block, AdmFixedParametersHip params, int scale,
                        int64_t *accum_global, WarpShift ws, const uint32_t shift_inner_accum,
                        const uint32_t add_shift_inner_accum)
{
    (void)csf_a_stride;
    (void)buffer_h;
    (void)buffer_stride;
    (void)accum_per_block;
    (void)scale;

    const hip_adm_dwt_band_t *ref   = &buf.ref_dwt2;
    const hip_adm_dwt_band_t *dis   = &buf.dis_dwt2;
    const hip_adm_dwt_band_t *csf_f = &buf.csf_f;
    const int band = blockIdx.z + 1;
    int16_t *const *flt_angles = csf_f->bands + 1;

    uint32_t *i_rfactor              = params.i_rfactor;
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    int cta_y = (int)((blockDim.y * blockIdx.y + threadIdx.y) * rows_per_thread);
    int y     = start_row + cta_y;

    const int total_rows = (3 + rows_per_thread - 1);
    const int band2 = blockIdx.z;

    /* Load WarpShift fields into locals (matches CUDA twin). */
    int32_t add_shift_cub = ws.add_shift_cub[band2];
    int32_t shift_cub     = ws.shift_cub[band2];
    int32_t add_shift_sq  = ws.add_shift_sq[band2];
    int32_t shift_sq      = ws.shift_sq[band2];
    int32_t shift_sub_block = cm_shift_sub_const[blockIdx.z];

    int64_t accum_row[rows_per_thread] = {0};

    for (int x = start_col + (int)threadIdx.x; x < end_col; x += (int)blockDim.x) {
        int pos_x[3] = {x - 1, x, x + 1};
        pos_x[0]     = abs(pos_x[0]);
        pos_x[2]     = pos_x[2] - max(0, 2 * (x - w) + 1);

        int32_t thr[rows_per_thread] = {0};

#pragma unroll
        for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
            for (int row = 0; row < total_rows; ++row) {
                int pos_y = y - 1 + row;
                pos_y     = abs(pos_y);
                pos_y     = pos_y - max(0, 2 * (y - h) + 1);

                int16_t csf_a_val =
                    inline_s0_csf_a(ref, dis, pos_y * src_stride + x, theta, i_rfactor,
                                    adm_enhn_gain_limit);
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
                            thr[thread_item] += (int16_t)(
                                ((ONE_BY_15 * abs((int32_t)csf_a_val)) + 2048) >> 12);
                        }
                    }
                }
            }
        }

        for (int row = 0; row < rows_per_thread; ++row) {
            int16_t sb = 0;
            if ((y + row) < end_row) {
                sb = inline_s0_decouple_r(ref, dis, (y + row) * src_stride + x, band - 1,
                                          adm_enhn_gain_limit);
            }
            int32_t val = abs(int32_t(i_rfactor[blockIdx.z] * sb)) - (thr[row] << shift_sub_block);
            int32_t accum_thread = max(0, val);
            const int32_t x_sq =
                (int32_t)((((int64_t)accum_thread * accum_thread) + add_shift_sq) >> shift_sq);
            accum_row[row] += (((int64_t)x_sq * accum_thread) + add_shift_cub) >> shift_cub;
        }
    }

    __shared__ int64_t s_row[4][32];
    for (int row = 0; row < rows_per_thread; ++row) {
        s_row[threadIdx.y][threadIdx.x] = accum_row[row];
        __syncthreads();
        for (int s = 16; s > 0; s >>= 1) {
            if (threadIdx.x < s) {
                s_row[threadIdx.y][threadIdx.x] += s_row[threadIdx.y][threadIdx.x + s];
            }
            __syncthreads();
        }
        if (threadIdx.x == 0 && (y + row) < end_row) {
            int64_t row_total = s_row[threadIdx.y][0];
            int64_t shifted = (row_total + add_shift_inner_accum) >> shift_inner_accum;
            atomicAdd((uint64_cu *)&accum_global[band2], (uint64_cu)shifted);
        }
    }
}

extern "C" __global__ void adm_cm_line_kernel_8(AdmBufferHip buf, int h, int w, int top, int bottom, int left,
                                     int right, int start_row, int end_row, int start_col,
                                     int end_col, int src_stride, int csf_a_stride, int buffer_h,
                                     int buffer_stride, int32_t *accum_per_block,
                                     AdmFixedParametersHip params, int scale,
                                     int64_t *accum_global, WarpShift ws,
                                     const uint32_t shift_inner_accum,
                                     const uint32_t add_shift_inner_accum)
{
    adm_cm_line_kernel_body<8>(buf, h, w, top, bottom, left, right, start_row, end_row, start_col,
                               end_col, src_stride, csf_a_stride, buffer_h, buffer_stride,
                               accum_per_block, params, scale, accum_global, ws, shift_inner_accum,
                               add_shift_inner_accum);
}

/* ----------------------------------------------------------------------
 * adm_cm_reduce_line_kernel_4 — consumes accum_per_thread scratch (written
 * by i4_adm_cm_line_kernel) and reduces into accum[band] via int64
 * atomicAdd. Sums the full row in int64 and applies shift_inner_accum
 * once per row.
 * -------------------------------------------------------------------- */

template <int val_per_thread>
__device__ __forceinline__ static void
adm_cm_reduce_line_kernel_body(int h, int w, int scale, int buffer_h, int buffer_stride,
                               const int32_t *buffer, int64_t *accum)
{
    const int band  = blockIdx.z;
    const int line  = blockIdx.y;
    const int off   = band * (buffer_h * buffer_stride);
    const int b_off = off + line * buffer_stride;

    uint32_t shift_cub     = (uint32_t)ceilf(log2f((float)w));
    uint32_t add_shift_cub = 1u << (shift_cub - 1);
    int32_t shift_sq       = 30;
    int32_t add_shift_sq   = 536870912; /* 2^29 */
    if (scale == 0) {
        shift_cub     = (uint32_t)ceilf(log2f((float)w) - (float)cm_fixed_shift_const[band]);
        add_shift_cub = 1u << (shift_cub - 1);
        shift_sq      = cm_shift_xsq_const[band];
        add_shift_sq  = cm_add_shift_xsq_const[band];
    }

    int64_t temp_value = 0;
    for (int col = (int)threadIdx.x; col < buffer_stride; col += (int)blockDim.x) {
        const int32_t x = buffer[b_off + col];
        const int32_t x_sq = (int32_t)((((int64_t)x * x) + add_shift_sq) >> shift_sq);
        temp_value += (((int64_t)x_sq * x) + add_shift_cub) >> shift_cub;
    }

    __shared__ int64_t s_row[256];
    s_row[threadIdx.x] = temp_value;
    __syncthreads();
    for (int s = (int)blockDim.x / 2; s > 0; s >>= 1) {
        if ((int)threadIdx.x < s) {
            s_row[threadIdx.x] += s_row[threadIdx.x + s];
        }
        __syncthreads();
    }

    if (threadIdx.x == 0) {
        const uint32_t shift_inner_accum     = (uint32_t)ceilf(log2f((float)h));
        const uint32_t add_shift_inner_accum = 1u << (shift_inner_accum - 1);
        int64_t shifted = (s_row[0] + add_shift_inner_accum) >> shift_inner_accum;
        atomicAdd((uint64_cu *)&accum[band], (uint64_cu)shifted);
    }
}

extern "C" __global__ void adm_cm_reduce_line_kernel_4(int h, int w, int scale, int buffer_h,
                                                       int buffer_stride, const int32_t *buffer,
                                                       int64_t *accum)
{
    adm_cm_reduce_line_kernel_body<4>(h, w, scale, buffer_h, buffer_stride, buffer, accum);
}
