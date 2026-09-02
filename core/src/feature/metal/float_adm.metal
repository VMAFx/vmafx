/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernels for the float_adm feature extractor.
 *  Port of `core/src/feature/cuda/float_adm/float_adm_score.cu` (CUDA twin,
 *  ADR-0192 / ADR-0202 / ADR-0574) to MSL — same six pipeline stages, same
 *  `-1` / edge-mirror forms, same cross-band CM threshold, same AIM pass.
 *
 *  Six kernel functions, dispatched in order by `float_adm_metal.mm` once
 *  per scale (4 scales total):
 *
 *    Stage 0 — float_adm_dwt_vert_{8,16}bpc
 *        9/7-tap DWT vertical pass (separable lo/hi). At scale 0 reads the
 *        raw u8/u16 source plane (the only stage that is bpc-specific —
 *        hence the two variants); at scales >0 reads the parent LL band.
 *        Emits lo/hi sub-rows into a packed dwt_tmp scratch.
 *        Grid: ceil(cur_w/16) x ceil(half_h/16), 16x16 threads, 2 z-slices
 *        (z=0 ref, z=1 dis) flattened to a `plane_is_dis` flag.
 *
 *    Stage 1 — float_adm_dwt_hori
 *        DWT horizontal pass — reads lo/hi sub-rows, emits 4 sub-bands
 *        (a=LL, h=HL, v=LH, d=HH) packed contiguous in ref_band/dis_band.
 *        Grid: ceil(half_w/16) x ceil(half_h/16), 16x16, 2 z-slices.
 *
 *    Stage 2 — float_adm_decouple_csf
 *        Decouple (anomaly) + CSF on decouple_a → csf_a + csf_f for stage 3.
 *        Grid: ceil(half_w/16) x ceil(half_h/16), 16x16.
 *
 *    Stage 3 — float_adm_csf_cm
 *        Fused CSF denominator (|rfactor*ref|^3) + CM numerator
 *        ((|csf_a| - thr)_+^3) with cross-band 3x3 threshold. 1D dispatch:
 *        (3 * num_active_rows) threadgroups, each TG reduces one band-row
 *        across active columns. Writes accum slots [0..5].
 *
 *    Stage 2b — float_adm_csf_r
 *        CSF on decouple_r (remodulated component) → csf_a_aim + csf_f_aim
 *        for the AIM pass. Same grid as stage 2.
 *
 *    Stage 3b — float_adm_aim_cm
 *        AIM CM numerator (noise_weight=0) masking decouple_a with the
 *        decouple_r CSF threshold. Writes accum slots [6..8]. Same 1D
 *        dispatch as stage 3.
 *
 *  accum_out layout per threadgroup (FADM_ACCUM_SLOTS = 9):
 *    [0..2] csf_den per band (adm2 denominator)
 *    [3..5] cm_num per band  (adm2 CM numerator)
 *    [6..8] aim_cm per band  (AIM CM numerator)
 *  Host reduces these across threadgroups in double precision and applies
 *  the cube-root / p-norm pooling, mirroring float_adm_cuda.c::collect.
 *
 *  Numeric design notes:
 *   - No 64-bit atomics in MSL → per-TG partial sums via simd_sum +
 *     threadgroup reduction, then a single float per (band, row) written
 *     to accum_out; host sums in double.
 *   - Parenthesisation of the angle-flag dot products is load-bearing for
 *     places=4 parity (matches the CUDA twin + CPU adm_decouple_s).
 *   - Watson-97 CSF (adm_csf_mode 0) only — the default CPU path. Other
 *     modes are rejected in the .mm init (matches the CUDA twin).
 */

#include <metal_stdlib>
using namespace metal;

#define FADM_NUM_BANDS 3
#define FADM_ACCUM_SLOTS 9

/* 9/7 biorthogonal DWT taps (same constants as the CUDA twin / CPU
 * adm_tools.c dwt2_db2_coeffs_lo / _hi). */
