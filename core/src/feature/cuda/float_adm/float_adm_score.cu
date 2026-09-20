/**
 *  Copyright 2016-2020 Netflix, Inc.
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
#define FADM_WG_SIZE (FADM_BX * FADM_BY)
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
extern "C" __global__ void
float_adm_dwt_vert(int scale, const uint8_t *ref_raw, const uint8_t *dis_raw, ptrdiff_t raw_stride,
                   const float *parent_ref_band, const float *parent_dis_band,
                   int parent_buf_stride, int parent_half_h, int parent_w, int parent_h,
                   float *dwt_tmp_ref, float *dwt_tmp_dis, int cur_w, int cur_h, int half_h,
                   unsigned bpc, float scaler, float pixel_offset)
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

extern "C" __global__ void float_adm_dwt_hori(int scale, const float *dwt_tmp_ref,
                                              const float *dwt_tmp_dis, float *ref_band,
                                              float *dis_band, int cur_w, int half_w, int half_h,
                                              int buf_stride)
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

extern "C" __global__ void float_adm_decouple_csf(const float *ref_band, const float *dis_band,
                                                  float *csf_a, float *csf_f, int half_w,
                                                  int half_h, int buf_stride, float rfactor_h,
                                                  float rfactor_v, float rfactor_d,
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
    /* Edge policy — must match the CPU closed form in
     * `adm_cm_thresh3x3_s` (core/src/feature/adm_tools.c), which is
     * asymmetric:
     *     i_m1 = (i == 0)     ? 1     : i - 1;   // near edge MIRRORS to 1
     *     i_p1 = (i == h - 1) ? h - 1 : i + 1;   // far  edge CLAMPS to h-1
     * The near edge mirrors, the far edge clamps to the last index.
     * This kernel previously mirrored the far edge as well
     * (`2 * half_w - x - 2`, i.e. w-2), which silently diverged from the
     * CPU reference whenever a scale's border crop collapsed to zero —
     * `(int)(dim * ADM_BORDER_FACTOR - 0.5) == 0` for dim <= 14 — because
     * only then do row 0 / row h-1 / col 0 / col w-1 enter the CM sum.
     * Reads are only ever at +/-1, so clamping to the last index is the
     * exact CPU semantics. See ADR-1204. */
    if (x < 0)
        x = -x;
    if (x >= half_w)
        x = half_w - 1;
    if (y < 0)
        y = -y;
    if (y >= half_h)
        y = half_h - 1;
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

/* p-norm accumulation, mirroring adm_tools.c exactly: the CPU special-cases
 * p == 3 to a literal cube and only falls back to powf() otherwise, so the
 * default path stays bit-identical. ADR-1220. */
__device__ static __forceinline__ float fadm_pnorm_term(float x, float p_norm)
{
    return (p_norm == 3.0f) ? (x * x * x) : powf(x, p_norm);
}

struct FadmBandPair {
    float original[FADM_NUM_BANDS];
    float distorted[FADM_NUM_BANDS];
    bool angle_flag;
};

__device__ static __forceinline__ FadmBandPair fadm_load_band_pair(const float *ref_band,
                                                                   const float *dis_band, int row,
                                                                   int col, int buf_stride,
                                                                   int half_h)
{
    FadmBandPair pair;
    pair.original[0] = fadm_read_band_at(ref_band, 1, row, col, buf_stride, half_h);
    pair.original[1] = fadm_read_band_at(ref_band, 2, row, col, buf_stride, half_h);
    pair.original[2] = fadm_read_band_at(ref_band, 3, row, col, buf_stride, half_h);
    pair.distorted[0] = fadm_read_band_at(dis_band, 1, row, col, buf_stride, half_h);
    pair.distorted[1] = fadm_read_band_at(dis_band, 2, row, col, buf_stride, half_h);
    pair.distorted[2] = fadm_read_band_at(dis_band, 3, row, col, buf_stride, half_h);
    const float dot =
        (pair.original[0] * pair.distorted[0]) + (pair.original[1] * pair.distorted[1]);
    const float original_mag =
        (pair.original[0] * pair.original[0]) + (pair.original[1] * pair.original[1]);
    const float distorted_mag =
        (pair.distorted[0] * pair.distorted[0]) + (pair.distorted[1] * pair.distorted[1]);
    const float lhs = dot * dot;
    const float rhs = FADM_COS_1DEG_SQ * (original_mag * distorted_mag);
    pair.angle_flag = (dot >= 0.0f) && (lhs >= rhs);
    return pair;
}

