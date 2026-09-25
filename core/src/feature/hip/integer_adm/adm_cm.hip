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
 *
 *  Linkage: the three kernels are `extern "C"` because the host TU resolves
 *  them by name from the embedded HSACO (`hipModuleGetFunction`, ADR-0372).
 *  Their parameter lists mirror the host launch-argument arrays one for one,
 *  so a parameter a kernel does not read stays in the signature unnamed.
 *  Everything else in this file is a device helper with internal linkage.
 */

#include <cstddef>
#include <cstdint>
#include <hip/hip_runtime.h>

#include "adm_cm_accumulator.h"
#include "hip/integer_adm_hip.h"
#include "common.h"

/* Inline decouple helpers (shared with adm_csf.hip). */
#include "adm_decouple_inline.hip"

/* AMD-portable alias for the CUDA uint64_cu atomicAdd target type. */
using uint64_cu = unsigned long long int;

/* Per-band shifts for the int16 compute kernel (matches CUDA twin). */
__device__ __constant__ const int32_t cm_shift_sub_const[3] = {10, 10, 12};
__device__ __constant__ const int cm_fixed_shift_const[3] = {4, 4, 3};
__device__ __constant__ const int32_t cm_shift_xsq_const[3] = {29, 29, 30};
__device__ __constant__ const int32_t cm_add_shift_xsq_const[3] = {268435456, 268435456, 536870912};

namespace
{

/* WarpShift mirrors the CUDA `WarpShift` struct — same layout so the host
 * `hipModuleLaunchKernel` argument blob stays byte-for-byte compatible. */
struct WarpShift {
    uint32_t shift_cub[3];
    uint32_t add_shift_cub[3];
    uint32_t shift_sq[3];
    uint32_t add_shift_sq[3];
};

/* Per-band shift set the scale-0 kernel applies to one masked sample. */
struct S0CmShifts {
    int32_t add_shift_cub;
    int32_t shift_cub;
    int32_t add_shift_sq;
    int32_t shift_sq;
    int32_t shift_sub_block;
};

/* Neighbours of one sample along one axis of the 3x3 CM window. */
struct CmNeighbours {
    int before;
    int after;
};

/* ----------------------------------------------------------------------
 * Inline helpers — i4 (scales 1-3) and s0 (scale 0). Mirror the CUDA
 * twin's `inline_*` helpers line-for-line, with cuda_*_band_t renamed to
 * hip_*_band_t. Required by both compute kernels.
 * -------------------------------------------------------------------- */

/* Select the dis-band sample of orientation `theta` (0 = H, 1 = V, 2 = D). */
template <typename T> __device__ __forceinline__ T select_theta(int theta, T h, T v, T d)
{
    if (theta == 0) {
        return h;
    }
    if (theta == 1) {
        return v;
    }
    return d;
}

__device__ __forceinline__ int32_t inline_i4_csf_a(const hip_i4_adm_dwt_band_t *ref,
                                                   const hip_i4_adm_dwt_band_t *dis, int idx,
                                                   int theta, const uint32_t *rfactor,
                                                   double adm_enhn_gain_limit)
{
    const uint32_t shift_dst = 28;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));

    const int32_t oh = ref->band_h[idx];
    const int32_t ov = ref->band_v[idx];
    const int32_t od = ref->band_d[idx];
    const int32_t th = dis->band_h[idx];
    const int32_t tv = dis->band_v[idx];
    const int32_t td = dis->band_d[idx];

    const int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
    const int32_t r_val =
        decouple_r_s123(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);

    const int32_t t_val = select_theta<int32_t>(theta, th, tv, td);

    const int32_t a_val = t_val - r_val;
    return (int32_t)(((rfactor[theta] * int64_t(a_val)) + add_bef_shift_dst) >> shift_dst);
}