constant float FADM_LO0 = 0.482962913144690f;
constant float FADM_LO1 = 0.836516303737469f;
constant float FADM_LO2 = 0.224143868041857f;
constant float FADM_LO3 = -0.129409522550921f;
constant float FADM_HI0 = -0.129409522550921f;
constant float FADM_HI1 = -0.224143868041857f;
constant float FADM_HI2 = 0.836516303737469f;
constant float FADM_HI3 = -0.482962913144690f;

constant float FADM_ONE_BY_30 = 0.0333333351f;
constant float FADM_ONE_BY_15 = 0.0666666701f;
constant float FADM_COS_1DEG_SQ = 0.99969541789740297f;
constant float FADM_EPS = 1e-30f;

/* Geometry uniform shared by every stage. Packed to keep the
 * setBytes payload small and stable across stages. */
struct FadmDims {
    int scale;
    int cur_w;
    int cur_h;
    int half_w;
    int half_h;
    int buf_stride;
    int parent_w;
    int parent_h;
    int parent_half_h;
    int parent_buf_stride;
    uint bpc;     /* informational; the 8/16 split is by kernel name */
    uint _pad0;
};

/* CM / CSF stage uniform: active region + per-band rfactors + gain. */
struct FadmCsf {
    int active_left;
    int active_top;
    int active_right;
    int active_bottom;
    float rfactor_h;
    float rfactor_v;
    float rfactor_d;
    float gain_limit;
    float scaler;       /* >8bpc raw divisor (informational here) */
    float pixel_offset; /* -128 */
    uint _pad0;
    uint _pad1;
};

/* Both axes use `2*sup - idx - 1` for the over-range mirror and `-idx`
 * for the negative mirror — matches dwt2_src_indices_filt_s in
 * adm_tools.c (the only mirror form the float ADM CPU pipeline uses). */
static inline int fadm_mirror(int idx, int sup)
{
    if (idx < 0) { return -idx; }
    if (idx >= sup) { return 2 * sup - idx - 1; }
    return idx;
}

static inline float fadm_read_band_a(const device float *band_buf, int buf_stride, int parent_w,
                                     int parent_h, int y, int x)
{
    y = fadm_mirror(y, parent_h);
    if (x < 0) { x = 0; }
    if (x >= parent_w) { x = parent_w - 1; }
    return band_buf[y * buf_stride + x];
}

static inline float fadm_read_band_at(const device float *band_buf, int band, int y, int x,
                                      int buf_stride, int half_h)
{
    const int slice = buf_stride * half_h;
    return band_buf[band * slice + y * buf_stride + x];
}

static inline void fadm_write_csf(device float *csf_buf, int band, int y, int x, int buf_stride,
                                  int half_h, float val)
{
    const int slice = buf_stride * half_h;
    csf_buf[band * slice + y * buf_stride + x] = val;
}

/* Edge mirror for csf_f reads — matches the CUDA twin's read_csf_f_at and
 * the CPU ADM_CM_THRESH_S_* macro variants: (i±1, ±1) reads mirror to
 * col=1 / col=w-2 (NOT clamp-to-edge). */
static inline float fadm_read_csf_f_at(const device float *csf_f_buf, int band, int y, int x,
                                       int half_w, int half_h, int buf_stride)
{
    if (x < 0) { x = -x; }
    if (x >= half_w) { x = 2 * half_w - x - 2; }
    if (y < 0) { y = -y; }
    if (y >= half_h) { y = 2 * half_h - y - 2; }
    if (x < 0) { x = 0; }
    if (y < 0) { y = 0; }
    if (x >= half_w) { x = half_w - 1; }
    if (y >= half_h) { y = half_h - 1; }
    const int slice = buf_stride * half_h;
    return csf_f_buf[band * slice + y * buf_stride + x];
}

static inline float fadm_read_csf_a_at(const device float *csf_a_buf, int band, int y, int x,
                                       int half_w, int half_h, int buf_stride)
{
    if (x < 0) { x = 0; }
    if (x >= half_w) { x = half_w - 1; }
    if (y < 0) { y = 0; }
    if (y >= half_h) { y = half_h - 1; }
    const int slice = buf_stride * half_h;
    return csf_a_buf[band * slice + y * buf_stride + x];
}

