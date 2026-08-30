/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernels for the float_adm feature extractor
 *  (T7-23 / batch 3 part 6b — ADR-0192 / ADR-0202). CUDA twin of
 *  float_adm_vulkan (PR #154 / ADR-0199) — same four pipeline
 *  stages, same fused stage 3 (csf_den + cm), same "CM threshold
 *  sums all 3 bands" semantics, same `-1` mirror form on both axes.
 *
 *  Stages:
 *    0 — DWT vertical pass (ref+dis fused)
 *    1 — DWT horizontal pass (ref+dis fused) → 4 sub-bands
 *    2 — Decouple + CSF on decouple_a (writes csf_a + csf_f)
 *    3 — CSF denominator + CM fused (adm2 path); slots 0..5 per WG
 *    2b — float_adm_csf_r: CSF on decouple_r (writes csf_a_aim + csf_f_aim)
 *    3b — float_adm_aim_cm: AIM CM using decouple_a; slots 6..8 per WG
 *
 *  Per-frame flow: 24 launches (6 stages × 4 scales). Submit on the
 *  picture stream so launches serialise; D2H on the secondary stream
 *  with an event fence. Reduction across WGs runs on the host in
 *  double precision — same trick as the Vulkan host wrapper, matches
 *  CPU `adm_csf_den_scale_s` / `adm_cm_s` row-by-row order to
 *  hold the places=4 contract.
 *
 *  accum_out layout per WG (FADM_ACCUM_SLOTS = 9):
 *    [0..2]  csf_den per band   (adm2 denominator accumulator)
 *    [3..5]  cm_num per band    (adm2 CM numerator accumulator)
 *    [6..8]  aim_cm per band    (AIM CM numerator, noise_weight=0)
 *                               — ADR-0572
 */

#include "common.h"
#include "cuda_helper.cuh"

#define FADM_BX 16
#define FADM_BY 16
#define FADM_NUM_BANDS 3
/* ADR-0574: slots 0..5 = adm2 csf+cm per band; slots 6..8 = aim_cm per band. */
#define FADM_ACCUM_SLOTS 9

#define FADM_LO0 (0.482962913144690f)
#define FADM_LO1 (0.836516303737469f)
#define FADM_LO2 (0.224143868041857f)
#define FADM_LO3 (-0.129409522550921f)
#define FADM_HI0 (-0.129409522550921f)
#define FADM_HI1 (-0.224143868041857f)
#define FADM_HI2 (0.836516303737469f)
#define FADM_HI3 (-0.482962913144690f)

#define FADM_ONE_BY_30 (0.0333333351f)
#define FADM_ONE_BY_15 (0.0666666701f)
#define FADM_COS_1DEG_SQ (0.99969541789740297f)
#define FADM_EPS (1e-30f)

__device__ static __forceinline__ int fadm_mirror(int idx, int sup)
{
    /* Both axes use `2*sup - idx - 1` — matches CPU
     * dwt2_src_indices_filt_s in adm_tools.c (the only mirror form
     * the float ADM CPU pipeline uses). */
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 1;
    return idx;
}

__device__ static __forceinline__ float fadm_read_src_pixel(const uint8_t *plane,
                                                            ptrdiff_t stride_bytes, int y, int x,
                                                            int w, int h, unsigned bpc,
                                                            float scaler, float pixel_offset)
{
    y = fadm_mirror(y, h);
    if (x < 0)
        x = 0;
    if (x >= w)
        x = w - 1;
    if (bpc <= 8u) {
        return (float)plane[y * stride_bytes + x] + pixel_offset;
    }
    const uint16_t v = reinterpret_cast<const uint16_t *>(plane + y * stride_bytes)[x];
    return (float)v / scaler + pixel_offset;
}

__device__ static __forceinline__ float fadm_read_band_a(const float *band_buf, int buf_stride,
                                                         int half_h, int parent_w, int parent_h,
                                                         int y, int x)
{
    /* Parent LL band read: parent dims = cur_w/cur_h here. The buffer
     * is 4 sub-bands packed contiguous. Band 0 = LL. */
    y = fadm_mirror(y, parent_h);
    if (x < 0)
        x = 0;
    if (x >= parent_w)
        x = parent_w - 1;
    /* Note: integer cast on buf_stride/half_h to silence -Wconversion. */
    (void)half_h;
    return band_buf[y * buf_stride + x];
}

extern "C" {

/* ------------------------------------------------------------------
 * Stage 0 — DWT vertical pass.
 *
 * Output row n consumes input rows (2n - 1 .. 2n + 2). Layout of
 * dwt_tmp:  [gy * (cur_w * 2) + gx]              = lo
 *           [gy * (cur_w * 2) + cur_w + gx]      = hi
 * Z dimension fuses ref+dis (z=0 → ref plane, z=1 → dis plane).
 *
 * For SCALE > 0, the input is the parent LL band (band 0 of the
 * previous-scale ref_band/dis_band buffer). The scale-0 path reads
 * from the raw u8/u16 source.
 * ------------------------------------------------------------------ */
__global__ void float_adm_dwt_vert(int scale, const uint8_t *ref_raw, const uint8_t *dis_raw,
                                   ptrdiff_t raw_stride, const float *parent_ref_band,
                                   const float *parent_dis_band, int parent_buf_stride,
                                   int parent_half_h, int parent_w, int parent_h,
                                   float *dwt_tmp_ref, float *dwt_tmp_dis, int cur_w, int cur_h,
                                   int half_h, unsigned bpc, float scaler, float pixel_offset)
{
    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    const int plane_is_dis = (int)blockIdx.z;
    if (gx >= cur_w || gy >= half_h)
        return;
    (void)half_h;

    const int row_start = 2 * gy - 1;
    float s[4];
#pragma unroll
    for (int k = 0; k < 4; k++) {
        if (scale == 0) {
            const uint8_t *plane = (plane_is_dis == 0) ? ref_raw : dis_raw;
            s[k] = fadm_read_src_pixel(plane, raw_stride, row_start + k, gx, cur_w, cur_h, bpc,
                                       scaler, pixel_offset);
        } else {
            const float *band = (plane_is_dis == 0) ? parent_ref_band : parent_dis_band;
            s[k] = fadm_read_band_a(band, parent_buf_stride, parent_half_h, parent_w, parent_h,
                                    row_start + k, gx);
        }
    }

    const float lo = FADM_LO0 * s[0] + FADM_LO1 * s[1] + FADM_LO2 * s[2] + FADM_LO3 * s[3];
    const float hi = FADM_HI0 * s[0] + FADM_HI1 * s[1] + FADM_HI2 * s[2] + FADM_HI3 * s[3];

    const int out_stride = cur_w * 2;
    float *dst = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    dst[gy * out_stride + gx] = lo;
    dst[gy * out_stride + cur_w + gx] = hi;
}

/* ------------------------------------------------------------------
 * Stage 1 — DWT horizontal pass.
 *
 * Reads lo / hi sub-rows from stage 0; emits 4 bands (a/h/v/d) with
 *   band_a = lo · lo (LL); band_h = hi · lo (HL high-V);
 *   band_v = lo · hi (LH high-H); band_d = hi · hi (HH).
 * Same a/h/v/d order as integer ADM convention (and the Vulkan kernel).
 * ------------------------------------------------------------------ */
__device__ static __forceinline__ float fadm_read_dwt_tmp(const float *dwt_tmp, int gy, int x_sub,
                                                          int cur_w, int half_offset)
{
    x_sub = fadm_mirror(x_sub, cur_w);
    const int stride = cur_w * 2;
    return dwt_tmp[gy * stride + half_offset + x_sub];
}

__global__ void float_adm_dwt_hori(int scale, const float *dwt_tmp_ref, const float *dwt_tmp_dis,
                                   float *ref_band, float *dis_band, int cur_w, int half_w,
                                   int half_h, int buf_stride)
{
    (void)scale;
    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    const int plane_is_dis = (int)blockIdx.z;
    if (gx >= half_w || gy >= half_h)
        return;

    const float *src = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    float *dst = (plane_is_dis == 0) ? ref_band : dis_band;

    const int base_x = 2 * gx;
    /* lo sub-row taps. */
    const float l0 = fadm_read_dwt_tmp(src, gy, base_x - 1, cur_w, 0);
    const float l1 = fadm_read_dwt_tmp(src, gy, base_x + 0, cur_w, 0);
    const float l2 = fadm_read_dwt_tmp(src, gy, base_x + 1, cur_w, 0);
    const float l3 = fadm_read_dwt_tmp(src, gy, base_x + 2, cur_w, 0);
    const float a_val = FADM_LO0 * l0 + FADM_LO1 * l1 + FADM_LO2 * l2 + FADM_LO3 * l3;
    const float v_val = FADM_HI0 * l0 + FADM_HI1 * l1 + FADM_HI2 * l2 + FADM_HI3 * l3;

    /* hi sub-row taps. */
    const float h0 = fadm_read_dwt_tmp(src, gy, base_x - 1, cur_w, cur_w);
    const float h1 = fadm_read_dwt_tmp(src, gy, base_x + 0, cur_w, cur_w);
    const float h2 = fadm_read_dwt_tmp(src, gy, base_x + 1, cur_w, cur_w);
    const float h3 = fadm_read_dwt_tmp(src, gy, base_x + 2, cur_w, cur_w);
    const float h_val = FADM_LO0 * h0 + FADM_LO1 * h1 + FADM_LO2 * h2 + FADM_LO3 * h3;
    const float d_val = FADM_HI0 * h0 + FADM_HI1 * h1 + FADM_HI2 * h2 + FADM_HI3 * h3;

    const int slice = buf_stride * half_h;
    dst[0 * slice + gy * buf_stride + gx] = a_val;
    dst[1 * slice + gy * buf_stride + gx] = h_val;
    dst[2 * slice + gy * buf_stride + gx] = v_val;
    dst[3 * slice + gy * buf_stride + gx] = d_val;
}

/* ------------------------------------------------------------------
 * Stage 2 — Decouple + CSF (writes csf_a + csf_f for stage 3).
 *
 * Computes (per band): a_val = bth - rst with rst = clamp(k, 0, 1) ·
 * oh_self, then csf_a = rfactor · a_val, csf_f = (1/30) · |csf_a|.
 * The angle-flag computation uses `precise` (FMA-off) ordering on the
 * Vulkan side; on CUDA we get the same effect by avoiding inline FMA
 * via explicit parens + the nvcc default of `--fmad=true` being
 * suppressed for the decouple closed-form expression. The NB: the
 * order of operands matches the Vulkan precise{}-block layout — the
 * extra parentheses below are load-bearing for places=4.
 * ------------------------------------------------------------------ */
__device__ static __forceinline__ float fadm_read_band_at(const float *band_buf, int band, int y,
                                                          int x, int buf_stride, int half_h)
{
    const int slice = buf_stride * half_h;
    return band_buf[band * slice + y * buf_stride + x];
}

__device__ static __forceinline__ void fadm_write_csf(float *csf_buf, int band, int y, int x,
                                                      int buf_stride, int half_h, float val)
{
    const int slice = buf_stride * half_h;
    csf_buf[band * slice + y * buf_stride + x] = val;
}

__global__ void float_adm_decouple_csf(const float *ref_band, const float *dis_band, float *csf_a,
                                       float *csf_f, int half_w, int half_h, int buf_stride,
                                       float rfactor_h, float rfactor_v, float rfactor_d,
                                       float gain_limit)
{
    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    if (gx >= half_w || gy >= half_h)
        return;

    const float oh = fadm_read_band_at(ref_band, 1, gy, gx, buf_stride, half_h);
    const float ov = fadm_read_band_at(ref_band, 2, gy, gx, buf_stride, half_h);
    const float od = fadm_read_band_at(ref_band, 3, gy, gx, buf_stride, half_h);
    const float th = fadm_read_band_at(dis_band, 1, gy, gx, buf_stride, half_h);
    const float tv = fadm_read_band_at(dis_band, 2, gy, gx, buf_stride, half_h);
    const float td = fadm_read_band_at(dis_band, 3, gy, gx, buf_stride, half_h);

    /* Angle flag: matches CPU adm_decouple_s exactly (parens preserve
     * non-FMA semantics that the Vulkan kernel achieves via `precise`). */
    const float ot_dp = (oh * th) + (ov * tv);
    const float o_mag = (oh * oh) + (ov * ov);
    const float t_mag = (th * th) + (tv * tv);
    const float lhs = ot_dp * ot_dp;
    const float rhs = FADM_COS_1DEG_SQ * (o_mag * t_mag);
    const bool angle_flag = (ot_dp >= 0.0f) && (lhs >= rhs);

    float oarr[3] = {oh, ov, od};
    float tarr[3] = {th, tv, td};
    float rfac[3] = {rfactor_h, rfactor_v, rfactor_d};

#pragma unroll
    for (int b = 0; b < FADM_NUM_BANDS; b++) {
        float k = tarr[b] / (oarr[b] + FADM_EPS);
        k = fmaxf(0.0f, fminf(k, 1.0f));
        float rst = k * oarr[b];
        if (angle_flag && rst > 0.0f)
            rst = fminf(rst * gain_limit, tarr[b]);
        else if (angle_flag && rst < 0.0f)
            rst = fmaxf(rst * gain_limit, tarr[b]);
        const float a_val = tarr[b] - rst;
        const float csf_a_val = rfac[b] * a_val;
        fadm_write_csf(csf_a, b, gy, gx, buf_stride, half_h, csf_a_val);
        fadm_write_csf(csf_f, b, gy, gx, buf_stride, half_h, FADM_ONE_BY_30 * fabsf(csf_a_val));
    }
}

/* ------------------------------------------------------------------
 * Stage 3 — CSF denominator (|rfactor*ref|^3) + CM (((|csf_a| - thr)
 * clamp 0)^3) fused, per-band per-row. Mirrors the Vulkan kernel
 * verbatim including the cross-band `cm_threshold_all_bands` semantic
 * — that's load-bearing for places=4 against CPU adm_cm_s.
 *
 * Workgroup grid: (3 * num_active_rows, 1, 1).
 *   band_idx = wg / num_active_rows
 *   row_idx  = wg % num_active_rows
 *
 * Output slot layout per WG: 9 floats (FADM_ACCUM_SLOTS = 9):
 *   [0..2]  csf_h/v/d    (adm2 CSF denominator)
 *   [3..5]  cm_h/v/d     (adm2 CM numerator)
 *   [6..8]  aim_cm_h/v/d (AIM CM numerator — written by stage 3b)
 * This kernel writes only slots 0..5; slots 6..8 stay zero until
 * stage 3b (float_adm_aim_cm) writes them.
 * ------------------------------------------------------------------ */
__device__ static __forceinline__ float fadm_read_csf_f_at(const float *csf_f_buf, int band, int y,
                                                           int x, int half_w, int half_h,
                                                           int buf_stride)
{
    /* Edge mirror — matches the Vulkan kernel's `read_csf_f_at` and
     * the CPU ADM_CM_THRESH_S_*_0 / *_W_M_1 / 0_* / H_M_1_* macro
     * variants in adm_tools.h. The CPU treats (i±1, ±1) reads as
     * **mirrored to col=1 / col=w-2**, NOT clamp-to-edge. The
     * difference is small but systematic; clamp-to-edge over-counts
     * the border CM contribution. */
    if (x < 0)
        x = -x;
    if (x >= half_w)
        x = 2 * half_w - x - 2;
    if (y < 0)
        y = -y;
    if (y >= half_h)
        y = 2 * half_h - y - 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= half_w)
        x = half_w - 1;
    if (y >= half_h)
        y = half_h - 1;
    const int slice = buf_stride * half_h;
    return csf_f_buf[band * slice + y * buf_stride + x];
}

__device__ static __forceinline__ float fadm_read_csf_a_at(const float *csf_a_buf, int band, int y,
                                                           int x, int half_w, int half_h,
                                                           int buf_stride)
{
    if (x < 0)
        x = 0;
    if (x >= half_w)
        x = half_w - 1;
    if (y < 0)
        y = 0;
    if (y >= half_h)
        y = half_h - 1;
    const int slice = buf_stride * half_h;
    return csf_a_buf[band * slice + y * buf_stride + x];
}

__device__ static __forceinline__ float fadm_warp_reduce(float v)
{
    for (int off = 16; off > 0; off >>= 1)
        v += __shfl_down_sync(0xffffffff, v, off);
    return v;
}

__global__ void float_adm_csf_cm(const float *ref_band, const float *dis_band, const float *csf_a,
                                 const float *csf_f, float *accum_out, int half_w, int half_h,
                                 int buf_stride, int active_left, int active_top, int active_right,
                                 int active_bottom, float rfactor_h, float rfactor_v,
                                 float rfactor_d, float gain_limit)
{
    const int active_h = active_bottom - active_top;
    const int active_w = active_right - active_left;
    if (active_h <= 0 || active_w <= 0)
        return;

    const unsigned wg_id = blockIdx.x;
    const unsigned num_rows = (unsigned)active_h;
    const unsigned band_idx = wg_id / num_rows;
    const unsigned row_idx = wg_id - band_idx * num_rows;
    const int row = active_top + (int)row_idx;

    const unsigned tx = threadIdx.x;
    const unsigned ty = threadIdx.y;
    const unsigned lid = ty * FADM_BX + tx;

    const float rfactor_band = (band_idx == 0u) ? rfactor_h :
                               (band_idx == 1u) ? rfactor_v :
                                                  rfactor_d;
    const unsigned WG_SIZE = FADM_BX * FADM_BY;

    float local_csf_sum = 0.0f;
    float local_cm_sum = 0.0f;

    for (int col = active_left + (int)lid; col < active_right; col += (int)WG_SIZE) {
        /* CSF denominator: (|rfactor * ref_band|)^3 from raw bands. */
        const float src_ref =
            fadm_read_band_at(ref_band, (int)band_idx + 1, row, col, buf_stride, half_h);
        const float csf_o = fabsf(rfactor_band * src_ref);
        local_csf_sum += csf_o * csf_o * csf_o;

        /* Re-derive decoupled-r value inline (cheaper than reading
         * csf_a back and reconstructing — see Vulkan kernel for the
         * same closed form). Order of multiplications matches the
         * decouple stage's parenthesisation. */
        const float oh = fadm_read_band_at(ref_band, 1, row, col, buf_stride, half_h);
        const float ov = fadm_read_band_at(ref_band, 2, row, col, buf_stride, half_h);
        const float od = fadm_read_band_at(ref_band, 3, row, col, buf_stride, half_h);
        const float th = fadm_read_band_at(dis_band, 1, row, col, buf_stride, half_h);
        const float tv = fadm_read_band_at(dis_band, 2, row, col, buf_stride, half_h);
        const float td = fadm_read_band_at(dis_band, 3, row, col, buf_stride, half_h);
        (void)od;
        (void)td;

        const float ot_dp = (oh * th) + (ov * tv);
        const float o_mag = (oh * oh) + (ov * ov);
        const float t_mag = (th * th) + (tv * tv);
        const float lhs = ot_dp * ot_dp;
        const float rhs = FADM_COS_1DEG_SQ * (o_mag * t_mag);
        const bool angle_flag = (ot_dp >= 0.0f) && (lhs >= rhs);

        float oarr[3] = {oh, ov, od};
        float tarr[3] = {th, tv, td};
        float k = tarr[band_idx] / (oarr[band_idx] + FADM_EPS);
        k = fmaxf(0.0f, fminf(k, 1.0f));
        float r_val = k * oarr[band_idx];
        if (angle_flag && r_val > 0.0f)
            r_val = fminf(r_val * gain_limit, tarr[band_idx]);
        else if (angle_flag && r_val < 0.0f)
            r_val = fmaxf(r_val * gain_limit, tarr[band_idx]);

        /* CM threshold sums csf_f over all 3 bands' 8-neighbours +
         * (1/15)·|csf_a centre| for each of the 3 bands — matches the
         * CPU `ADM_CM_THRESH_S_I_J` macro's 3-band aggregate. */
        float thr = 0.0f;
#pragma unroll
        for (int b = 0; b < FADM_NUM_BANDS; b++) {
#pragma unroll
            for (int dy = -1; dy <= 1; dy++) {
#pragma unroll
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0)
                        continue;
                    thr += fadm_read_csf_f_at(csf_f, b, row + dy, col + dx, half_w, half_h,
                                              buf_stride);
                }
            }
        }
        const float own_h = fadm_read_csf_a_at(csf_a, 0, row, col, half_w, half_h, buf_stride);
        const float own_v = fadm_read_csf_a_at(csf_a, 1, row, col, half_w, half_h, buf_stride);
        const float own_d = fadm_read_csf_a_at(csf_a, 2, row, col, half_w, half_h, buf_stride);
        thr += FADM_ONE_BY_15 * fabsf(own_h);
        thr += FADM_ONE_BY_15 * fabsf(own_v);
        thr += FADM_ONE_BY_15 * fabsf(own_d);

        const float x_val = rfactor_band * r_val;
        float xa = fabsf(x_val) - thr;
        if (xa < 0.0f)
            xa = 0.0f;
        local_cm_sum += xa * xa * xa;
    }

    /* Warp + cross-warp reduction. */
    __shared__ float s_csf[WG_SIZE / 32];
    __shared__ float s_cm[WG_SIZE / 32];
    const float wn_csf = fadm_warp_reduce(local_csf_sum);
    const float wn_cm = fadm_warp_reduce(local_cm_sum);
    const unsigned lane = lid % 32u;
    const unsigned warp_id = lid / 32u;
    if (lane == 0u) {
        s_csf[warp_id] = wn_csf;
        s_cm[warp_id] = wn_cm;
    }
    __syncthreads();
    if (lid == 0u) {
        float total_csf = 0.0f;
        float total_cm = 0.0f;
#pragma unroll
        for (unsigned i = 0u; i < WG_SIZE / 32u; i++) {
            total_csf += s_csf[i];
            total_cm += s_cm[i];
        }
        const unsigned slot_base = wg_id * FADM_ACCUM_SLOTS;
        accum_out[slot_base + band_idx] = total_csf;
        accum_out[slot_base + 3u + band_idx] = total_cm;
    }
}