__device__ __forceinline__ int32_t inline_i4_decouple_r(const hip_i4_adm_dwt_band_t *ref,
                                                        const hip_i4_adm_dwt_band_t *dis, int idx,
                                                        int band_idx, double adm_enhn_gain_limit)
{
    const int32_t oh = ref->band_h[idx];
    const int32_t ov = ref->band_v[idx];
    const int32_t od = ref->band_d[idx];
    const int32_t th = dis->band_h[idx];
    const int32_t tv = dis->band_v[idx];
    const int32_t td = dis->band_d[idx];

    const int angle_flag = decouple_angle_flag_s123(oh, ov, th, tv);
    return decouple_r_s123(oh, ov, od, th, tv, td, band_idx, angle_flag, adm_enhn_gain_limit);
}

__device__ __forceinline__ int16_t inline_s0_csf_a(const hip_adm_dwt_band_t *ref,
                                                   const hip_adm_dwt_band_t *dis, int idx,
                                                   int theta, const uint32_t *i_rfactor,
                                                   double adm_enhn_gain_limit)
{
    /* Match CUDA twin's per-band scale-0 shifts. */
    const uint8_t i_shifts_cm[4] = {0, 15, 15, 17};
    const uint16_t i_shiftsadd_cm[4] = {0, 16384, 16384, 65535};

    const int16_t oh = ref->band_h[idx];
    const int16_t ov = ref->band_v[idx];
    const int16_t od = ref->band_d[idx];
    const int16_t th = dis->band_h[idx];
    const int16_t tv = dis->band_v[idx];
    const int16_t td = dis->band_d[idx];

    const int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    const int16_t r_val =
        decouple_r_s0(oh, ov, od, th, tv, td, theta, angle_flag, adm_enhn_gain_limit);

    const int16_t t_val = select_theta<int16_t>(theta, th, tv, td);

    const int16_t a_val = t_val - r_val;
    const int band = theta + 1;
    const int32_t dst_val = i_rfactor[theta] * (uint32_t)a_val;
    return (int16_t)((dst_val + i_shiftsadd_cm[band]) >> i_shifts_cm[band]);
}

__device__ __forceinline__ int16_t inline_s0_decouple_r(const hip_adm_dwt_band_t *ref,
                                                        const hip_adm_dwt_band_t *dis, int idx,
                                                        int band_idx, double adm_enhn_gain_limit)
{
    const int16_t oh = ref->band_h[idx];
    const int16_t ov = ref->band_v[idx];
    const int16_t od = ref->band_d[idx];
    const int16_t th = dis->band_h[idx];
    const int16_t tv = dis->band_v[idx];
    const int16_t td = dis->band_d[idx];

    const int angle_flag = decouple_angle_flag_s0(oh, ov, th, tv);
    return decouple_r_s0(oh, ov, od, th, tv, td, band_idx, angle_flag, adm_enhn_gain_limit);
}

/* ----------------------------------------------------------------------
 * i4 (scales 1-3) CM helpers.
 *
 * Row offsets below are a row index times the band stride. The product
 * indexes a band buffer, so it is bounded by that buffer's element count;
 * it is computed in int, as in the CUDA twin, and the cast only makes its
 * widening to a pointer offset explicit.
 * -------------------------------------------------------------------- */

/* The sample before `idx` mirrors to idx + 1 at the leading edge; the sample
 * after it clamps to idx at the trailing edge. Only one edge applies. */
__device__ __forceinline__ CmNeighbours i4_cm_neighbours(int idx, int len, int lead, int trail)
{
    CmNeighbours n = {idx - 1, idx + 1};
    if (idx == 0 && lead <= 0) {
        n.before = idx + 1;
    } else if (idx == (len - 1) && trail > (len - 1)) {
        n.after = idx;
    }
    return n;
}

/* Sum of the 3x3 CSF-filtered neighbourhood over the three orientations,
 * with the centre sample replaced by the CSF-weighted residual / 15. */