/* ------------------------------------------------------------------ */
/*  Stage 0 — DWT vertical pass (8-bpc + 16-bpc raw read variants).    */
/*                                                                      */
/*  Output row n consumes input rows (2n-1 .. 2n+2). Layout of dwt_tmp: */
/*    [gy * (cur_w*2) + gx]         = lo                                */
/*    [gy * (cur_w*2) + cur_w + gx] = hi                                */
/*  plane_is_dis flag (z-slice) selects ref vs dis source + dst.        */
/*                                                                      */
/*  Buffer bindings:                                                    */
/*   [[buffer(0)]] ref_raw / parent_ref_band                           */
/*   [[buffer(1)]] dis_raw / parent_dis_band                           */
/*   [[buffer(2)]] dwt_tmp_ref                                          */
/*   [[buffer(3)]] dwt_tmp_dis                                          */
/*   [[buffer(4)]] dims (FadmDims)                                      */
/*   [[buffer(5)]] csf  (FadmCsf — for scaler/pixel_offset)            */
/*  Grid z = 2 (ref/dis).                                              */
/* ------------------------------------------------------------------ */
static void fadm_dwt_vert_impl(const device uchar *ref_raw_u8, const device ushort *ref_raw_u16,
                               const device uchar *dis_raw_u8, const device ushort *dis_raw_u16,
                               const device float *parent_ref_band,
                               const device float *parent_dis_band, device float *dwt_tmp_ref,
                               device float *dwt_tmp_dis, constant FadmDims &d, constant FadmCsf &c,
                               uint3 gid, bool is16)
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    const int plane_is_dis = (int)gid.z;
    if (gx >= d.cur_w || gy >= d.half_h) { return; }

    const int row_start = 2 * gy - 1;
    /* raw_stride in elements for the source plane (cur_w at scale 0). */
    const int raw_stride = d.cur_w;
    float s[4];
    for (int k = 0; k < 4; ++k) {
        if (d.scale == 0) {
            int yy = fadm_mirror(row_start + k, d.cur_h);
            int xx = gx;
            if (xx < 0) { xx = 0; }
            if (xx >= d.cur_w) { xx = d.cur_w - 1; }
            if (is16) {
                const device ushort *plane = (plane_is_dis == 0) ? ref_raw_u16 : dis_raw_u16;
                s[k] = (float)plane[yy * raw_stride + xx] / c.scaler + c.pixel_offset;
            } else {
                const device uchar *plane = (plane_is_dis == 0) ? ref_raw_u8 : dis_raw_u8;
                s[k] = (float)plane[yy * raw_stride + xx] + c.pixel_offset;
            }
        } else {
            const device float *band = (plane_is_dis == 0) ? parent_ref_band : parent_dis_band;
            s[k] = fadm_read_band_a(band, d.parent_buf_stride, d.parent_w, d.parent_h,
                                    row_start + k, gx);
        }
    }

    const float lo = FADM_LO0 * s[0] + FADM_LO1 * s[1] + FADM_LO2 * s[2] + FADM_LO3 * s[3];
    const float hi = FADM_HI0 * s[0] + FADM_HI1 * s[1] + FADM_HI2 * s[2] + FADM_HI3 * s[3];

    const int out_stride = d.cur_w * 2;
    device float *dst = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    dst[gy * out_stride + gx] = lo;
    dst[gy * out_stride + d.cur_w + gx] = hi;
}

kernel void float_adm_dwt_vert_8bpc(const device uchar *ref_raw [[buffer(0)]],
                                    const device uchar *dis_raw [[buffer(1)]],
                                    const device float *parent_ref_band [[buffer(6)]],
                                    const device float *parent_dis_band [[buffer(7)]],
                                    device float *dwt_tmp_ref [[buffer(2)]],
                                    device float *dwt_tmp_dis [[buffer(3)]],
                                    constant FadmDims &d [[buffer(4)]],
                                    constant FadmCsf &c [[buffer(5)]],
                                    uint3 gid [[thread_position_in_grid]])
{
    fadm_dwt_vert_impl(ref_raw, (const device ushort *)0, dis_raw, (const device ushort *)0,
                       parent_ref_band, parent_dis_band, dwt_tmp_ref, dwt_tmp_dis, d, c, gid, false);
}