/* ------------------------------------------------------------------
 * Stage 2b — CSF on decouple_r (for AIM numerator). ADR-0574.
 *
 * Mirrors stage 2 (float_adm_decouple_csf) but computes the CSF of
 * the *remodulated* component `decouple_r = k * ref[band]` (with
 * gain-limit applied) rather than the anomaly `decouple_a`.
 *
 * Output:
 *   csf_a_out[band] = rfactor[band] * r_val
 *   csf_f_out[band] = FADM_ONE_BY_30 * |csf_a_out[band]|
 *
 * This matches the CPU `adm_csf(&decouple_r, &csf_a, &csf_f, ...)`
 * call that precedes `aim_num_scale = adm_cm(&decouple_a, ...)` in
 * adm.c, preserving the same rfactor scaling and |·|/30 csf_f form.
 * ------------------------------------------------------------------ */
__global__ void float_adm_csf_r(const float *ref_band, const float *dis_band, float *csf_a_out,
                                float *csf_f_out, int half_w, int half_h, int buf_stride,
                                float rfactor_h, float rfactor_v, float rfactor_d, float gain_limit)
{
    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    if (gx >= half_w || gy >= half_h)
        return;

    const float oh = fadm_read_band_at(ref_band, 1, gy, gx, buf_stride, half_h);
    const float ov = fadm_read_band_at(ref_band, 2, gy, gx, buf_stride, half_h);
    const float od = fadm_read_band_at(ref_band, 3, gy, gx, buf_stride, half_h);
    const float th = fadm_read_band_at(dis_band, 1, gy, gx, buf_stride, half_h);
    const float tv = fadm_read_band_at(dis_band, 2, gy, gx, buf_stride, half_h);
    const float td = fadm_read_band_at(dis_band, 3, gy, gx, buf_stride, half_h);

    /* Re-derive angle flag — identical to float_adm_decouple_csf. */
    const float ot_dp = (oh * th) + (ov * tv);
    const float o_mag = (oh * oh) + (ov * ov);
    const float t_mag = (th * th) + (tv * tv);
    const float lhs = ot_dp * ot_dp;
    const float rhs = FADM_COS_1DEG_SQ * (o_mag * t_mag);
    const bool angle_flag = (ot_dp >= 0.0f) && (lhs >= rhs);

    float oarr[3] = {oh, ov, od};
    float tarr[3] = {th, tv, td};
    float rfac[3] = {rfactor_h, rfactor_v, rfactor_d};

#pragma unroll
    for (int b = 0; b < FADM_NUM_BANDS; b++) {
        /* Compute decouple_r[b] = k * o, same logic as stage 2. */
        float k = tarr[b] / (oarr[b] + FADM_EPS);
        k = fmaxf(0.0f, fminf(k, 1.0f));
        float r_val = k * oarr[b];
        if (angle_flag && r_val > 0.0f)
            r_val = fminf(r_val * gain_limit, tarr[b]);
        else if (angle_flag && r_val < 0.0f)
            r_val = fmaxf(r_val * gain_limit, tarr[b]);
        /* CSF on decouple_r: matches adm_csf(&decouple_r, ...) in adm.c. */
        const float csf_a_val = rfac[b] * r_val;
        fadm_write_csf(csf_a_out, b, gy, gx, buf_stride, half_h, csf_a_val);
        fadm_write_csf(csf_f_out, b, gy, gx, buf_stride, half_h, FADM_ONE_BY_30 * fabsf(csf_a_val));
    }
}