__device__ __forceinline__ int32_t i4_cm_threshold(const hip_i4_adm_dwt_band_t *ref,
                                                   const hip_i4_adm_dwt_band_t *dis,
                                                   int32_t *const *flt_angles, int i, int j,
                                                   CmNeighbours rows, CmNeighbours cols,
                                                   int src_stride, const uint32_t *rfactor,
                                                   double adm_enhn_gain_limit)
{
    /* 1u << 31 wraps to INT32_MIN here exactly as in the CPU reference
     * (ADR-0155); the golden scores encode that rounding. */
    const uint32_t shift_flt = 32;
    const int32_t add_bef_shift_flt = (int32_t)(1u << (shift_flt - 1));

    int32_t thr = 0;
    for (int theta = 0; theta < 3; ++theta) {
        int32_t sum = 0;
        const int32_t csf_a_val =
            inline_i4_csf_a(ref, dis, i * src_stride + j, theta, rfactor, adm_enhn_gain_limit);

        const int32_t *flt_top = flt_angles[theta] + (ptrdiff_t)(rows.before * src_stride);
        const int32_t *flt_mid = flt_angles[theta] + (ptrdiff_t)(i * src_stride);
        const int32_t *flt_bot = flt_angles[theta] + (ptrdiff_t)(rows.after * src_stride);

        sum += flt_top[cols.before];
        sum += flt_top[j];
        sum += flt_top[cols.after];

        sum += flt_mid[cols.before];
        sum +=
            (int32_t)((((int64_t)I4_ONE_BY_15 * abs(csf_a_val)) + add_bef_shift_flt) >> shift_flt);
        sum += flt_mid[cols.after];

        sum += flt_bot[cols.before];
        sum += flt_bot[j];
        sum += flt_bot[cols.after];

        thr += sum;
    }
    return thr;
}

/* Masked contrast of one sample: |rfactor * decouple_r| minus the threshold,
 * floored at zero. */
__device__ __forceinline__ int32_t i4_cm_masked(const hip_i4_adm_dwt_band_t *ref,
                                                const hip_i4_adm_dwt_band_t *dis, int idx,
                                                int band_idx, uint32_t rfactor_band, int32_t thr,
                                                double adm_enhn_gain_limit)
{
    const uint32_t shift_dst = 28;
    const int32_t add_bef_shift_dst = (1u << (shift_dst - 1));
    const int32_t shift_sub_local = 0;

    const int32_t r_val = inline_i4_decouple_r(ref, dis, idx, band_idx, adm_enhn_gain_limit);
    int32_t x = (int32_t)((((int64_t)r_val * rfactor_band) + add_bef_shift_dst) >> shift_dst);
    x = abs(x) - (thr >> shift_sub_local);
    return x < 0 ? 0 : x;
}

/* ----------------------------------------------------------------------
 * s0 (scale 0) CM helpers — see adm_cm_line_kernel_8 below.
 * -------------------------------------------------------------------- */

/* Fold one filtered row into the threshold of every output row it borders. */
template <int rows_per_thread>
__device__ __forceinline__ void s0_cm_add_row(int32_t (&thr)[rows_per_thread], int row,
                                              const int16_t (&flt_row)[3], int16_t csf_a_val)
{
#pragma unroll
    for (int thread_item = 0; thread_item < rows_per_thread; ++thread_item) {
        const int thread_row = row - thread_item;
        if (thread_row >= 0 && thread_row < 3) {
            thr[thread_item] += flt_row[0] + flt_row[2];
            if (thread_row != 1) {
                thr[thread_item] += flt_row[1];
            } else {
                thr[thread_item] += (int16_t)(((ONE_BY_15 * abs((int32_t)csf_a_val)) + 2048) >> 12);
            }
        }
    }
}

/* Thresholds for the `rows_per_thread` output rows starting at `y`, column
 * `x`. Rows mirror at the top edge and clamp at the bottom edge. */