kernel void float_adm_dwt_vert_16bpc(const device ushort *ref_raw [[buffer(0)]],
                                     const device ushort *dis_raw [[buffer(1)]],
                                     const device float *parent_ref_band [[buffer(6)]],
                                     const device float *parent_dis_band [[buffer(7)]],
                                     device float *dwt_tmp_ref [[buffer(2)]],
                                     device float *dwt_tmp_dis [[buffer(3)]],
                                     constant FadmDims &d [[buffer(4)]],
                                     constant FadmCsf &c [[buffer(5)]],
                                     uint3 gid [[thread_position_in_grid]])
{
    fadm_dwt_vert_impl((const device uchar *)0, ref_raw, (const device uchar *)0, dis_raw,
                       parent_ref_band, parent_dis_band, dwt_tmp_ref, dwt_tmp_dis, d, c, gid, true);
}

/* ------------------------------------------------------------------ */
/*  Stage 1 — DWT horizontal pass.                                     */
/*  Emits 4 bands: a=lo·lo (LL), h=lo·hi (HL high-V written to band1), */
/*  v=hi·lo (LH high-H band2), d=hi·hi (HH band3) — same a/h/v/d order  */
/*  as the CUDA twin.                                                  */
/* ------------------------------------------------------------------ */
static inline float fadm_read_dwt_tmp(const device float *dwt_tmp, int gy, int x_sub, int cur_w,
                                      int half_offset)
{
    x_sub = fadm_mirror(x_sub, cur_w);
    const int stride = cur_w * 2;
    return dwt_tmp[gy * stride + half_offset + x_sub];
}

kernel void float_adm_dwt_hori(const device float *dwt_tmp_ref [[buffer(0)]],
                               const device float *dwt_tmp_dis [[buffer(1)]],
                               device float *ref_band [[buffer(2)]],
                               device float *dis_band [[buffer(3)]],
                               constant FadmDims &d [[buffer(4)]],
                               uint3 gid [[thread_position_in_grid]])
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    const int plane_is_dis = (int)gid.z;
    if (gx >= d.half_w || gy >= d.half_h) { return; }

    const device float *src = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    device float *dst = (plane_is_dis == 0) ? ref_band : dis_band;

    const int cur_w = d.cur_w;
    const int base_x = 2 * gx;

    /* lo sub-row taps → a (LL) and v (LH high-H). */
    const float l0 = fadm_read_dwt_tmp(src, gy, base_x - 1, cur_w, 0);
    const float l1 = fadm_read_dwt_tmp(src, gy, base_x + 0, cur_w, 0);
    const float l2 = fadm_read_dwt_tmp(src, gy, base_x + 1, cur_w, 0);
    const float l3 = fadm_read_dwt_tmp(src, gy, base_x + 2, cur_w, 0);
    const float a_val = FADM_LO0 * l0 + FADM_LO1 * l1 + FADM_LO2 * l2 + FADM_LO3 * l3;
    const float v_val = FADM_HI0 * l0 + FADM_HI1 * l1 + FADM_HI2 * l2 + FADM_HI3 * l3;

    /* hi sub-row taps → h (HL) and d (HH). */
    const float h0 = fadm_read_dwt_tmp(src, gy, base_x - 1, cur_w, cur_w);
    const float h1 = fadm_read_dwt_tmp(src, gy, base_x + 0, cur_w, cur_w);
    const float h2 = fadm_read_dwt_tmp(src, gy, base_x + 1, cur_w, cur_w);
    const float h3 = fadm_read_dwt_tmp(src, gy, base_x + 2, cur_w, cur_w);
    const float h_val = FADM_LO0 * h0 + FADM_LO1 * h1 + FADM_LO2 * h2 + FADM_LO3 * h3;
    const float d_val = FADM_HI0 * h0 + FADM_HI1 * h1 + FADM_HI2 * h2 + FADM_HI3 * h3;

    const int slice = d.buf_stride * d.half_h;
    dst[0 * slice + gy * d.buf_stride + gx] = a_val;
    dst[1 * slice + gy * d.buf_stride + gx] = h_val;
    dst[2 * slice + gy * d.buf_stride + gx] = v_val;
    dst[3 * slice + gy * d.buf_stride + gx] = d_val;
}

