/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernels for the integer_adm feature extractor (feature
 *  "adm", the VMAF-default ADM path). Integer / fixed-point twin of
 *  core/src/feature/metal/float_adm.metal — it mirrors that proven kernel's
 *  six pipeline stages, 1D per-(band,row) reduction, and 9-slot accumulator
 *  layout verbatim, and swaps the float arithmetic for the bit-exact
 *  fixed-point arithmetic of the CPU reference
 *  core/src/feature/integer_adm.c and the CUDA twin
 *  core/src/feature/cuda/integer_adm/ (adm_dwt2 / adm_decouple_inline /
 *  adm_csf / adm_csf_den / adm_cm .cu).
 *
 *  Six kernel functions, dispatched in order by integer_adm_metal.mm once
 *  per scale (4 scales total). The integer pipeline has two distinct data
 *  representations:
 *    - scale 0   : int16 DWT bands, scale-0 decouple/CSF/CM fixed-point math
 *                  (decouple_r_s0 / adm_csf_kernel / adm_cm_line_kernel).
 *    - scales 1-3: int32 ("i4") DWT bands, i4 fixed-point math
 *                  (decouple_r_s123 / i4_adm_csf_kernel /
 *                   i4_adm_cm_line_kernel_fused).
 *
 *    Stage 0 — integer_adm_dwt_vert_{8,16}bpc
 *        9/7-tap fixed-point DWT vertical pass. At scale 0 reads the raw
 *        u8/u16 source (the only bpc-specific stage); emits int32 lo/hi
 *        sub-rows. The vertical accumulators use the int32 filter taps
 *        {15826,27411,7345,-4240} (lo) / {-4240,-7345,27411,-15826} (hi).
 *        At scales >0 the host swaps to the i4 vert/hori variants below.
 *
 *    Stage 1 — integer_adm_dwt_hori
 *        Fixed-point DWT horizontal pass — reads the lo/hi sub-rows, emits
 *        4 sub-bands (a=LL, h=HL, v=LH, d=HH) into int16 (scale 0) or int32
 *        (scales 1-3) band buffers, with the per-scale (add_shift, shift)
 *        rounding from the CUDA host dispatcher.
 *
 *    Stage 2 — integer_adm_decouple_csf
 *        Inline fixed-point decouple (anomaly) + CSF; writes csf_a (i_rfactor *
 *        a_val, post-shift) + csf_f (FIX_ONE_BY_30 * |csf_a|, post-shift)
 *        for the DLM CM threshold. Matches adm_csf_kernel / i4_adm_csf_kernel.
 *
 *    Stage 3 — integer_adm_csf_cm
 *        Fused CSF denominator (cube of |band|) + CM numerator. 1D dispatch
 *        of (3 * num_active_rows) threadgroups, each reducing one (band,row)
 *        across active columns into the int64 cube accumulator. Writes
 *        accum slots [0..5] as lo/hi uint pairs.
 *
 *    Stage 2b — integer_adm_csf_r
 *        CSF on decouple_r → csf_a_aim + csf_f_aim for the AIM pass.
 *
 *    Stage 3b — integer_adm_aim_cm
 *        AIM CM numerator (noise_weight=0): signal = i_rfactor * a_val,
 *        threshold = csf_r 3x3 neighbourhood. Writes accum slots [6..8].
 *
 *  accum_out layout per (band,row) threadgroup (IADM_ACCUM_SLOTS = 9), each
 *  slot stored as a lo/hi pair of uint32 (no 64-bit MSL atomics):
 *    [0..2] csf_den per band (adm2 denominator, uint64 cube sum)
 *    [3..5] cm_num per band  (adm2 CM numerator, int64 cube sum)
 *    [6..8] aim_cm per band  (AIM CM numerator, int64 cube sum)
 *  Host reduces these to int64 / uint64 and applies the per-scale
 *  conclude_adm_cm / conclude_adm_csf_den float recovery (cube-root pooling),
 *  byte-for-byte with integer_adm_cuda.c.
 *
 *  Numeric design notes:
 *   - No 64-bit atomics in MSL (Apple GPU); each (band,row) threadgroup owns
 *     a unique accum slot, so the cube sums are written directly (no
 *     cross-TG accumulation on device). The host sums the per-row partials
 *     in 64-bit. Per-TG fan-in uses a threadgroup int64 (lo/hi uint) array.
 *   - The fixed-point decouple, get_best15_from32 (__clz emulation via MSL
 *     clz()), and per-band shift tables are load-bearing for places=4 parity;
 *     they replicate the CUDA inline helpers exactly.
 *   - csf_mode 0 (Watson-97) only — the CPU default; other modes rejected in
 *     the .mm init (matches the CUDA twin which only ships mode 0).
 */

#include <metal_stdlib>
using namespace metal;

#define IADM_NUM_BANDS 3
#define IADM_ACCUM_SLOTS 9

/* 9/7 biorthogonal DWT taps in fixed point (Q-scaled), identical to the
 * CUDA host's AdmFixedParametersCuda.dwt2_db2_coeffs_{lo,hi}. */
constant int IADM_LO[4] = {15826, 27411, 7345, -4240};
constant int IADM_HI[4] = {-4240, -7345, 27411, -15826};
constant int IADM_LO_SUM = 46342; /* dwt2_db2_coeffs_lo_sum */

/* Fixed-point reciprocal constant 2^30, mirrors div_Q_factor. */
constant float IADM_DIV_Q_FACTOR = 1073741824.0f; /* 2^30 */
constant float IADM_COS_1DEG_SQ = 0.99969541f;    /* cosf(M_PI/180)^2 */

/* CM threshold fixed-point coefficients (matches integer_adm.c /
 * adm_cm.cu). Scale 0: ONE_BY_15 with >>12; scales 1-3: I4_ONE_BY_15 with
 * >>32 (shift_flt), and FIX_ONE_BY_30 = I4_ONE_BY_15/2 for neighbours. */
constant uint IADM_S0_FIX_ONE_BY_30 = 4369u;      /* (1/30)*2^17, >>12 */
constant uint IADM_S0_ONE_BY_15 = 8738u;          /* (1/15)*2^17, >>12 */
constant uint IADM_I4_FIX_ONE_BY_30 = 143165577u; /* (1/30)*2^32, >>32 */
constant ulong IADM_I4_ONE_BY_15 = 286331153ul;   /* (1/15)*2^32, >>32 */

/* Geometry uniform shared by every stage (mirrors IadmDimsHost in the .mm). */
struct IadmDims {
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
    uint bpc;
    uint _pad0;
};

/* CSF / CM stage uniform: active region + per-band i_rfactor + shifts. */
struct IadmCsf {
    int active_left;
    int active_top;
    int active_right;
    int active_bottom;
    /* i_rfactor[3] for this scale (Q21/Q23 at scale 0, Q32 at scales 1-3). */
    uint i_rfactor_h;
    uint i_rfactor_v;
    uint i_rfactor_d;
    /* enhancement gain limit (float, applied to the integer decouple rst). */
    float gain_limit;
    /* DWT scale-0 raw normalisation: v_shift / v_add_shift / h_shift /
     * h_add_shift packed for the stage-0/1 kernels. */
    int v_shift;
    int v_add_shift;
    int h_shift;
    int h_add_shift;
    /* s123 vert/hori shifts (used at scales 1-3). */
    int s123_vert_add;
    int s123_vert_shift;
    int s123_hori_add;
    int s123_hori_shift;
    /* csf_den per-band square pre-shift (s123 only). */
    int den_shift_sq;
    int den_add_shift_sq;
    /* csf_den inner-accum shift (per (band,row), matches the device atomicAdd
     * shift in adm_csf_den_*_line_kernel). scale 0: shift_accum; scales 1-3:
     * cube shift then row shift folded — see host fill in the .mm. */
    int den_shift_accum;
    int den_add_shift_accum;
    int den_shift_cub;     /* s123 only: per-row cube shift. */
    int den_add_shift_cub; /* s123 only. */
    /* CM cube-accumulation shifts (per band, computed host-side to match
     * integer_adm_cuda.c WarpShift + conclude_adm_cm). */
    int cm_shift_sq[3];
    int cm_add_shift_sq[3];
    int cm_shift_cub[3];
    int cm_add_shift_cub[3];
    int cm_shift_sub[3]; /* scale 0: {10,10,12}; scales 1-3: {0,0,0}. */
    int cm_shift_inner;
    int cm_add_shift_inner;
    uint _pad0;
};