__device__ static __forceinline__ float fadm_remodulated(const FadmBandPair &pair, unsigned band,
                                                         float gain_limit)
{
    float gain = pair.distorted[band] / (pair.original[band] + FADM_EPS);
    gain = fmaxf(0.0f, fminf(gain, 1.0f));
    float value = gain * pair.original[band];
    if (pair.angle_flag && value > 0.0f)
        value = fminf(value * gain_limit, pair.distorted[band]);
    else if (pair.angle_flag && value < 0.0f)
        value = fmaxf(value * gain_limit, pair.distorted[band]);
    return value;
}

__device__ static float fadm_cm_threshold(const float *csf_a, const float *csf_f, int row, int col,
                                          int half_w, int half_h, int buf_stride)
{
    float threshold = 0.0f;
#pragma unroll
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
#pragma unroll
        for (int dy = -1; dy <= 1; dy++) {
#pragma unroll
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                threshold +=
                    fadm_read_csf_f_at(csf_f, band, row + dy, col + dx, half_w, half_h, buf_stride);
            }
        }
    }
    const float own_h = fadm_read_csf_a_at(csf_a, 0, row, col, half_w, half_h, buf_stride);
    const float own_v = fadm_read_csf_a_at(csf_a, 1, row, col, half_w, half_h, buf_stride);
    const float own_d = fadm_read_csf_a_at(csf_a, 2, row, col, half_w, half_h, buf_stride);
    threshold += FADM_ONE_BY_15 * fabsf(own_h);
    threshold += FADM_ONE_BY_15 * fabsf(own_v);
    threshold += FADM_ONE_BY_15 * fabsf(own_d);
    return threshold;
}

__device__ static void fadm_reduce_pair(float csf, float cm, float *shared_csf, float *shared_cm,
                                        float *accum_out, unsigned workgroup, unsigned band,
                                        unsigned lid)
{
    const float warp_csf = fadm_warp_reduce(csf);
    const float warp_cm = fadm_warp_reduce(cm);
    const unsigned lane = lid % 32u;
    const unsigned warp_id = lid / 32u;
    if (lane == 0u) {
        shared_csf[warp_id] = warp_csf;
        shared_cm[warp_id] = warp_cm;
    }
    __syncthreads();
    if (lid != 0u)
        return;
    float total_csf = 0.0f;
    float total_cm = 0.0f;
#pragma unroll
    for (unsigned i = 0u; i < FADM_WG_SIZE / 32u; i++) {
        total_csf += shared_csf[i];
        total_cm += shared_cm[i];
    }
    const unsigned slot = workgroup * FADM_ACCUM_SLOTS;
    accum_out[slot + band] = total_csf;
    accum_out[slot + 3u + band] = total_cm;
}

__device__ static void fadm_reduce_aim(float aim, float *shared_aim, float *accum_out,
                                       unsigned workgroup, unsigned band, unsigned lid)
{
    const float warp_aim = fadm_warp_reduce(aim);
    const unsigned lane = lid % 32u;
    const unsigned warp_id = lid / 32u;
    if (lane == 0u)
        shared_aim[warp_id] = warp_aim;
    __syncthreads();
    if (lid != 0u)
        return;
    float total = 0.0f;
#pragma unroll
    for (unsigned i = 0u; i < FADM_WG_SIZE / 32u; i++)
        total += shared_aim[i];
    accum_out[workgroup * FADM_ACCUM_SLOTS + 6u + band] = total;
}