/* ------------------------------------------------------------------ */
/*  Decouple closed form shared by stages 2, 2b, 3, 3b.                */
/*  Returns decouple_r[band] (remodulated) for the requested band; the */
/*  anomaly is t - r at the call site. `angle_flag` parens are         */
/*  load-bearing for places=4 (no FMA fusion across the dot products). */
/* ------------------------------------------------------------------ */
static inline bool fadm_angle_flag(float oh, float ov, float th, float tv)
{
    const float ot_dp = (oh * th) + (ov * tv);
    const float o_mag = (oh * oh) + (ov * ov);
    const float t_mag = (th * th) + (tv * tv);
    const float lhs = ot_dp * ot_dp;
    const float rhs = FADM_COS_1DEG_SQ * (o_mag * t_mag);
    return (ot_dp >= 0.0f) && (lhs >= rhs);
}

static inline float fadm_decouple_r(float o, float t, bool angle_flag, float gain_limit)
{
    float k = t / (o + FADM_EPS);
    k = max(0.0f, min(k, 1.0f));
    float r = k * o;
    if (angle_flag && r > 0.0f) {
        r = min(r * gain_limit, t);
    } else if (angle_flag && r < 0.0f) {
        r = max(r * gain_limit, t);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/*  Stage 2 — Decouple + CSF on decouple_a → csf_a + csf_f.            */
/* ------------------------------------------------------------------ */
kernel void float_adm_decouple_csf(const device float *ref_band [[buffer(0)]],
                                   const device float *dis_band [[buffer(1)]],
                                   device float *csf_a [[buffer(2)]],
                                   device float *csf_f [[buffer(3)]],
                                   constant FadmDims &d [[buffer(4)]],
                                   constant FadmCsf &c [[buffer(5)]],
                                   uint2 gid [[thread_position_in_grid]])
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= d.half_w || gy >= d.half_h) { return; }

    const float oh = fadm_read_band_at(ref_band, 1, gy, gx, d.buf_stride, d.half_h);
    const float ov = fadm_read_band_at(ref_band, 2, gy, gx, d.buf_stride, d.half_h);
    const float od = fadm_read_band_at(ref_band, 3, gy, gx, d.buf_stride, d.half_h);
    const float th = fadm_read_band_at(dis_band, 1, gy, gx, d.buf_stride, d.half_h);
    const float tv = fadm_read_band_at(dis_band, 2, gy, gx, d.buf_stride, d.half_h);
    const float td = fadm_read_band_at(dis_band, 3, gy, gx, d.buf_stride, d.half_h);

    const bool angle_flag = fadm_angle_flag(oh, ov, th, tv);

    const float oarr[3] = {oh, ov, od};
    const float tarr[3] = {th, tv, td};
    const float rfac[3] = {c.rfactor_h, c.rfactor_v, c.rfactor_d};

    for (int b = 0; b < FADM_NUM_BANDS; ++b) {
        const float r = fadm_decouple_r(oarr[b], tarr[b], angle_flag, c.gain_limit);
        const float a_val = tarr[b] - r;
        const float csf_a_val = rfac[b] * a_val;
        fadm_write_csf(csf_a, b, gy, gx, d.buf_stride, d.half_h, csf_a_val);
        fadm_write_csf(csf_f, b, gy, gx, d.buf_stride, d.half_h, FADM_ONE_BY_30 * fabs(csf_a_val));
    }
}