/* Mirror form matches the CUDA calculate_indices() over-range reflection used
 * throughout the integer DWT (negative indices are handled by the n==0
 * special-case in the kernels, mirroring calculate_indices()). */
static inline int iadm_mirror_hi(int idx, int sup)
{
    return (idx >= sup) ? (2 * sup - idx - 1) : idx;
}

/* ------------------------------------------------------------------ */
/*  Fixed-point decouple — scale 0 (int16 bands).                      */
/*  Bit-for-bit replica of decouple_angle_flag_s0 / decouple_r_s0.     */
/* ------------------------------------------------------------------ */
static inline bool iadm_angle_flag_s0(int oh, int ov, int th, int tv)
{
    int ot_dp = oh * th + ov * tv;
    int o_mag_sq = oh * oh + ov * ov;
    int t_mag_sq = th * th + tv * tv;
    return (ot_dp >= 0) &&
           (float((long)ot_dp * ot_dp) >= float((long)o_mag_sq * t_mag_sq) * IADM_COS_1DEG_SQ);
}

static inline int iadm_decouple_r_s0(int o_val, int t_val, bool angle_flag, float gain_limit)
{
    int tmp_k = (o_val == 0) ?
                    32768 :
                    (int)((((long)(int)(IADM_DIV_Q_FACTOR / float(o_val)) * t_val) + 16384) >> 15);
    int k = max(0, min(32768, tmp_k));
    float egl = angle_flag ? gain_limit : 1.0f;

    int rst = (int)((float)(((k * o_val) + 16384) >> 15) * egl);
    int rst_s = k * o_val;
    if (angle_flag) {
        if (rst_s > 0) {
            rst = min(rst, t_val);
        }
        if (rst_s < 0) {
            rst = max(rst, t_val);
        }
    }
    return rst;
}

/* ------------------------------------------------------------------ */
/*  Fixed-point decouple — scales 1-3 (int32 bands).                   */
/*  Replica of get_best15_from32 / decouple_angle_flag_s123 /          */
/*  decouple_r_s123. MSL clz(uint) == CUDA __clz.                      */
/* ------------------------------------------------------------------ */
static inline uint iadm_get_best15_from32(uint temp, thread int *x)
{
    int k = (int)clz(temp);
    k = 17 - k;
    temp = (temp + (1u << (k - 1))) >> k;
    *x = k;
    return temp;
}

static inline bool iadm_angle_flag_s123(int oh, int ov, int th, int tv)
{
    long ot_dp = (long)oh * th + (long)ov * tv;
    long o_mag_sq = (long)oh * oh + (long)ov * ov;
    long t_mag_sq = (long)th * th + (long)tv * tv;
    float d = (float)ot_dp / 4096.0f;
    return (d >= 0.0f) &&
           (d * d >= IADM_COS_1DEG_SQ * ((float)o_mag_sq / 4096.0f) * ((float)t_mag_sq / 4096.0f));
}

static inline int iadm_decouple_r_s123(int o_val, int t_val, bool angle_flag, float gain_limit)
{
    const int div_Q_factor = 1073741824; /* 2^30 */
    int kh_shift = 0;
    uint abs_o = (uint)abs(o_val);
    int sign_o = (o_val < 0) ? -1 : 1;
    int o_msb = (abs_o < 32768u) ? (int)abs_o : (int)iadm_get_best15_from32(abs_o, &kh_shift);

    long tmp_k = (o_val == 0) ?
                     32768 :
                     ((((long)(div_Q_factor / o_msb) * t_val) * sign_o + (1l << (14 + kh_shift))) >>
                      (15 + kh_shift));
    long k = tmp_k < 0 ? 0 : (tmp_k > 32768 ? 32768 : tmp_k);

    float egl = angle_flag ? gain_limit : 1.0f;
    int rst = (int)((float)(((k * o_val) + 16384) >> 15) * egl);
    float rst_f = ((float)k / 32768.0f) * ((float)o_val / 64.0f);
    if (angle_flag && (rst_f > 0.0f)) {
        rst = min(rst, t_val);
    }
    if (angle_flag && (rst_f < 0.0f)) {
        rst = max(rst, t_val);
    }
    return rst;
}

/* ------------------------------------------------------------------ */
/*  Band accessors. Bands packed contiguous: a,h,v,d at slice strides.  */
/*  Scale-0 bands are int16, scales 1-3 bands are int32 — two pointer   */
/*  views are passed to the kernels by the host (only one is active).    */
/* ------------------------------------------------------------------ */
static inline int iadm_read16(const device short *band, int band_idx, int y, int x, int buf_stride,
                              int half_h)
{
    const int slice = buf_stride * half_h;
    return (int)band[band_idx * slice + y * buf_stride + x];
}
static inline int iadm_read32(const device int *band, int band_idx, int y, int x, int buf_stride,
                              int half_h)
{
    const int slice = buf_stride * half_h;
    return band[band_idx * slice + y * buf_stride + x];
}
static inline void iadm_write16(device short *buf, int band_idx, int y, int x, int buf_stride,
                                int half_h, int val)
{
    const int slice = buf_stride * half_h;
    buf[band_idx * slice + y * buf_stride + x] = (short)val;
}
static inline void iadm_write32(device int *buf, int band_idx, int y, int x, int buf_stride,
                                int half_h, int val)
{
    const int slice = buf_stride * half_h;
    buf[band_idx * slice + y * buf_stride + x] = val;
}

/* CM neighbour reads mirror (i±1, j±1) into the active interior the same way
 * the CPU ADM_CM_THRESH_S_* macros / CUDA offset_i/offset_j logic do. */
static inline int iadm_clampx(int x, int w)
{
    if (x < 0) {
        x = -x;
    }
    if (x >= w) {
        x = x - max(0, 2 * (x - w) + 1);
    }
    if (x < 0) {
        x = 0;
    }
    if (x >= w) {
        x = w - 1;
    }
    return x;
}