template <int rows_per_thread>
__device__ __forceinline__ void
s0_cm_threshold(const hip_adm_dwt_band_t *ref, const hip_adm_dwt_band_t *dis,
                int16_t *const *flt_angles, const uint32_t *i_rfactor, double adm_enhn_gain_limit,
                int y, int h, int x, const int (&pos_x)[3], int src_stride,
                int32_t (&thr)[rows_per_thread])
{
    const int total_rows = (3 + rows_per_thread - 1);

#pragma unroll
    for (int theta = 0; theta < 3; ++theta) {
#pragma unroll
        for (int row = 0; row < total_rows; ++row) {
            int pos_y = y - 1 + row;
            pos_y = abs(pos_y);
            pos_y = min(pos_y, h - 1);

            const int16_t csf_a_val = inline_s0_csf_a(ref, dis, pos_y * src_stride + x, theta,
                                                      i_rfactor, adm_enhn_gain_limit);
            const int16_t *flt_ptr = flt_angles[theta] + (ptrdiff_t)(pos_y * src_stride);
            const int16_t flt_row[3] = {flt_ptr[pos_x[0]], flt_ptr[pos_x[1]], flt_ptr[pos_x[2]]};
            s0_cm_add_row<rows_per_thread>(thr, row, flt_row, csf_a_val);
        }
    }
}

/* Masked contrast cubed and accumulated per output row. */
template <int rows_per_thread>
__device__ __forceinline__ void
s0_cm_accumulate(const hip_adm_dwt_band_t *ref, const hip_adm_dwt_band_t *dis,
                 const uint32_t *i_rfactor, double adm_enhn_gain_limit, int band, int y, int x,
                 int end_row, int src_stride, const int32_t (&thr)[rows_per_thread],
                 const S0CmShifts &sh, int64_t (&accum_row)[rows_per_thread])
{
    for (int row = 0; row < rows_per_thread; ++row) {
        int16_t sb = 0;
        if ((y + row) < end_row) {
            sb = inline_s0_decouple_r(ref, dis, (y + row) * src_stride + x, band - 1,
                                      adm_enhn_gain_limit);
        }
        const int32_t val =
            abs(int32_t(i_rfactor[blockIdx.z] * sb)) - (thr[row] << sh.shift_sub_block);
        const int32_t accum_thread = max(0, val);
        const int32_t x_sq =
            (int32_t)((((int64_t)accum_thread * accum_thread) + sh.add_shift_sq) >> sh.shift_sq);
        accum_row[row] += (((int64_t)x_sq * accum_thread) + sh.add_shift_cub) >> sh.shift_cub;
    }
}

/* Reduce each output row across the 32 threads of a block row, then add the
 * row total (rounded by shift_inner_accum) into the band accumulator. */
template <int rows_per_thread>
__device__ __forceinline__ void
s0_cm_block_reduce(const int64_t (&accum_row)[rows_per_thread], int y, int end_row,
                   int64_t *accum_band, uint32_t shift_inner_accum, uint32_t add_shift_inner_accum)
{
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
            const int64_t row_total = s_row[threadIdx.y][0];
            const int64_t shifted =
                adm_cm_round_row_total(row_total, add_shift_inner_accum, shift_inner_accum);
            atomicAdd((uint64_cu *)accum_band, (uint64_cu)shifted);
        }
    }
}

/* ----------------------------------------------------------------------
 * adm_cm_line_kernel_8 body — scale-0 compute. Fused warp-equivalent
 * atomicAdd pattern: every block row reduces in shared memory and
 * atomicAdd-s its row total into accum_global (instead of CUDA's per-warp
 * shuffle reduce + per-warp atomicAdd).
 * -------------------------------------------------------------------- */