/* ------------------------------------------------------------------ */
/*  Stage 2b — CSF on decouple_r → csf_a_aim + csf_f_aim (AIM pass).   */
/* ------------------------------------------------------------------ */
kernel void float_adm_csf_r(const device float *ref_band [[buffer(0)]],
                            const device float *dis_band [[buffer(1)]],
                            device float *csf_a_out [[buffer(2)]],
                            device float *csf_f_out [[buffer(3)]],
                            constant FadmDims &d [[buffer(4)]],
                            constant FadmCsf &c [[buffer(5)]],
                            uint2 gid [[thread_position_in_grid]])
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= d.half_w || gy >= d.half_h) { return; }

    const float oh = fadm_read_band_at(ref_band, 1, gy, gx, d.buf_stride, d.half_h);
    const float ov = fadm_read_band_at(ref_band, 2, gy, gx, d.buf_stride, d.half_h);
    const float od = fadm_read_band_at(ref_band, 3, gy, gx, d.buf_stride, d.half_h);
    const float th = fadm_read_band_at(dis_band, 1, gy, gx, d.buf_stride, d.half_h);
    const float tv = fadm_read_band_at(dis_band, 2, gy, gx, d.buf_stride, d.half_h);
    const float td = fadm_read_band_at(dis_band, 3, gy, gx, d.buf_stride, d.half_h);

    const bool angle_flag = fadm_angle_flag(oh, ov, th, tv);

    const float oarr[3] = {oh, ov, od};
    const float tarr[3] = {th, tv, td};
    const float rfac[3] = {c.rfactor_h, c.rfactor_v, c.rfactor_d};

    for (int b = 0; b < FADM_NUM_BANDS; ++b) {
        const float r = fadm_decouple_r(oarr[b], tarr[b], angle_flag, c.gain_limit);
        const float csf_a_val = rfac[b] * r;
        fadm_write_csf(csf_a_out, b, gy, gx, d.buf_stride, d.half_h, csf_a_val);
        fadm_write_csf(csf_f_out, b, gy, gx, d.buf_stride, d.half_h,
                       FADM_ONE_BY_30 * fabs(csf_a_val));
    }
}