/* ------------------------------------------------------------------ */
/*  Stage 0 — DWT vertical pass (scale 0, 8-bpc + 16-bpc raw read).     */
/*  Emits int32 lo/hi sub-rows in dwt_tmp (layout: row*2*cur_w packed). */
/* ------------------------------------------------------------------ */
static void iadm_dwt_vert_s0_impl(const device uchar *ref_raw_u8, const device ushort *ref_raw_u16,
                                  const device uchar *dis_raw_u8, const device ushort *dis_raw_u16,
                                  device int *dwt_tmp_ref, device int *dwt_tmp_dis,
                                  constant IadmDims &d, constant IadmCsf &c, uint3 gid, bool is16)
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    const int plane_is_dis = (int)gid.z;
    if (gx >= d.cur_w || gy >= d.half_h) {
        return;
    }

    /* CUDA calculate_indices(): rows (2n-1..2n+2), n==0 special-cased. */
    int4 idx;
    if (gy == 0) {
        idx = int4(1, 0, 1, 2);
    } else {
        idx = int4(2 * gy - 1, 2 * gy, 2 * gy + 1, 2 * gy + 2);
    }
    idx.x = iadm_mirror_hi(idx.x, d.cur_h);
    idx.y = iadm_mirror_hi(idx.y, d.cur_h);
    idx.z = iadm_mirror_hi(idx.z, d.cur_h);
    idx.w = iadm_mirror_hi(idx.w, d.cur_h);

    const int raw_stride = d.cur_w;
    int s[4];
    int yy[4] = {idx.x, idx.y, idx.z, idx.w};
    for (int k = 0; k < 4; ++k) {
        if (is16) {
            const device ushort *plane = (plane_is_dis == 0) ? ref_raw_u16 : dis_raw_u16;
            s[k] = (int)plane[yy[k] * raw_stride + gx];
        } else {
            const device uchar *plane = (plane_is_dis == 0) ? ref_raw_u8 : dis_raw_u8;
            s[k] = (int)plane[yy[k] * raw_stride + gx];
        }
    }

    int accum_lo = 0, accum_hi = 0;
    for (int k = 0; k < 4; ++k) {
        accum_lo += IADM_LO[k] * s[k];
        accum_hi += IADM_HI[k] * s[k];
    }
    /* normalise range (0..N) -> (-N/2..N/2): subtract coeff_sum * v_add_shift. */
    accum_lo -= IADM_LO_SUM * c.v_add_shift;
    accum_hi -= 0 * c.v_add_shift; /* hi_sum == 0 */

    const int out_stride = d.cur_w * 2;
    device int *dst = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    dst[gy * out_stride + gx] = (accum_lo + c.v_add_shift) >> c.v_shift;
    dst[gy * out_stride + d.cur_w + gx] = (accum_hi + c.v_add_shift) >> c.v_shift;
}

kernel void integer_adm_dwt_vert_8bpc(const device uchar *ref_raw [[buffer(0)]],
                                      const device uchar *dis_raw [[buffer(1)]],
                                      device int *dwt_tmp_ref [[buffer(2)]],
                                      device int *dwt_tmp_dis [[buffer(3)]],
                                      constant IadmDims &d [[buffer(4)]],
                                      constant IadmCsf &c [[buffer(5)]],
                                      uint3 gid [[thread_position_in_grid]])
{
    iadm_dwt_vert_s0_impl(ref_raw, (const device ushort *)0, dis_raw, (const device ushort *)0,
                          dwt_tmp_ref, dwt_tmp_dis, d, c, gid, false);
}

kernel void integer_adm_dwt_vert_16bpc(const device ushort *ref_raw [[buffer(0)]],
                                       const device ushort *dis_raw [[buffer(1)]],
                                       device int *dwt_tmp_ref [[buffer(2)]],
                                       device int *dwt_tmp_dis [[buffer(3)]],
                                       constant IadmDims &d [[buffer(4)]],
                                       constant IadmCsf &c [[buffer(5)]],
                                       uint3 gid [[thread_position_in_grid]])
{
    iadm_dwt_vert_s0_impl((const device uchar *)0, ref_raw, (const device uchar *)0, dis_raw,
                          dwt_tmp_ref, dwt_tmp_dis, d, c, gid, true);
}

/* ------------------------------------------------------------------ */
/*  Stage 0' — DWT vertical pass (scales 1-3, int32 parent LL band).   */
/*  Reads parent band_a; emits int32 lo/hi sub-rows. The s123 vertical  */
/*  rounding (add_shift, shift) is supplied per scale via IadmCsf.       */
/* ------------------------------------------------------------------ */
kernel void integer_adm_dwt_vert_s123(const device int *parent_ref_band [[buffer(6)]],
                                      const device int *parent_dis_band [[buffer(7)]],
                                      device int *dwt_tmp_ref [[buffer(2)]],
                                      device int *dwt_tmp_dis [[buffer(3)]],
                                      constant IadmDims &d [[buffer(4)]],
                                      constant IadmCsf &c [[buffer(5)]],
                                      uint3 gid [[thread_position_in_grid]])
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    const int plane_is_dis = (int)gid.z;
    if (gx >= d.cur_w || gy >= d.half_h) {
        return;
    }

    int4 idx;
    if (gy == 0) {
        idx = int4(1, 0, 1, 2);
    } else {
        idx = int4(2 * gy - 1, 2 * gy, 2 * gy + 1, 2 * gy + 2);
    }
    idx.x = iadm_mirror_hi(idx.x, d.cur_h);
    idx.y = iadm_mirror_hi(idx.y, d.cur_h);
    idx.z = iadm_mirror_hi(idx.z, d.cur_h);
    idx.w = iadm_mirror_hi(idx.w, d.cur_h);

    /* Parent band_a is stored at band 0 in the parent band buffer. */
    const device int *band = (plane_is_dis == 0) ? parent_ref_band : parent_dis_band;
    const int pstride = d.parent_buf_stride;
    int yy[4] = {idx.x, idx.y, idx.z, idx.w};
    long accum_lo = 0, accum_hi = 0;
    for (int k = 0; k < 4; ++k) {
        int v = band[yy[k] * pstride + gx];
        accum_lo += (long)IADM_LO[k] * v;
        accum_hi += (long)IADM_HI[k] * v;
    }

    const int out_stride = d.cur_w * 2;
    device int *dst = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    dst[gy * out_stride + gx] = (int)((accum_lo + c.s123_vert_add) >> c.s123_vert_shift);
    dst[gy * out_stride + d.cur_w + gx] = (int)((accum_hi + c.s123_vert_add) >> c.s123_vert_shift);
}

/* ------------------------------------------------------------------ */
/*  Stage 1 — DWT horizontal pass (scale 0 -> int16, s123 -> int32).   */
/*  Emits 4 bands a/h/v/d. Matches adm_dwt2_8_vert_hori_kernel (h pass) */
/*  and dwt_s123_combined_hori_kernel band ordering.                    */
/* ------------------------------------------------------------------ */
static inline int iadm_read_dwt_tmp(const device int *dwt_tmp, int gy, int x_sub, int cur_w,
                                    int half_offset)
{
    /* calculate_indices over-range mirror (x_sub already mirror_lo'd by caller). */
    x_sub = iadm_mirror_hi(x_sub, cur_w);
    const int stride = cur_w * 2;
    return dwt_tmp[gy * stride + half_offset + x_sub];
}