template <int rows_per_thread>
__device__ __forceinline__ void
adm_cm_line_kernel_body(const AdmBufferHip *__restrict__ buf_ptr, int h, int w, int start_row,
                        int end_row, int start_col, int end_col, int src_stride,
                        AdmFixedParametersHip params, int64_t *accum_global, WarpShift ws,
                        const uint32_t shift_inner_accum, const uint32_t add_shift_inner_accum)
{
    const hip_adm_dwt_band_t *ref = &buf_ptr->ref_dwt2;
    const hip_adm_dwt_band_t *dis = &buf_ptr->dis_dwt2;
    const hip_adm_dwt_band_t *csf_f = &buf_ptr->csf_f;
    const int band = blockIdx.z + 1;
    int16_t *const *flt_angles = csf_f->bands + 1;

    const uint32_t *i_rfactor = params.i_rfactor;
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    const int cta_y = (int)((blockDim.y * blockIdx.y + threadIdx.y) * rows_per_thread);
    const int y = start_row + cta_y;

    const int band2 = blockIdx.z;

    /* Load WarpShift fields into locals (matches CUDA twin). */
    const S0CmShifts sh = {
        (int32_t)ws.add_shift_cub[band2], (int32_t)ws.shift_cub[band2],
        (int32_t)ws.add_shift_sq[band2],  (int32_t)ws.shift_sq[band2],
        cm_shift_sub_const[blockIdx.z],
    };

    int64_t accum_row[rows_per_thread] = {0};

    for (int x = start_col + (int)threadIdx.x; x < end_col; x += (int)blockDim.x) {
        /* ADR-1210's asymmetric rule, as the CPU's adm_cm_thresh(): x - 1 mirrors
         * to 1 at the left edge, x + 1 clamps to w - 1 at the right edge. Both
         * edges are inside the CM region only for bands of 14 samples or less. */
        const int pos_x[3] = {abs(x - 1), x, min(x + 1, w - 1)};

        int32_t thr[rows_per_thread] = {0};
        s0_cm_threshold<rows_per_thread>(ref, dis, flt_angles, i_rfactor, adm_enhn_gain_limit, y, h,
                                         x, pos_x, src_stride, thr);
        s0_cm_accumulate<rows_per_thread>(ref, dis, i_rfactor, adm_enhn_gain_limit, band, y, x,
                                          end_row, src_stride, thr, sh, accum_row);
    }

    s0_cm_block_reduce<rows_per_thread>(accum_row, y, end_row, &accum_global[band2],
                                        shift_inner_accum, add_shift_inner_accum);
}

/* ----------------------------------------------------------------------
 * adm_cm_reduce_line_kernel_4 body — consumes accum_per_thread scratch
 * (written by i4_adm_cm_line_kernel) and reduces into accum[band] via int64
 * atomicAdd. Sums the full row in int64 and applies shift_inner_accum
 * once per row.
 * -------------------------------------------------------------------- */
template <int val_per_thread>
__device__ __forceinline__ void
adm_cm_reduce_line_kernel_body(int h, int w, int scale, int buffer_h, int buffer_stride,
                               const int32_t *buffer, int64_t *accum)
{
    const int band = blockIdx.z;
    const int line = blockIdx.y;
    const int off = band * (buffer_h * buffer_stride);
    const int b_off = off + line * buffer_stride;

    uint32_t shift_cub = (uint32_t)ceilf(log2f((float)w));
    uint32_t add_shift_cub = 1u << (shift_cub - 1);
    int32_t shift_sq = 30;
    int32_t add_shift_sq = 536870912; /* 2^29 */
    if (scale == 0) {
        /* 0 for bands 9 to 16 samples wide (frames 17 to 32 pixels), where
         * 1u << (shift_cub - 1) is undefined; see adm_half_shift(). */
        shift_cub = (uint32_t)ceilf(log2f((float)w) - (float)cm_fixed_shift_const[band]);
        add_shift_cub = (shift_cub > 0u) ? (1u << (shift_cub - 1u)) : 0u;
        shift_sq = cm_shift_xsq_const[band];
        add_shift_sq = cm_add_shift_xsq_const[band];
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
        const uint32_t shift_inner_accum = (uint32_t)ceilf(log2f((float)h));
        const uint32_t add_shift_inner_accum = 1u << (shift_inner_accum - 1);
        const int64_t shifted =
            adm_cm_round_row_total(s_row[0], add_shift_inner_accum, shift_inner_accum);
        atomicAdd((uint64_cu *)&accum[band], (uint64_cu)shifted);
    }
}

} // namespace