/* ------------------------------------------------------------------ */
/*  Per-(band,row) reduction helper. The 1D dispatch maps             */
/*  threadgroup → (band, row); the threads in the TG cooperatively     */
/*  stride across active columns. simd_sum + a threadgroup fan-in       */
/*  reduce to one float per output slot (no 64-bit atomics).           */
/* ------------------------------------------------------------------ */
static inline float fadm_tg_reduce(float v, threadgroup float *scratch, uint lid, uint simd_lane,
                                   uint simd_id, uint simd_count)
{
    const float lane = simd_sum(v);
    if (simd_lane == 0) { scratch[simd_id] = lane; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float total = 0.0f;
    if (lid == 0) {
        for (uint i = 0; i < simd_count; ++i) { total += scratch[i]; }
    }
    return total;
}

/* ------------------------------------------------------------------ */
/*  Stage 3 — CSF denominator + CM fused. Writes accum slots [0..5].   */
/*  threadgroup_count.x = 3 * num_active_rows.                          */
/*    band_idx = wg / num_active_rows                                   */
/*    row_idx  = wg % num_active_rows                                   */
/* ------------------------------------------------------------------ */
kernel void float_adm_csf_cm(const device float *ref_band [[buffer(0)]],
                             const device float *dis_band [[buffer(1)]],
                             const device float *csf_a [[buffer(2)]],
                             const device float *csf_f [[buffer(3)]],
                             device float *accum_out [[buffer(8)]],
                             constant FadmDims &d [[buffer(4)]],
                             constant FadmCsf &c [[buffer(5)]],
                             uint wg_id [[threadgroup_position_in_grid]],
                             uint lid [[thread_index_in_threadgroup]],
                             uint tg_size [[threads_per_threadgroup]],
                             uint simd_lane [[thread_index_in_simdgroup]],
                             uint simd_id [[simdgroup_index_in_threadgroup]],
                             uint simd_count [[simdgroups_per_threadgroup]])
{
    const int active_h = c.active_bottom - c.active_top;
    const int active_w = c.active_right - c.active_left;
    if (active_h <= 0 || active_w <= 0) { return; }

    const uint num_rows = (uint)active_h;
    const uint band_idx = wg_id / num_rows;
    const uint row_idx = wg_id - band_idx * num_rows;
    const int row = c.active_top + (int)row_idx;

    const float rfactor_band = (band_idx == 0u) ? c.rfactor_h
                               : (band_idx == 1u) ? c.rfactor_v
                                                  : c.rfactor_d;

    float local_csf_sum = 0.0f;
    float local_cm_sum = 0.0f;

    for (int col = c.active_left + (int)lid; col < c.active_right; col += (int)tg_size) {
        /* CSF denominator: (|rfactor * ref_band[band]|)^3. */
        const float src_ref =
            fadm_read_band_at(ref_band, (int)band_idx + 1, row, col, d.buf_stride, d.half_h);
        const float csf_o = fabs(rfactor_band * src_ref);
        local_csf_sum += csf_o * csf_o * csf_o;

        /* Re-derive decouple_r for the band (cheaper than reading csf_a). */
        const float oh = fadm_read_band_at(ref_band, 1, row, col, d.buf_stride, d.half_h);
        const float ov = fadm_read_band_at(ref_band, 2, row, col, d.buf_stride, d.half_h);
        const float od = fadm_read_band_at(ref_band, 3, row, col, d.buf_stride, d.half_h);
        const float th = fadm_read_band_at(dis_band, 1, row, col, d.buf_stride, d.half_h);
        const float tv = fadm_read_band_at(dis_band, 2, row, col, d.buf_stride, d.half_h);
        const float td = fadm_read_band_at(dis_band, 3, row, col, d.buf_stride, d.half_h);

        const bool angle_flag = fadm_angle_flag(oh, ov, th, tv);
        const float oarr[3] = {oh, ov, od};
        const float tarr[3] = {th, tv, td};
        const float r_val =
            fadm_decouple_r(oarr[band_idx], tarr[band_idx], angle_flag, c.gain_limit);

        /* CM threshold: csf_f 8-neighbours over all 3 bands +
         * (1/15)·|csf_a centre| per band — matches the CPU 3-band aggregate. */
        float thr = 0.0f;
        for (int b = 0; b < FADM_NUM_BANDS; ++b) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) { continue; }
                    thr += fadm_read_csf_f_at(csf_f, b, row + dy, col + dx, d.half_w, d.half_h,
                                              d.buf_stride);
                }
            }
        }
        const float own_h = fadm_read_csf_a_at(csf_a, 0, row, col, d.half_w, d.half_h, d.buf_stride);
        const float own_v = fadm_read_csf_a_at(csf_a, 1, row, col, d.half_w, d.half_h, d.buf_stride);
        const float own_d = fadm_read_csf_a_at(csf_a, 2, row, col, d.half_w, d.half_h, d.buf_stride);
        thr += FADM_ONE_BY_15 * fabs(own_h);
        thr += FADM_ONE_BY_15 * fabs(own_v);
        thr += FADM_ONE_BY_15 * fabs(own_d);

        const float x_val = rfactor_band * r_val;
        float xa = fabs(x_val) - thr;
        if (xa < 0.0f) { xa = 0.0f; }
        local_cm_sum += xa * xa * xa;
    }

    threadgroup float scratch_csf[32];
    threadgroup float scratch_cm[32];
    const float total_csf =
        fadm_tg_reduce(local_csf_sum, scratch_csf, lid, simd_lane, simd_id, simd_count);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const float total_cm =
        fadm_tg_reduce(local_cm_sum, scratch_cm, lid, simd_lane, simd_id, simd_count);

    if (lid == 0) {
        const uint slot_base = wg_id * FADM_ACCUM_SLOTS;
        accum_out[slot_base + band_idx] = total_csf;
        accum_out[slot_base + 3u + band_idx] = total_cm;
    }
}