static void iadm_dwt_hori_impl(const device int *dwt_tmp_ref, const device int *dwt_tmp_dis,
                               device short *ref_band16, device short *dis_band16,
                               device int *ref_band32, device int *dis_band32, constant IadmDims &d,
                               constant IadmCsf &c, uint3 gid, bool is_s0)
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    const int plane_is_dis = (int)gid.z;
    if (gx >= d.half_w || gy >= d.half_h) {
        return;
    }

    const device int *src = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    const int cur_w = d.cur_w;

    /* calculate_indices(gx, cur_w): (2gx-1..2gx+2), gx==0 special. */
    int4 px;
    if (gx == 0) {
        px = int4(1, 0, 1, 2);
    } else {
        px = int4(2 * gx - 1, 2 * gx, 2 * gx + 1, 2 * gx + 2);
    }
    int xs[4] = {px.x, px.y, px.z, px.w};

    int sl[4], sh[4];
    for (int k = 0; k < 4; ++k) {
        sl[k] = iadm_read_dwt_tmp(src, gy, xs[k], cur_w, 0);
        sh[k] = iadm_read_dwt_tmp(src, gy, xs[k], cur_w, cur_w);
    }

    const int add = is_s0 ? c.h_add_shift : c.s123_hori_add;
    const int shift = is_s0 ? c.h_shift : c.s123_hori_shift;

    long a = 0, v = 0, h = 0, dd = 0;
    for (int k = 0; k < 4; ++k) {
        a += (long)IADM_LO[k] * sl[k];
        v += (long)IADM_HI[k] * sl[k];
        h += (long)IADM_LO[k] * sh[k];
        dd += (long)IADM_HI[k] * sh[k];
    }
    int a_val = (int)((a + add) >> shift);
    int v_val = (int)((v + add) >> shift);
    int h_val = (int)((h + add) >> shift);
    int d_val = (int)((dd + add) >> shift);

    if (is_s0) {
        device short *dst = (plane_is_dis == 0) ? ref_band16 : dis_band16;
        iadm_write16(dst, 0, gy, gx, d.buf_stride, d.half_h, a_val);
        iadm_write16(dst, 1, gy, gx, d.buf_stride, d.half_h, h_val);
        iadm_write16(dst, 2, gy, gx, d.buf_stride, d.half_h, v_val);
        iadm_write16(dst, 3, gy, gx, d.buf_stride, d.half_h, d_val);
    } else {
        device int *dst = (plane_is_dis == 0) ? ref_band32 : dis_band32;
        iadm_write32(dst, 0, gy, gx, d.buf_stride, d.half_h, a_val);
        iadm_write32(dst, 1, gy, gx, d.buf_stride, d.half_h, h_val);
        iadm_write32(dst, 2, gy, gx, d.buf_stride, d.half_h, v_val);
        iadm_write32(dst, 3, gy, gx, d.buf_stride, d.half_h, d_val);
    }
}

kernel void integer_adm_dwt_hori_s0(const device int *dwt_tmp_ref [[buffer(0)]],
                                    const device int *dwt_tmp_dis [[buffer(1)]],
                                    device short *ref_band [[buffer(2)]],
                                    device short *dis_band [[buffer(3)]],
                                    constant IadmDims &d [[buffer(4)]],
                                    constant IadmCsf &c [[buffer(5)]],
                                    uint3 gid [[thread_position_in_grid]])
{
    iadm_dwt_hori_impl(dwt_tmp_ref, dwt_tmp_dis, ref_band, dis_band, (device int *)0,
                       (device int *)0, d, c, gid, true);
}

kernel void integer_adm_dwt_hori_s123(const device int *dwt_tmp_ref [[buffer(0)]],
                                      const device int *dwt_tmp_dis [[buffer(1)]],
                                      device int *ref_band [[buffer(2)]],
                                      device int *dis_band [[buffer(3)]],
                                      constant IadmDims &d [[buffer(4)]],
                                      constant IadmCsf &c [[buffer(5)]],
                                      uint3 gid [[thread_position_in_grid]])
{
    iadm_dwt_hori_impl(dwt_tmp_ref, dwt_tmp_dis, (device short *)0, (device short *)0, ref_band,
                       dis_band, d, c, gid, false);
}

/* ------------------------------------------------------------------ */
/*  Inline csf_a / csf_r helpers — scale 0 (int16). Mirror             */
/*  inline_s0_csf_a / inline_s0_csf_r in adm_cm.cu.                     */
/* ------------------------------------------------------------------ */
constant uint IADM_S0_SHIFTS[4] = {0u, 15u, 15u, 17u};
constant uint IADM_S0_SHIFTADD[4] = {0u, 16384u, 16384u, 65535u};

static inline int iadm_s0_band_vals(const device short *ref, const device short *dis, int y, int x,
                                    int buf_stride, int half_h, int theta, bool use_r,
                                    constant IadmCsf &c, thread int *out_a)
{
    int oh = iadm_read16(ref, 1, y, x, buf_stride, half_h);
    int ov = iadm_read16(ref, 2, y, x, buf_stride, half_h);
    int od = iadm_read16(ref, 3, y, x, buf_stride, half_h);
    int th = iadm_read16(dis, 1, y, x, buf_stride, half_h);
    int tv = iadm_read16(dis, 2, y, x, buf_stride, half_h);
    int td = iadm_read16(dis, 3, y, x, buf_stride, half_h);

    bool af = iadm_angle_flag_s0(oh, ov, th, tv);
    int o_val = (theta == 0) ? oh : (theta == 1) ? ov : od;
    int t_val = (theta == 0) ? th : (theta == 1) ? tv : td;
    int r_val = iadm_decouple_r_s0(o_val, t_val, af, c.gain_limit);

    uint irf = (theta == 0) ? c.i_rfactor_h : (theta == 1) ? c.i_rfactor_v : c.i_rfactor_d;
    int band = theta + 1;
    int src = use_r ? r_val : (t_val - r_val);
    int dst_val = (int)(irf * (uint)src);
    int csf = (dst_val + (int)IADM_S0_SHIFTADD[band]) >> IADM_S0_SHIFTS[band];
    *out_a = (t_val - r_val);
    return csf;
}

/* ------------------------------------------------------------------ */
/*  Inline csf_a / csf_r helpers — scales 1-3 (int32). Mirror          */
/*  inline_i4_csf_a / inline_i4_csf_r in adm_cm.cu.                     */
/* ------------------------------------------------------------------ */
static inline int iadm_i4_band_vals(const device int *ref, const device int *dis, int y, int x,
                                    int buf_stride, int half_h, int theta, bool use_r,
                                    constant IadmCsf &c, thread int *out_a)
{
    const uint shift_dst = 28u;
    const int add_bef_shift_dst = (int)(1u << (shift_dst - 1u));

    int oh = iadm_read32(ref, 1, y, x, buf_stride, half_h);
    int ov = iadm_read32(ref, 2, y, x, buf_stride, half_h);
    int od = iadm_read32(ref, 3, y, x, buf_stride, half_h);
    int th = iadm_read32(dis, 1, y, x, buf_stride, half_h);
    int tv = iadm_read32(dis, 2, y, x, buf_stride, half_h);
    int td = iadm_read32(dis, 3, y, x, buf_stride, half_h);

    bool af = iadm_angle_flag_s123(oh, ov, th, tv);
    int o_val = (theta == 0) ? oh : (theta == 1) ? ov : od;
    int t_val = (theta == 0) ? th : (theta == 1) ? tv : td;
    int r_val = iadm_decouple_r_s123(o_val, t_val, af, c.gain_limit);

    uint irf = (theta == 0) ? c.i_rfactor_h : (theta == 1) ? c.i_rfactor_v : c.i_rfactor_d;
    int src = use_r ? r_val : (t_val - r_val);
    int csf = (int)(((irf * (long)src) + add_bef_shift_dst) >> shift_dst);
    *out_a = (t_val - r_val);
    return csf;
}

/* ------------------------------------------------------------------ */
/*  Stage 2 / 2b — Decouple + CSF: writes csf_a + csf_f (or AIM r).     */
/*  Scale 0 (int16) variant.                                           */
/* ------------------------------------------------------------------ */
static void iadm_decouple_csf_s0(const device short *ref_band, const device short *dis_band,
                                 device short *csf_a, device short *csf_f, constant IadmDims &d,
                                 constant IadmCsf &c, uint2 gid, bool use_r)
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= d.half_w || gy >= d.half_h) {
        return;
    }
    for (int b = 0; b < IADM_NUM_BANDS; ++b) {
        int a_dummy;
        int csf = iadm_s0_band_vals(ref_band, dis_band, gy, gx, d.buf_stride, d.half_h, b, use_r, c,
                                    &a_dummy);
        iadm_write16(csf_a, b, gy, gx, d.buf_stride, d.half_h, csf);
        int flt = (int)(((IADM_S0_FIX_ONE_BY_30 * (uint)abs(csf)) + 2048u) >> 12);
        iadm_write16(csf_f, b, gy, gx, d.buf_stride, d.half_h, flt);
    }
}