extern "C" __global__ void float_adm_csf_cm(const float *ref_band, const float *dis_band,
                                            const float *csf_a, const float *csf_f,
                                            float *accum_out, int half_w, int half_h,
                                            int buf_stride, int active_left, int active_top,
                                            int active_right, int active_bottom, float rfactor_h,
                                            float rfactor_v, float rfactor_d, float gain_limit,
                                            float p_norm, int bypass_cm)
{
    const int active_h = active_bottom - active_top;
    if (active_h <= 0 || active_right <= active_left)
        return;
    const unsigned wg_id = blockIdx.x;
    const unsigned num_rows = (unsigned)active_h;
    const unsigned band_idx = wg_id / num_rows;
    const unsigned row_idx = wg_id - band_idx * num_rows;
    const int row = active_top + (int)row_idx;
    const unsigned lid = threadIdx.y * FADM_BX + threadIdx.x;
    const float rfactor_band = (band_idx == 0u) ? rfactor_h :
                               (band_idx == 1u) ? rfactor_v :
                                                  rfactor_d;
    float local_csf_sum = 0.0f;
    float local_cm_sum = 0.0f;
    for (int col = active_left + (int)lid; col < active_right; col += FADM_WG_SIZE) {
        const FadmBandPair pair =
            fadm_load_band_pair(ref_band, dis_band, row, col, buf_stride, half_h);
        const float csf = fabsf(rfactor_band * pair.original[band_idx]);
        local_csf_sum += fadm_pnorm_term(csf, p_norm);
        const float remodulated = fadm_remodulated(pair, band_idx, gain_limit);
        const float threshold = (bypass_cm == 0) ? fadm_cm_threshold(csf_a, csf_f, row, col, half_w,
                                                                     half_h, buf_stride) :
                                                   0.0f;
        float masked = fabsf(rfactor_band * remodulated) - threshold;
        if (masked < 0.0f)
            masked = 0.0f;
        local_cm_sum += fadm_pnorm_term(masked, p_norm);
    }
    __shared__ float s_csf[FADM_WG_SIZE / 32];
    __shared__ float s_cm[FADM_WG_SIZE / 32];
    fadm_reduce_pair(local_csf_sum, local_cm_sum, s_csf, s_cm, accum_out, wg_id, band_idx, lid);
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
extern "C" __global__ void float_adm_csf_r(const float *ref_band, const float *dis_band,
                                           float *csf_a_out, float *csf_f_out, int half_w,
                                           int half_h, int buf_stride, float rfactor_h,
                                           float rfactor_v, float rfactor_d, float gain_limit)
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
extern "C" __global__ void float_adm_aim_cm(const float *ref_band, const float *dis_band,
                                            const float *csf_a_aim, const float *csf_f_aim,
                                            float *accum_out, int half_w, int half_h,
                                            int buf_stride, int active_left, int active_top,
                                            int active_right, int active_bottom, float rfactor_h,
                                            float rfactor_v, float rfactor_d, float gain_limit,
                                            float p_norm, int bypass_cm)
{
    const int active_h = active_bottom - active_top;
    if (active_h <= 0 || active_right <= active_left)
        return;
    const unsigned wg_id = blockIdx.x;
    const unsigned num_rows = (unsigned)active_h;
    const unsigned band_idx = wg_id / num_rows;
    const unsigned row_idx = wg_id - band_idx * num_rows;
    const int row = active_top + (int)row_idx;
    const unsigned lid = threadIdx.y * FADM_BX + threadIdx.x;
    const float rfactor_band = (band_idx == 0u) ? rfactor_h :
                               (band_idx == 1u) ? rfactor_v :
                                                  rfactor_d;
    float local_aim_cm = 0.0f;
    for (int col = active_left + (int)lid; col < active_right; col += FADM_WG_SIZE) {
        const FadmBandPair pair =
            fadm_load_band_pair(ref_band, dis_band, row, col, buf_stride, half_h);
        const float remodulated = fadm_remodulated(pair, band_idx, gain_limit);
        const float anomaly = pair.distorted[band_idx] - remodulated;
        const float threshold = (bypass_cm == 0) ? fadm_cm_threshold(csf_a_aim, csf_f_aim, row, col,
                                                                     half_w, half_h, buf_stride) :
                                                   0.0f;
        float masked = fabsf(rfactor_band * anomaly) - threshold;
        if (masked < 0.0f)
            masked = 0.0f;
        local_aim_cm += fadm_pnorm_term(masked, p_norm);
    }
    __shared__ float s_aim[FADM_WG_SIZE / 32];
    fadm_reduce_aim(local_aim_cm, s_aim, accum_out, wg_id, band_idx, lid);
}