/* ------------------------------------------------------------------ */
/*  Stage 3b — AIM CM numerator (noise_weight=0). Writes slots [6..8]. */
/*  Masks decouple_a with the decouple_r CSF threshold (csf_*_aim).     */
/* ------------------------------------------------------------------ */
kernel void float_adm_aim_cm(const device float *ref_band [[buffer(0)]],
                             const device float *dis_band [[buffer(1)]],
                             const device float *csf_a_aim [[buffer(2)]],
                             const device float *csf_f_aim [[buffer(3)]],
                             device float *accum_out [[buffer(8)]],
                             constant FadmDims &d [[buffer(4)]],
                             constant FadmCsf &c [[buffer(5)]],
                             uint wg_id [[threadgroup_position_in_grid]],
                             uint lid [[thread_index_in_threadgroup]],
                             uint tg_size [[threads_per_threadgroup]],
                             uint simd_lane [[thread_index_in_simdgroup]],
                             uint simd_id [[simdgroup_index_in_threadgroup]],
                             uint simd_count [[simdgroups_per_threadgroup]])
{
    const int active_h = c.active_bottom - c.active_top;
    const int active_w = c.active_right - c.active_left;
    if (active_h <= 0 || active_w <= 0) { return; }

    const uint num_rows = (uint)active_h;
    const uint band_idx = wg_id / num_rows;
    const uint row_idx = wg_id - band_idx * num_rows;
    const int row = c.active_top + (int)row_idx;

    const float rfactor_band = (band_idx == 0u) ? c.rfactor_h
                               : (band_idx == 1u) ? c.rfactor_v
                                                  : c.rfactor_d;

    float local_aim_cm = 0.0f;

    for (int col = c.active_left + (int)lid; col < c.active_right; col += (int)tg_size) {
        const float oh = fadm_read_band_at(ref_band, 1, row, col, d.buf_stride, d.half_h);
        const float ov = fadm_read_band_at(ref_band, 2, row, col, d.buf_stride, d.half_h);
        const float od = fadm_read_band_at(ref_band, 3, row, col, d.buf_stride, d.half_h);
        const float th = fadm_read_band_at(dis_band, 1, row, col, d.buf_stride, d.half_h);
        const float tv = fadm_read_band_at(dis_band, 2, row, col, d.buf_stride, d.half_h);
        const float td = fadm_read_band_at(dis_band, 3, row, col, d.buf_stride, d.half_h);

        const bool angle_flag = fadm_angle_flag(oh, ov, th, tv);
        const float oarr[3] = {oh, ov, od};
        const float tarr[3] = {th, tv, td};
        const float r_val =
            fadm_decouple_r(oarr[band_idx], tarr[band_idx], angle_flag, c.gain_limit);
        /* decouple_a[band] = t - r (the anomaly component). */
        const float a_val = tarr[band_idx] - r_val;

        float thr = 0.0f;
        for (int b = 0; b < FADM_NUM_BANDS; ++b) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) { continue; }
                    thr += fadm_read_csf_f_at(csf_f_aim, b, row + dy, col + dx, d.half_w, d.half_h,
                                              d.buf_stride);
                }
            }
        }
        const float own_h =
            fadm_read_csf_a_at(csf_a_aim, 0, row, col, d.half_w, d.half_h, d.buf_stride);
        const float own_v =
            fadm_read_csf_a_at(csf_a_aim, 1, row, col, d.half_w, d.half_h, d.buf_stride);
        const float own_d =
            fadm_read_csf_a_at(csf_a_aim, 2, row, col, d.half_w, d.half_h, d.buf_stride);
        thr += FADM_ONE_BY_15 * fabs(own_h);
        thr += FADM_ONE_BY_15 * fabs(own_v);
        thr += FADM_ONE_BY_15 * fabs(own_d);

        const float x_val = rfactor_band * a_val;
        float xa = fabs(x_val) - thr;
        if (xa < 0.0f) { xa = 0.0f; }
        local_aim_cm += xa * xa * xa;
    }

    threadgroup float scratch_aim[32];
    const float total_aim =
        fadm_tg_reduce(local_aim_cm, scratch_aim, lid, simd_lane, simd_id, simd_count);

    if (lid == 0) {
        const uint slot_base = wg_id * FADM_ACCUM_SLOTS;
        accum_out[slot_base + 6u + band_idx] = total_aim;
    }
}