kernel void integer_adm_decouple_csf_s0(const device short *ref_band [[buffer(0)]],
                                        const device short *dis_band [[buffer(1)]],
                                        device short *csf_a [[buffer(2)]],
                                        device short *csf_f [[buffer(3)]],
                                        constant IadmDims &d [[buffer(4)]],
                                        constant IadmCsf &c [[buffer(5)]],
                                        uint2 gid [[thread_position_in_grid]])
{
    iadm_decouple_csf_s0(ref_band, dis_band, csf_a, csf_f, d, c, gid, false);
}

kernel void
integer_adm_csf_r_s0(const device short *ref_band [[buffer(0)]],
                     const device short *dis_band [[buffer(1)]], device short *csf_a [[buffer(2)]],
                     device short *csf_f [[buffer(3)]], constant IadmDims &d [[buffer(4)]],
                     constant IadmCsf &c [[buffer(5)]], uint2 gid [[thread_position_in_grid]])
{
    iadm_decouple_csf_s0(ref_band, dis_band, csf_a, csf_f, d, c, gid, true);
}

/* Scales 1-3 (int32) decouple + CSF. csf_f uses FIX_ONE_BY_30 >> 32. */
static void iadm_decouple_csf_s123(const device int *ref_band, const device int *dis_band,
                                   device int *csf_a, device int *csf_f, constant IadmDims &d,
                                   constant IadmCsf &c, uint2 gid, bool use_r)
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= d.half_w || gy >= d.half_h) {
        return;
    }
    const uint shift_flt = 32u;
    const long add_bef_shift_flt = (long)(1u << (shift_flt - 1u));
    for (int b = 0; b < IADM_NUM_BANDS; ++b) {
        int a_dummy;
        int csf = iadm_i4_band_vals(ref_band, dis_band, gy, gx, d.buf_stride, d.half_h, b, use_r, c,
                                    &a_dummy);
        iadm_write32(csf_a, b, gy, gx, d.buf_stride, d.half_h, csf);
        int flt =
            (int)((((long)IADM_I4_FIX_ONE_BY_30 * abs(csf)) + add_bef_shift_flt) >> shift_flt);
        iadm_write32(csf_f, b, gy, gx, d.buf_stride, d.half_h, flt);
    }
}

kernel void integer_adm_decouple_csf_s123(const device int *ref_band [[buffer(0)]],
                                          const device int *dis_band [[buffer(1)]],
                                          device int *csf_a [[buffer(2)]],
                                          device int *csf_f [[buffer(3)]],
                                          constant IadmDims &d [[buffer(4)]],
                                          constant IadmCsf &c [[buffer(5)]],
                                          uint2 gid [[thread_position_in_grid]])
{
    iadm_decouple_csf_s123(ref_band, dis_band, csf_a, csf_f, d, c, gid, false);
}

kernel void integer_adm_csf_r_s123(const device int *ref_band [[buffer(0)]],
                                   const device int *dis_band [[buffer(1)]],
                                   device int *csf_a [[buffer(2)]], device int *csf_f [[buffer(3)]],
                                   constant IadmDims &d [[buffer(4)]],
                                   constant IadmCsf &c [[buffer(5)]],
                                   uint2 gid [[thread_position_in_grid]])
{
    iadm_decouple_csf_s123(ref_band, dis_band, csf_a, csf_f, d, c, gid, true);
}

/* ------------------------------------------------------------------ */
/*  Per-(band,row) int64 cube reduction. accum_out stores lo/hi uint     */
/*  pairs (no 64-bit MSL atomics). threadgroup fan-in via 2x uint arrays. */
/* ------------------------------------------------------------------ */
static inline ulong iadm_tg_reduce_u64(ulong v, threadgroup atomic_uint *scratch_lo,
                                       threadgroup atomic_uint *scratch_hi, uint lid, uint tg_size)
{
    /* Simple shared-memory tree reduction using two uint atomics per slot;
     * MSL has no 64-bit atomics so we split the running sum. Because each
     * (band,row) threadgroup writes one unique output slot, the host needs
     * only the final per-TG total — computed here by thread 0 after a
     * barrier-protected accumulation in shared memory. */
    /* Lane partials are accumulated into a single shared int64 via atomic add
     * on the lo word with manual carry into hi. */
    uint lo = (uint)(v & 0xFFFFFFFFul);
    uint hi = (uint)(v >> 32);
    uint prev_lo = atomic_fetch_add_explicit(scratch_lo, lo, memory_order_relaxed);
    uint carry = (prev_lo + lo < prev_lo) ? 1u : 0u;
    atomic_fetch_add_explicit(scratch_hi, hi + carry, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid != 0) {
        return 0;
    }
    uint sum_lo = atomic_load_explicit(scratch_lo, memory_order_relaxed);
    uint sum_hi = atomic_load_explicit(scratch_hi, memory_order_relaxed);
    return ((ulong)sum_hi << 32) | (ulong)sum_lo;
}

/* Per-pixel CM cube contribution with the per-band square + cube shifts —
 * mirrors the WarpShift x_sq / lane_accum arithmetic in adm_cm.cu. The
 * host applies only the inner-accum shift after summing rows (matching
 * conclude_adm_cm's expectation that accum_global already carries x_sq/cube
 * shifts but NOT the inner-accum shift, which integer_adm_cuda.c applies in
 * the atomicAdd path; here we keep the inner-accum shift host-side per row to
 * keep the cross-TG sum lossless). */
static inline ulong iadm_cm_cube(long accum_thread, int shift_sq, long add_shift_sq, int shift_cub,
                                 long add_shift_cub)
{
    long x_sq = ((accum_thread * accum_thread) + add_shift_sq) >> shift_sq;
    long cube = ((x_sq * accum_thread) + add_shift_cub) >> shift_cub;
    return (ulong)cube;
}