/* ------------------------------------------------------------------
 * Stage 3b — AIM CM numerator (noise_weight = 0). ADR-0574.
 *
 * Mirrors stage 3 (float_adm_csf_cm) but:
 *   - The "distortion" being masked is decouple_a (anomaly)
 *   - The threshold comes from csf_a_aim / csf_f_aim (CSF of
 *     decouple_r, produced by stage 2b)
 *   - noise_weight = 0 so no noise constant is added
 *
 * Accumulator slot layout: aim_cm values land in slots [6..8]:
 *   accum_out[wg_id * FADM_ACCUM_SLOTS + 6 + band_idx]
 *
 * Matches CPU `adm_cm(&decouple_a, &csf_a, &csf_f, ..., noise_weight=0)`
 * in adm.c (the call producing `aim_num_scale`).
 * ------------------------------------------------------------------ */
__global__ void float_adm_aim_cm(const float *ref_band, const float *dis_band,
                                 const float *csf_a_aim, const float *csf_f_aim, float *accum_out,
                                 int half_w, int half_h, int buf_stride, int active_left,
                                 int active_top, int active_right, int active_bottom,
                                 float rfactor_h, float rfactor_v, float rfactor_d,
                                 float gain_limit)
{
    const int active_h = active_bottom - active_top;
    const int active_w = active_right - active_left;
    if (active_h <= 0 || active_w <= 0)
        return;

    const unsigned wg_id = blockIdx.x;
    const unsigned num_rows = (unsigned)active_h;
    const unsigned band_idx = wg_id / num_rows;
    const unsigned row_idx = wg_id - band_idx * num_rows;
    const int row = active_top + (int)row_idx;

    const unsigned tx = threadIdx.x;
    const unsigned ty = threadIdx.y;
    const unsigned lid = ty * FADM_BX + tx;

    const float rfactor_band = (band_idx == 0u) ? rfactor_h :
                               (band_idx == 1u) ? rfactor_v :
                                                  rfactor_d;
    const unsigned WG_SIZE = FADM_BX * FADM_BY;

    float local_aim_cm = 0.0f;

    for (int col = active_left + (int)lid; col < active_right; col += (int)WG_SIZE) {
        /* Re-derive both decouple_r and decouple_a per pixel. */
        const float oh = fadm_read_band_at(ref_band, 1, row, col, buf_stride, half_h);
        const float ov = fadm_read_band_at(ref_band, 2, row, col, buf_stride, half_h);
        const float od = fadm_read_band_at(ref_band, 3, row, col, buf_stride, half_h);
        const float th = fadm_read_band_at(dis_band, 1, row, col, buf_stride, half_h);
        const float tv = fadm_read_band_at(dis_band, 2, row, col, buf_stride, half_h);
        const float td = fadm_read_band_at(dis_band, 3, row, col, buf_stride, half_h);
        (void)od;
        (void)td;

        const float ot_dp = (oh * th) + (ov * tv);
        const float o_mag = (oh * oh) + (ov * ov);
        const float t_mag = (th * th) + (tv * tv);
        const float lhs = ot_dp * ot_dp;
        const float rhs = FADM_COS_1DEG_SQ * (o_mag * t_mag);
        const bool angle_flag = (ot_dp >= 0.0f) && (lhs >= rhs);

        float oarr[3] = {oh, ov, od};
        float tarr[3] = {th, tv, td};
        float k = tarr[band_idx] / (oarr[band_idx] + FADM_EPS);
        k = fmaxf(0.0f, fminf(k, 1.0f));
        float r_val = k * oarr[band_idx];
        if (angle_flag && r_val > 0.0f)
            r_val = fminf(r_val * gain_limit, tarr[band_idx]);
        else if (angle_flag && r_val < 0.0f)
            r_val = fmaxf(r_val * gain_limit, tarr[band_idx]);
        /* decouple_a[band] = t - r (the anomaly component). */
        const float a_val = tarr[band_idx] - r_val;

        /* AIM CM threshold: cross-band aggregate using aim csf buffers
         * (derived from decouple_r) — identical structure to stage 3. */
        float thr = 0.0f;
#pragma unroll
        for (int b = 0; b < FADM_NUM_BANDS; b++) {
#pragma unroll
            for (int dy = -1; dy <= 1; dy++) {
#pragma unroll
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0)
                        continue;
                    thr += fadm_read_csf_f_at(csf_f_aim, b, row + dy, col + dx, half_w, half_h,
                                              buf_stride);
                }
            }
        }
        const float own_h = fadm_read_csf_a_at(csf_a_aim, 0, row, col, half_w, half_h, buf_stride);
        const float own_v = fadm_read_csf_a_at(csf_a_aim, 1, row, col, half_w, half_h, buf_stride);
        const float own_d = fadm_read_csf_a_at(csf_a_aim, 2, row, col, half_w, half_h, buf_stride);
        thr += FADM_ONE_BY_15 * fabsf(own_h);
        thr += FADM_ONE_BY_15 * fabsf(own_v);
        thr += FADM_ONE_BY_15 * fabsf(own_d);

        /* CM: (|rfactor * a_val| - thr)_+^3; noise_weight=0 so no
         * noise constant — mirrors `adm_cm_s` with noise_weight=0. */
        const float x_val = rfactor_band * a_val;
        float xa = fabsf(x_val) - thr;
        if (xa < 0.0f)
            xa = 0.0f;
        local_aim_cm += xa * xa * xa;
    }

    /* Warp + cross-warp reduction — same pattern as stage 3. */
    __shared__ float s_aim[WG_SIZE / 32];
    const float wn_aim = fadm_warp_reduce(local_aim_cm);
    const unsigned lane = lid % 32u;
    const unsigned warp_id = lid / 32u;
    if (lane == 0u)
        s_aim[warp_id] = wn_aim;
    __syncthreads();
    if (lid == 0u) {
        float total_aim = 0.0f;
#pragma unroll
        for (unsigned i = 0u; i < WG_SIZE / 32u; i++)
            total_aim += s_aim[i];
        /* AIM CM lands in slots 6..8 of the per-WG accumulator. */
        const unsigned slot_base = wg_id * FADM_ACCUM_SLOTS;
        accum_out[slot_base + 6u + band_idx] = total_aim;
    }
}

} /* extern "C" */