extern "C" {

/* ----------------------------------------------------------------------
 * i4_adm_cm_line_kernel — scales 1-3 compute. Writes accum_per_thread
 * scratch consumed by adm_cm_reduce_line_kernel_4. Same launch shape as
 * the CUDA twin (host TU controls grid dims).
 *
 * This kernel and adm_cm_line_kernel_8 take the device-resident
 * AdmBufferHip by pointer (ADR-0759): the host uploads it once in
 * init_fex_hip() and never changes it, so every launch reads the bytes the
 * by-value form used to copy into the kernel arguments.
 * -------------------------------------------------------------------- */
__global__ void i4_adm_cm_line_kernel(const AdmBufferHip *__restrict__ buf_ptr, int h, int w,
                                      int top, int bottom, int left, int right, int start_row,
                                      int end_row, int start_col, int end_col, int src_stride,
                                      int /* csf_a_stride */, int scale, int buffer_h,
                                      int buffer_stride, int32_t *accum_per_thread,
                                      AdmFixedParametersHip params)
{
    const hip_i4_adm_dwt_band_t *ref = &buf_ptr->i4_ref_dwt2;
    const hip_i4_adm_dwt_band_t *dis = &buf_ptr->i4_dis_dwt2;
    const hip_i4_adm_dwt_band_t *csf_f = &buf_ptr->i4_csf_f;
    const int band = blockIdx.z + 1;
    int32_t *const *flt_angles = csf_f->bands + 1;

    const uint32_t *rfactor = &params.i_rfactor[(ptrdiff_t)(scale * 3)];
    const double adm_enhn_gain_limit = params.adm_enhn_gain_limit;

    const int i = start_row + (int)blockIdx.y;
    const int j = start_col + (int)(blockIdx.x * blockDim.x + threadIdx.x);

    int32_t accum_thread = 0;
    if (i < end_row && j < end_col) {
        const CmNeighbours rows = i4_cm_neighbours(i, h, top, bottom);
        const CmNeighbours cols = i4_cm_neighbours(j, w, left, right);
        const int32_t thr = i4_cm_threshold(ref, dis, flt_angles, i, j, rows, cols, src_stride,
                                            rfactor, adm_enhn_gain_limit);
        accum_thread = i4_cm_masked(ref, dis, i * src_stride + j, band - 1, rfactor[blockIdx.z],
                                    thr, adm_enhn_gain_limit);
    }

    const int tid_x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (tid_x < buffer_stride) {
        accum_per_thread[(blockIdx.z * buffer_h + blockIdx.y) * buffer_stride + tid_x] =
            accum_thread;
    }
}

__global__ void adm_cm_line_kernel_8(const AdmBufferHip *__restrict__ buf_ptr, int h, int w,
                                     int /* top */, int /* bottom */, int /* left */,
                                     int /* right */, int start_row, int end_row, int start_col,
                                     int end_col, int src_stride, int /* csf_a_stride */,
                                     int /* buffer_h */, int /* buffer_stride */,
                                     int32_t * /* accum_per_block */, AdmFixedParametersHip params,
                                     int /* scale */, int64_t *accum_global, WarpShift ws,
                                     const uint32_t shift_inner_accum,
                                     const uint32_t add_shift_inner_accum)
{
    adm_cm_line_kernel_body<8>(buf_ptr, h, w, start_row, end_row, start_col, end_col, src_stride,
                               params, accum_global, ws, shift_inner_accum, add_shift_inner_accum);
}

__global__ void adm_cm_reduce_line_kernel_4(int h, int w, int scale, int buffer_h,
                                            int buffer_stride, const int32_t *buffer,
                                            int64_t *accum)
{
    adm_cm_reduce_line_kernel_body<4>(h, w, scale, buffer_h, buffer_stride, buffer, accum);
}

} /* extern "C" */