/* ------------------------------------------------------------------ */
/*  Stage 3 — CSF denominator + DLM CM fused. Writes accum slots [0..5] */
/*  threadgroup_count.x = 3 * num_active_rows.                          */
/*    band_idx = wg / num_active_rows; row_idx = wg % num_active_rows.   */
/*  Scale-0 (int16) variant. accum_out is uint (lo/hi pairs per slot).  */
/* ------------------------------------------------------------------ */
static void iadm_csf_cm_s0(const device short *ref_band, const device short *dis_band,
                           const device short *csf_f, device uint *accum_out, constant IadmDims &d,
                           constant IadmCsf &c, uint wg_id, uint lid, uint tg_size,
                           threadgroup atomic_uint *s_csf_lo, threadgroup atomic_uint *s_csf_hi,
                           threadgroup atomic_uint *s_cm_lo, threadgroup atomic_uint *s_cm_hi)
{
    const int active_h = c.active_bottom - c.active_top;
    const int active_w = c.active_right - c.active_left;
    if (active_h <= 0 || active_w <= 0) {
        return;
    }

    if (lid == 0) {
        atomic_store_explicit(s_csf_lo, 0u, memory_order_relaxed);
        atomic_store_explicit(s_csf_hi, 0u, memory_order_relaxed);
        atomic_store_explicit(s_cm_lo, 0u, memory_order_relaxed);
        atomic_store_explicit(s_cm_hi, 0u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint num_rows = (uint)active_h;
    const uint band_idx = wg_id / num_rows;
    const uint row_idx = wg_id - band_idx * num_rows;
    const int row = c.active_top + (int)row_idx;
    const int w = d.half_w;
    const int h = d.half_h;

    uint irf = (band_idx == 0u) ? c.i_rfactor_h : (band_idx == 1u) ? c.i_rfactor_v : c.i_rfactor_d;

    const int shift_sub = c.cm_shift_sub[band_idx];
    const int shift_sq = c.cm_shift_sq[band_idx];
    const long add_shift_sq = (long)c.cm_add_shift_sq[band_idx];
    const int shift_cub = c.cm_shift_cub[band_idx];
    const long add_shift_cub = (long)c.cm_add_shift_cub[band_idx];

    ulong local_csf = 0ul;
    ulong local_cm = 0ul;

    for (int col = c.active_left + (int)lid; col < c.active_right; col += (int)tg_size) {
        /* CSF denominator: cube of |band|. Scale 0 uses uint16 band cubes. */
        int src_ref = iadm_read16(ref_band, (int)band_idx + 1, row, col, d.buf_stride, d.half_h);
        uint t = (uint)abs(src_ref);
        local_csf += ((ulong)t * t) * t;

        /* CM threshold: 3x3 csf_f neighbourhood over all theta +
         * center ONE_BY_15 * |csf_a center|. Mirror neighbours into interior. */
        int thr = 0;
        for (int theta = 0; theta < IADM_NUM_BANDS; ++theta) {
            int a_center;
            int csf_a_center = iadm_s0_band_vals(ref_band, dis_band, row, col, d.buf_stride,
                                                 d.half_h, theta, false, c, &a_center);
            (void)a_center;
            int sum = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ry = iadm_clampx(row + dy, h);
                for (int dx = -1; dx <= 1; ++dx) {
                    int rx = iadm_clampx(col + dx, w);
                    if (dx == 0 && dy == 0) {
                        sum += (int)(((IADM_S0_ONE_BY_15 * (uint)abs(csf_a_center)) + 2048u) >> 12);
                    } else {
                        sum += iadm_read16(csf_f, theta, ry, rx, d.buf_stride, d.half_h);
                    }
                }
            }
            thr += sum;
        }

        /* CM signal: scale 0 uses abs(i_rfactor * decouple_r) (NOT decouple_a),
         * with the threshold left-shifted by shift_sub (= {10,10,12}) — mirrors
         * adm_cm_line_kernel (signal = inline_s0_decouple_r). */
        int oh = iadm_read16(ref_band, 1, row, col, d.buf_stride, d.half_h);
        int ov = iadm_read16(ref_band, 2, row, col, d.buf_stride, d.half_h);
        int od = iadm_read16(ref_band, 3, row, col, d.buf_stride, d.half_h);
        int th = iadm_read16(dis_band, 1, row, col, d.buf_stride, d.half_h);
        int tv = iadm_read16(dis_band, 2, row, col, d.buf_stride, d.half_h);
        int td = iadm_read16(dis_band, 3, row, col, d.buf_stride, d.half_h);
        bool af = iadm_angle_flag_s0(oh, ov, th, tv);
        int o_val = ((int)band_idx == 0) ? oh : ((int)band_idx == 1) ? ov : od;
        int t_val = ((int)band_idx == 0) ? th : ((int)band_idx == 1) ? tv : td;
        (void)td;
        int decouple_r = iadm_decouple_r_s0(o_val, t_val, af, c.gain_limit);
        int x = abs((int)(irf * (uint)decouple_r)) - (thr << shift_sub);
        if (x < 0) {
            x = 0;
        }
        local_cm += iadm_cm_cube((long)x, shift_sq, add_shift_sq, shift_cub, add_shift_cub);
    }

    ulong total_csf = iadm_tg_reduce_u64(local_csf, s_csf_lo, s_csf_hi, lid, tg_size);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    ulong total_cm = iadm_tg_reduce_u64(local_cm, s_cm_lo, s_cm_hi, lid, tg_size);

    if (lid == 0) {
        /* inner-accum shift applied per (band,row) to match accum_global. */
        ulong cm_out = (total_cm + (ulong)c.cm_add_shift_inner) >> (uint)c.cm_shift_inner;
        ulong csf_out = (total_csf + (ulong)c.den_add_shift_accum) >> (uint)c.den_shift_accum;
        const uint slot = wg_id * (uint)IADM_ACCUM_SLOTS;
        const uint base = slot * 2u;
        accum_out[(base + band_idx) * 2u + 0u] = (uint)(csf_out & 0xFFFFFFFFul);
        accum_out[(base + band_idx) * 2u + 1u] = (uint)(csf_out >> 32);
        accum_out[(base + 3u + band_idx) * 2u + 0u] = (uint)(cm_out & 0xFFFFFFFFul);
        accum_out[(base + 3u + band_idx) * 2u + 1u] = (uint)(cm_out >> 32);
    }
}

kernel void integer_adm_csf_cm_s0(
    const device short *ref_band [[buffer(0)]], const device short *dis_band [[buffer(1)]],
    const device short *csf_f [[buffer(3)]], device uint *accum_out [[buffer(8)]],
    constant IadmDims &d [[buffer(4)]], constant IadmCsf &c [[buffer(5)]],
    uint wg_id [[threadgroup_position_in_grid]], uint lid [[thread_index_in_threadgroup]],
    uint tg_size [[threads_per_threadgroup]])
{
    threadgroup atomic_uint s_csf_lo, s_csf_hi, s_cm_lo, s_cm_hi;
    iadm_csf_cm_s0(ref_band, dis_band, csf_f, accum_out, d, c, wg_id, lid, tg_size, &s_csf_lo,
                   &s_csf_hi, &s_cm_lo, &s_cm_hi);
}

/* Scales 1-3 (int32) CSF denom + DLM CM. CSF denom uses the s123 cube
 * shifts (square pre-shift then cube); accum slots [0..5]. */
static void iadm_csf_cm_s123(const device int *ref_band, const device int *dis_band,
                             const device int *csf_f, device uint *accum_out, constant IadmDims &d,
                             constant IadmCsf &c, uint wg_id, uint lid, uint tg_size,
                             threadgroup atomic_uint *s_csf_lo, threadgroup atomic_uint *s_csf_hi,
                             threadgroup atomic_uint *s_cm_lo, threadgroup atomic_uint *s_cm_hi)
{
    const int active_h = c.active_bottom - c.active_top;
    const int active_w = c.active_right - c.active_left;
    if (active_h <= 0 || active_w <= 0) {
        return;
    }

    if (lid == 0) {
        atomic_store_explicit(s_csf_lo, 0u, memory_order_relaxed);
        atomic_store_explicit(s_csf_hi, 0u, memory_order_relaxed);
        atomic_store_explicit(s_cm_lo, 0u, memory_order_relaxed);
        atomic_store_explicit(s_cm_hi, 0u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint num_rows = (uint)active_h;
    const uint band_idx = wg_id / num_rows;
    const uint row_idx = wg_id - band_idx * num_rows;
    const int row = c.active_top + (int)row_idx;
    const int w = d.half_w;
    const int h = d.half_h;

    uint irf = (band_idx == 0u) ? c.i_rfactor_h : (band_idx == 1u) ? c.i_rfactor_v : c.i_rfactor_d;

    const uint den_shift_sq = (uint)c.den_shift_sq;
    const ulong den_add_shift_sq = (ulong)c.den_add_shift_sq;
    const uint shift_dst = 28u;
    const long add_bef_shift_dst = (long)(1u << (shift_dst - 1u));
    const uint shift_flt = 32u;
    const long add_bef_shift_flt = (long)(1u << (shift_flt - 1u));

    const int cm_shift_sq = c.cm_shift_sq[band_idx];
    const long cm_add_shift_sq = (long)c.cm_add_shift_sq[band_idx];
    const int cm_shift_cub = c.cm_shift_cub[band_idx];
    const long cm_add_shift_cub = (long)c.cm_add_shift_cub[band_idx];

    ulong local_csf = 0ul;
    ulong local_cm = 0ul;

    for (int col = c.active_left + (int)lid; col < c.active_right; col += (int)tg_size) {
        /* CSF denominator s123: ((t*t + add)>>shift)*t, then per-row cube
         * shift; the inner-accum (row) shift is applied at store time. */
        uint t =
            (uint)abs(iadm_read32(ref_band, (int)band_idx + 1, row, col, d.buf_stride, d.half_h));
        local_csf += ((((((ulong)t * t) + den_add_shift_sq) >> den_shift_sq) * t) +
                      (ulong)c.den_add_shift_cub) >>
                     (uint)c.den_shift_cub;

        /* Threshold uses csf_a (decouple_a) center + csf_f neighbours. */
        int thr = 0;
        for (int theta = 0; theta < IADM_NUM_BANDS; ++theta) {
            int a_center;
            (void)iadm_i4_band_vals(ref_band, dis_band, row, col, d.buf_stride, d.half_h, theta,
                                    false, c, &a_center);
            uint irf_t = (theta == 0) ? c.i_rfactor_h :
                         (theta == 1) ? c.i_rfactor_v :
                                        c.i_rfactor_d;
            int csf_a_center = (int)(((irf_t * (long)a_center) + add_bef_shift_dst) >> shift_dst);
            int sum = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ry = iadm_clampx(row + dy, h);
                for (int dx = -1; dx <= 1; ++dx) {
                    int rx = iadm_clampx(col + dx, w);
                    if (dx == 0 && dy == 0) {
                        sum += (int)((((long)IADM_I4_ONE_BY_15 * abs(csf_a_center)) +
                                      add_bef_shift_flt) >>
                                     shift_flt);
                    } else {
                        sum += iadm_read32(csf_f, theta, ry, rx, d.buf_stride, d.half_h);
                    }
                }
            }
            thr += sum;
        }

        /* Signal: csf of decouple_r (NOT decouple_a) — matches
         * i4_adm_cm_line_kernel_fused. shift_sub = 0 for scales 1-3. */
        int r_dummy;
        int csf_r = iadm_i4_band_vals(ref_band, dis_band, row, col, d.buf_stride, d.half_h,
                                      (int)band_idx, true, c, &r_dummy);
        int x = abs(csf_r) - thr;
        if (x < 0) {
            x = 0;
        }
        local_cm +=
            iadm_cm_cube((long)x, cm_shift_sq, cm_add_shift_sq, cm_shift_cub, cm_add_shift_cub);
    }

    ulong total_csf = iadm_tg_reduce_u64(local_csf, s_csf_lo, s_csf_hi, lid, tg_size);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    ulong total_cm = iadm_tg_reduce_u64(local_cm, s_cm_lo, s_cm_hi, lid, tg_size);

    if (lid == 0) {
        ulong cm_out = (total_cm + (ulong)c.cm_add_shift_inner) >> (uint)c.cm_shift_inner;
        ulong csf_out = (total_csf + (ulong)c.den_add_shift_accum) >> (uint)c.den_shift_accum;
        const uint slot = wg_id * (uint)IADM_ACCUM_SLOTS;
        const uint base = slot * 2u;
        accum_out[(base + band_idx) * 2u + 0u] = (uint)(csf_out & 0xFFFFFFFFul);
        accum_out[(base + band_idx) * 2u + 1u] = (uint)(csf_out >> 32);
        accum_out[(base + 3u + band_idx) * 2u + 0u] = (uint)(cm_out & 0xFFFFFFFFul);
        accum_out[(base + 3u + band_idx) * 2u + 1u] = (uint)(cm_out >> 32);
    }
}

kernel void integer_adm_csf_cm_s123(
    const device int *ref_band [[buffer(0)]], const device int *dis_band [[buffer(1)]],
    const device int *csf_f [[buffer(3)]], device uint *accum_out [[buffer(8)]],
    constant IadmDims &d [[buffer(4)]], constant IadmCsf &c [[buffer(5)]],
    uint wg_id [[threadgroup_position_in_grid]], uint lid [[thread_index_in_threadgroup]],
    uint tg_size [[threads_per_threadgroup]])
{
    threadgroup atomic_uint s_csf_lo, s_csf_hi, s_cm_lo, s_cm_hi;
    iadm_csf_cm_s123(ref_band, dis_band, csf_f, accum_out, d, c, wg_id, lid, tg_size, &s_csf_lo,
                     &s_csf_hi, &s_cm_lo, &s_cm_hi);
}

/* ------------------------------------------------------------------ */
/*  Stage 3b — AIM CM numerator. Writes accum slots [6..8].            */
/*  Signal = i_rfactor * a_val; threshold = csf_r 3x3 neighbourhood     */
/*  (neighbours FIX_ONE_BY_30, center ONE_BY_15). Scale 0 (int16).     */
/* ------------------------------------------------------------------ */
/* csf_r for a single (theta) at (y,x), scale 0 — mirrors inline_s0_csf_r. */
static inline int iadm_s0_csf_r_at(const device short *ref, const device short *dis, int y, int x,
                                   int buf_stride, int half_h, int theta, constant IadmCsf &c)
{
    int oh = iadm_read16(ref, 1, y, x, buf_stride, half_h);
    int ov = iadm_read16(ref, 2, y, x, buf_stride, half_h);
    int od = iadm_read16(ref, 3, y, x, buf_stride, half_h);
    int th = iadm_read16(dis, 1, y, x, buf_stride, half_h);
    int tv = iadm_read16(dis, 2, y, x, buf_stride, half_h);
    int td = iadm_read16(dis, 3, y, x, buf_stride, half_h);
    bool af = iadm_angle_flag_s0(oh, ov, th, tv);
    int o_val = (theta == 0) ? oh : (theta == 1) ? ov : od;
    int t_val = (theta == 0) ? th : (theta == 1) ? tv : td;
    (void)od;
    (void)td;
    int r_val = iadm_decouple_r_s0(o_val, t_val, af, c.gain_limit);
    uint irf = (theta == 0) ? c.i_rfactor_h : (theta == 1) ? c.i_rfactor_v : c.i_rfactor_d;
    int band = theta + 1;
    int dst_val = (int)(irf * (uint)r_val);
    return (dst_val + (int)IADM_S0_SHIFTADD[band]) >> IADM_S0_SHIFTS[band];
}

static void iadm_aim_cm_s0(const device short *ref_band, const device short *dis_band,
                           device uint *accum_out, constant IadmDims &d, constant IadmCsf &c,
                           uint wg_id, uint lid, uint tg_size, threadgroup atomic_uint *s_lo,
                           threadgroup atomic_uint *s_hi)
{
    const int active_h = c.active_bottom - c.active_top;
    const int active_w = c.active_right - c.active_left;
    if (active_h <= 0 || active_w <= 0) {
        return;
    }

    if (lid == 0) {
        atomic_store_explicit(s_lo, 0u, memory_order_relaxed);
        atomic_store_explicit(s_hi, 0u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint num_rows = (uint)active_h;
    const uint band_idx = wg_id / num_rows;
    const uint row_idx = wg_id - band_idx * num_rows;
    const int row = c.active_top + (int)row_idx;
    const int w = d.half_w;
    const int h = d.half_h;

    uint irf = (band_idx == 0u) ? c.i_rfactor_h : (band_idx == 1u) ? c.i_rfactor_v : c.i_rfactor_d;
    const int shift_sub = c.cm_shift_sub[band_idx];
    const int shift_sq = c.cm_shift_sq[band_idx];
    const long add_shift_sq = (long)c.cm_add_shift_sq[band_idx];
    const int shift_cub = c.cm_shift_cub[band_idx];
    const long add_shift_cub = (long)c.cm_add_shift_cub[band_idx];
    ulong local_aim = 0ul;

    for (int col = c.active_left + (int)lid; col < c.active_right; col += (int)tg_size) {
        /* Threshold: csf_r 3x3 neighbourhood. Neighbours FIX_ONE_BY_30 >>12,
         * center ONE_BY_15 >>12 — computed inline (matches CUDA scale-0 AIM). */
        int thr = 0;
        for (int theta = 0; theta < IADM_NUM_BANDS; ++theta) {
            int sum = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ry = iadm_clampx(row + dy, h);
                for (int dx = -1; dx <= 1; ++dx) {
                    int rx = iadm_clampx(col + dx, w);
                    int csf_r = iadm_s0_csf_r_at(ref_band, dis_band, ry, rx, d.buf_stride, d.half_h,
                                                 theta, c);
                    if (dx == 0 && dy == 0) {
                        sum += (int)(((IADM_S0_ONE_BY_15 * (uint)abs(csf_r)) + 2048u) >> 12);
                    } else {
                        sum += (int)(((IADM_S0_FIX_ONE_BY_30 * (uint)abs(csf_r)) + 2048u) >> 12);
                    }
                }
            }
            thr += sum;
        }
        /* Signal: abs(i_rfactor * decouple_a) — matches adm_cm_aim_line_kernel. */
        int a_band;
        (void)iadm_s0_band_vals(ref_band, dis_band, row, col, d.buf_stride, d.half_h, (int)band_idx,
                                false, c, &a_band);
        int x = abs((int)(irf * (uint)a_band)) - (thr << shift_sub);
        if (x < 0) {
            x = 0;
        }
        local_aim += iadm_cm_cube((long)x, shift_sq, add_shift_sq, shift_cub, add_shift_cub);
    }

    ulong total = iadm_tg_reduce_u64(local_aim, s_lo, s_hi, lid, tg_size);
    if (lid == 0) {
        ulong out = (total + (ulong)c.cm_add_shift_inner) >> (uint)c.cm_shift_inner;
        const uint slot = wg_id * (uint)IADM_ACCUM_SLOTS;
        const uint base = slot * 2u;
        accum_out[(base + 6u + band_idx) * 2u + 0u] = (uint)(out & 0xFFFFFFFFul);
        accum_out[(base + 6u + band_idx) * 2u + 1u] = (uint)(out >> 32);
    }
}

kernel void integer_adm_aim_cm_s0(
    const device short *ref_band [[buffer(0)]], const device short *dis_band [[buffer(1)]],
    device uint *accum_out [[buffer(8)]], constant IadmDims &d [[buffer(4)]],
    constant IadmCsf &c [[buffer(5)]], uint wg_id [[threadgroup_position_in_grid]],
    uint lid [[thread_index_in_threadgroup]], uint tg_size [[threads_per_threadgroup]])
{
    threadgroup atomic_uint s_lo, s_hi;
    iadm_aim_cm_s0(ref_band, dis_band, accum_out, d, c, wg_id, lid, tg_size, &s_lo, &s_hi);
}

/* Scales 1-3 (int32) AIM CM. Threshold neighbours FIX_ONE_BY_30 >>32,
 * center ONE_BY_15 >>32, applied to csf_r values. */
static void iadm_aim_cm_s123(const device int *ref_band, const device int *dis_band,
                             device uint *accum_out, constant IadmDims &d, constant IadmCsf &c,
                             uint wg_id, uint lid, uint tg_size, threadgroup atomic_uint *s_lo,
                             threadgroup atomic_uint *s_hi)
{
    const int active_h = c.active_bottom - c.active_top;
    const int active_w = c.active_right - c.active_left;
    if (active_h <= 0 || active_w <= 0) {
        return;
    }

    if (lid == 0) {
        atomic_store_explicit(s_lo, 0u, memory_order_relaxed);
        atomic_store_explicit(s_hi, 0u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint num_rows = (uint)active_h;
    const uint band_idx = wg_id / num_rows;
    const uint row_idx = wg_id - band_idx * num_rows;
    const int row = c.active_top + (int)row_idx;
    const int w = d.half_w;
    const int h = d.half_h;

    const uint shift_flt = 32u;
    const long add_bef_shift_flt = (long)(1u << (shift_flt - 1u));
    const int shift_sq = c.cm_shift_sq[band_idx];
    const long add_shift_sq = (long)c.cm_add_shift_sq[band_idx];
    const int shift_cub = c.cm_shift_cub[band_idx];
    const long add_shift_cub = (long)c.cm_add_shift_cub[band_idx];
    ulong local_aim = 0ul;

    for (int col = c.active_left + (int)lid; col < c.active_right; col += (int)tg_size) {
        /* Threshold: csf_r 3x3 neighbourhood, neighbours FIX_ONE_BY_30 >>32,
         * center ONE_BY_15 >>32 (matches i4_adm_cm_aim_line_kernel_fused). */
        int thr = 0;
        for (int theta = 0; theta < IADM_NUM_BANDS; ++theta) {
            int sum = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ry = iadm_clampx(row + dy, h);
                for (int dx = -1; dx <= 1; ++dx) {
                    int rx = iadm_clampx(col + dx, w);
                    int a_dummy;
                    int csf_r = iadm_i4_band_vals(ref_band, dis_band, ry, rx, d.buf_stride,
                                                  d.half_h, theta, true, c, &a_dummy);
                    if (dx == 0 && dy == 0) {
                        sum += (int)((((long)IADM_I4_ONE_BY_15 * abs(csf_r)) + add_bef_shift_flt) >>
                                     shift_flt);
                    } else {
                        sum += (int)((((long)IADM_I4_FIX_ONE_BY_30 * abs(csf_r)) +
                                      add_bef_shift_flt) >>
                                     shift_flt);
                    }
                }
            }
            thr += sum;
        }
        /* Signal: csf of decouple_a (= inline_i4_csf_a). shift_sub = 0. */
        int a_band;
        int csf_a_band = iadm_i4_band_vals(ref_band, dis_band, row, col, d.buf_stride, d.half_h,
                                           (int)band_idx, false, c, &a_band);
        int x = abs(csf_a_band) - thr;
        if (x < 0) {
            x = 0;
        }
        local_aim += iadm_cm_cube((long)x, shift_sq, add_shift_sq, shift_cub, add_shift_cub);
    }

    ulong total = iadm_tg_reduce_u64(local_aim, s_lo, s_hi, lid, tg_size);
    if (lid == 0) {
        ulong out = (total + (ulong)c.cm_add_shift_inner) >> (uint)c.cm_shift_inner;
        const uint slot = wg_id * (uint)IADM_ACCUM_SLOTS;
        const uint base = slot * 2u;
        accum_out[(base + 6u + band_idx) * 2u + 0u] = (uint)(out & 0xFFFFFFFFul);
        accum_out[(base + 6u + band_idx) * 2u + 1u] = (uint)(out >> 32);
    }
}

kernel void integer_adm_aim_cm_s123(
    const device int *ref_band [[buffer(0)]], const device int *dis_band [[buffer(1)]],
    device uint *accum_out [[buffer(8)]], constant IadmDims &d [[buffer(4)]],
    constant IadmCsf &c [[buffer(5)]], uint wg_id [[threadgroup_position_in_grid]],
    uint lid [[thread_index_in_threadgroup]], uint tg_size [[threads_per_threadgroup]])
{
    threadgroup atomic_uint s_lo, s_hi;
    iadm_aim_cm_s123(ref_band, dis_band, accum_out, d, c, wg_id, lid, tg_size, &s_lo, &s_hi);
}
