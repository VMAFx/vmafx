/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernels for the integer_adm feature extractor (feature
 *  "adm", the VMAF-default ADM path). Integer / fixed-point twin of
 *  core/src/feature/metal/float_adm.metal — it mirrors that kernel's
 *  pipeline stages, 1D per-(band,row) reduction, and 9-slot accumulator
 *  layout, and swaps the float arithmetic for the bit-exact
 *  fixed-point arithmetic of the CPU reference
 *  core/src/feature/integer_adm.c and the CUDA twin
 *  core/src/feature/cuda/integer_adm/ (adm_dwt2 / adm_decouple_inline /
 *  adm_csf / adm_csf_den / adm_cm .cu).
 *
 *  Up to five dispatches per scale (4 scales total), in the order
 *  integer_adm_metal_host.c::iadm_metal_stages() lists them; the .mm encodes
 *  them. The integer pipeline has two distinct data
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
 *        At scales >0 the host swaps to the i4 vert/hori variants below:
 *        integer_adm_dwt_vert_s1 at scale 1 (int16 parent),
 *        integer_adm_dwt_vert_s123 at scales 2 and 3 (int32 parent).
 *
 *    Stage 1 — integer_adm_dwt_hori
 *        Fixed-point DWT horizontal pass — reads the lo/hi sub-rows, emits
 *        4 sub-bands (a=LL, h=HL, v=LH, d=HH) into int16 (scale 0) or int32
 *        (scales 1-3) band buffers, with the per-scale (add_shift, shift)
 *        rounding of the CPU (i4_dwt2_round() at scales 1-3).
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
 *    Stage 3b — integer_adm_aim_cm
 *        AIM CM numerator (noise_weight=0): signal = i_rfactor * a_val,
 *        threshold = csf_r 3x3 neighbourhood. Writes accum slots [6..8].
 *
 *  accum_out layout per (band,row) threadgroup: VMAF_MTL_IADM_ACCUM_SLOTS
 *  slots, each a lo/hi pair of uint32 (no 64-bit MSL atomics), addressed by
 *  vmaf_mtl_iadm_accum_word() of metal_integer_adm_uniforms.h, which the host
 *  reads with too:
 *    [0..2] csf_den per band (adm2 denominator, uint64 cube sum)
 *    [3..5] cm_num per band  (adm2 CM numerator, int64 cube sum)
 *    [6..8] aim_cm per band  (AIM CM numerator, int64 cube sum)
 *  The host (integer_adm_metal_host.c) adds the rows and concludes each scale
 *  with the CPU's adm_cm_result() / adm_csf_den_result() and their i4_ forms.
 *
 *  Scale 1 reads the int16 band a of scale 0 (integer_adm_dwt_vert_s1, the
 *  CPU's i16_to_i32()); scales 2 and 3 read the int32 band a of the scale
 *  before (integer_adm_dwt_vert_s123). T-METAL-INTEGER-ADM-TWIN-DEFECTS-
 *  2026-10-05.
 *
 *  Numeric design notes:
 *   - No 64-bit atomics in MSL (Apple GPU); each (band,row) threadgroup owns
 *     a unique accum slot, so the cube sums are written directly (no
 *     cross-TG accumulation on device). The host sums the per-row partials
 *     in 64-bit. Per-TG fan-in uses a threadgroup int64 (lo/hi uint) array.
 *   - The fixed-point decouple, get_best15_from32 (__clz emulation via MSL
 *     clz()), and per-band shift tables are load-bearing for places=4 parity;
 *     they replicate the CUDA inline helpers exactly.
 *   - CSF mode selection and per-scale power-of-two normalisation happen in
 *     the .mm host wrapper; all modes use these same integer kernels.
 */

#include <metal_stdlib>
using namespace metal;

/* The decouple (reciprocal, Q15 ratio, gain limit): ADR-1498, ADR-1413. */
#include "metal_integer_adm_math.h"
/* IadmDims, IadmCsf and the reduction layout, shared with the host. */
#include "metal_integer_adm_uniforms.h"

#define IADM_NUM_BANDS 3

/* 9/7 biorthogonal DWT taps in fixed point (Q-scaled), identical to the
 * CUDA host's AdmFixedParametersCuda.dwt2_db2_coeffs_{lo,hi}. */
constant int IADM_LO[4] = {15826, 27411, 7345, -4240};
constant int IADM_HI[4] = {-4240, -7345, 27411, -15826};
constant int IADM_LO_SUM = 46342; /* dwt2_db2_coeffs_lo_sum */

/* Exact significand of cos(1 deg)^2 as binary32 (0x3F7FEC0A): the constant
 * is MC * 2^-24 with MC = 16772106, so IADM_AF_D = 2^24 - MC. Used by
 * iadm_angle_flag(); mirrors ADM_ANGLE_FLAG_D in
 * core/src/feature/adm_angle_flag.h. */
constant ulong IADM_AF_D = 5110ul;

/* CM threshold fixed-point coefficients (matches integer_adm.c /
 * adm_cm.cu). Scale 0: ONE_BY_15 with >>12; scales 1-3: I4_ONE_BY_15 with
 * >>32 (shift_flt), and FIX_ONE_BY_30 = I4_ONE_BY_15/2 for neighbours. */
constant uint IADM_S0_FIX_ONE_BY_30 = 4369u;      /* (1/30)*2^17, >>12 */
constant uint IADM_S0_ONE_BY_15 = 8738u;          /* (1/15)*2^17, >>12 */
constant long IADM_I4_FIX_ONE_BY_30 = 143165577l; /* (1/30)*2^32, >>32 */
constant long IADM_I4_ONE_BY_15 = 286331153l;     /* (1/15)*2^32, >>32 */

/* Mirror form matches the CUDA calculate_indices() over-range reflection used
 * throughout the integer DWT (negative indices are handled by the n==0
 * special-case in the kernels, mirroring calculate_indices()). */
static inline int iadm_mirror_hi(int idx, int sup)
{
    return (idx >= sup) ? (2 * sup - idx - 1) : idx;
}

/* ------------------------------------------------------------------ */
/*  angle_flag — shared by scale 0 and scales 1-3.                     */
/*                                                                     */
/*  MSL mirror of adm_angle_flag_i64() in                              */
/*  core/src/feature/adm_angle_flag.h (ADR-1194). Metal Shading        */
/*  Language has no `double` type, so the golden-frozen CPU expression */
/*  — narrow each operand to float, then compare in binary64 — cannot  */
/*  be written directly; this is its bit-identical reformulation in    */
/*  64-bit integer arithmetic. Keep the two in lockstep: the C header  */
/*  is the source of truth and core/test/test_adm_angle_flag.c pins it */
/*  against the frozen expression.                                     */
/* ------------------------------------------------------------------ */
static inline int iadm_af_bitlen(ulong v)
{
    int n = 0;
    if (v >> 32) {
        v >>= 32;
        n += 32;
    }
    if (v >> 16) {
        v >>= 16;
        n += 16;
    }
    if (v >> 8) {
        v >>= 8;
        n += 8;
    }
    if (v >> 4) {
        v >>= 4;
        n += 4;
    }
    if (v >> 2) {
        v >>= 2;
        n += 2;
    }
    if (v >> 1) {
        v >>= 1;
        n += 1;
    }
    return n + (int)v;
}

static inline void iadm_af_norm24(ulong v, thread ulong *m, thread int *e)
{
    const int n = iadm_af_bitlen(v);
    if (n <= 24) {
        *m = v << (24 - n);
        *e = n - 24;
        return;
    }
    int s = n - 24;
    ulong q = v >> s;
    const ulong rem = v & ((1ul << s) - 1ul);
    /* NOT `half`: that is a built-in 16-bit float type in MSL, so declaring a
     * variable with that name is a redeclaration error (the Metal lane reports
     * "cannot combine with previous 'type-name' declaration specifier"). */
    const ulong halfway = 1ul << (s - 1);
    if (rem > halfway || (rem == halfway && (q & 1ul) != 0ul)) {
        q++;
        if (q == (1ul << 24)) {
            q >>= 1;
            s++;
        }
    }
    *m = q;
    *e = s;
}

/* round53(V) * 2^p with V = MC*mo*mt*2^-24; the scale exponent lands in
 * *p_out. Mirrors adm_angle_flag_round53_v() in
 * core/src/feature/adm_angle_flag.h. */
static inline ulong iadm_af_round53_v(ulong mo, ulong mt, thread int *p_out)
{
    const ulong g = mo * mt;
    const ulong r = g & 0xFFFFFFul;
    const ulong s_val = g - IADM_AF_D * (g >> 24);
    const ulong dr = IADM_AF_D * r;

    int n = iadm_af_bitlen(s_val);
    const ulong below = s_val - (1ul << (n - 1));
    if (below < IADM_AF_D && (below << 24) < dr) {
        n--;
    }

    const int p = 53 - n;
    const ulong u = dr << p;
    const ulong ui = u >> 24;
    const ulong uf = u & 0xFFFFFFul;
    ulong rounded = (s_val << p) - ui;
    if (uf != 0ul) {
        const ulong frac = 0x1000000ul - uf;
        rounded -= 1ul;
        if (frac > 0x800000ul) {
            rounded += 1ul;
        } else if (frac == 0x800000ul) {
            rounded += (rounded & 1ul);
        }
    }
    *p_out = p;
    return rounded;
}

static inline bool iadm_angle_flag(long ot_dp, long o_mag_sq, long t_mag_sq)
{
    if (ot_dp < 0) {
        return false;
    }
    if (o_mag_sq <= 0 || t_mag_sq <= 0) {
        return true; /* RHS <= 0 <= LHS (magnitudes are sums of squares) */
    }
    if (ot_dp == 0) {
        return false;
    }

    ulong mp = 0ul, mo = 0ul, mt = 0ul;
    int ep = 0, eo = 0, et = 0;
    iadm_af_norm24((ulong)ot_dp, &mp, &ep);
    iadm_af_norm24((ulong)o_mag_sq, &mo, &eo);
    iadm_af_norm24((ulong)t_mag_sq, &mt, &et);

    const int sp = 2 * ep - eo - et;
    if (sp >= 3) {
        return true;
    }
    if (sp <= -3) {
        return false;
    }

    int p = 0;
    const ulong rounded = iadm_af_round53_v(mo, mt, &p);
    return ((mp * mp) << (sp + p)) >= rounded;
}

/* ------------------------------------------------------------------ */
/*  Fixed-point decouple — scale 0 (int16 bands).                      */
/*  Replica of decouple_angle_flag_s0; the decouple itself is          */
/*  vmaf_mtl_iadm_decouple_s0() (metal_integer_adm_math.h).            */
/* ------------------------------------------------------------------ */
static inline bool iadm_angle_flag_s0(int oh, int ov, int th, int tv)
{
    /* The sums reach 2^31 at the corners of the int16 range (all four bands at -32768): int64
     * as the CPU's are, each int product alone fits int. */
    long ot_dp = (long)(oh * th) + (long)(ov * tv);
    long o_mag_sq = (long)(oh * oh) + (long)(ov * ov);
    long t_mag_sq = (long)(th * th) + (long)(tv * tv);
    return iadm_angle_flag(ot_dp, o_mag_sq, t_mag_sq);
}

/* The gain limit of the frame, from the uniform. */
static inline AdmGainLimit iadm_gain(constant IadmCsf &c)
{
    AdmGainLimit g;
    g.m_hi = c.gain_m_hi;
    g.m_lo = c.gain_m_lo;
    g.frac_bits = c.gain_frac_bits;
    return g;
}

/* ------------------------------------------------------------------ */
/*  Fixed-point decouple — scales 1-3 (int32 bands).                   */
/*  Replica of decouple_angle_flag_s123; the decouple itself is        */
/*  vmaf_mtl_iadm_decouple_s123() (metal_integer_adm_math.h).          */
/* ------------------------------------------------------------------ */
static inline bool iadm_angle_flag_s123(int oh, int ov, int th, int tv)
{
    long ot_dp = (long)oh * th + (long)ov * tv;
    long o_mag_sq = (long)oh * oh + (long)ov * ov;
    long t_mag_sq = (long)th * th + (long)tv * tv;
    return iadm_angle_flag(ot_dp, o_mag_sq, t_mag_sq);
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

    /* 64-bit sums: the first three low-pass taps add up to 50582, so a 16-bit
     * sum passes INT_MAX once three samples reach 42456. The CPU forms it in
     * int64 too (adm_dwt2_vpass16_tap4); the normalised value fits in int. */
    long accum_lo = 0;
    long accum_hi = 0;
    for (int k = 0; k < 4; ++k) {
        accum_lo += (long)IADM_LO[k] * s[k];
        accum_hi += (long)IADM_HI[k] * s[k];
    }
    /* normalise range (0..N) -> (-N/2..N/2): subtract coeff_sum * v_add_shift. */
    accum_lo -= (long)IADM_LO_SUM * c.v_add_shift;
    accum_hi -= 0 * c.v_add_shift; /* hi_sum == 0 */

    const int out_stride = d.cur_w * 2;
    device int *dst = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    dst[gy * out_stride + gx] = (int)((accum_lo + c.v_add_shift) >> c.v_shift);
    dst[gy * out_stride + d.cur_w + gx] = (int)((accum_hi + c.v_add_shift) >> c.v_shift);
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
/*  Stage 0' — DWT vertical pass (scales 1-3) over the parent's band a. */
/*  The parent of scale 1 is the int16 band of scale 0, read widened as  */
/*  the CPU's i16_to_i32() does; the parent of scales 2 and 3 is int32.  */
/*  The s123 vertical rounding (add_shift, shift) is i4_dwt2_round()'s.  */
/* ------------------------------------------------------------------ */
static void iadm_dwt_vert_s123_impl(const device int *parent32_ref, const device int *parent32_dis,
                                    const device short *parent16_ref,
                                    const device short *parent16_dis, device int *dwt_tmp_ref,
                                    device int *dwt_tmp_dis, constant IadmDims &d,
                                    constant IadmCsf &c, uint3 gid, bool parent16)
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
    const device int *band32 = (plane_is_dis == 0) ? parent32_ref : parent32_dis;
    const device short *band16 = (plane_is_dis == 0) ? parent16_ref : parent16_dis;
    const int pstride = d.parent_buf_stride;
    int yy[4] = {idx.x, idx.y, idx.z, idx.w};
    long accum_lo = 0;
    long accum_hi = 0;
    for (int k = 0; k < 4; ++k) {
        const int at = yy[k] * pstride + gx;
        const int v = parent16 ? (int)band16[at] : band32[at];
        accum_lo += (long)IADM_LO[k] * v;
        accum_hi += (long)IADM_HI[k] * v;
    }

    const int out_stride = d.cur_w * 2;
    device int *dst = (plane_is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
    dst[gy * out_stride + gx] = (int)((accum_lo + c.s123_vert_add) >> c.s123_vert_shift);
    dst[gy * out_stride + d.cur_w + gx] = (int)((accum_hi + c.s123_vert_add) >> c.s123_vert_shift);
}

/* Scale 1: the parent is the int16 band of scale 0. */
kernel void integer_adm_dwt_vert_s1(const device short *parent_ref_band [[buffer(6)]],
                                    const device short *parent_dis_band [[buffer(7)]],
                                    device int *dwt_tmp_ref [[buffer(2)]],
                                    device int *dwt_tmp_dis [[buffer(3)]],
                                    constant IadmDims &d [[buffer(4)]],
                                    constant IadmCsf &c [[buffer(5)]],
                                    uint3 gid [[thread_position_in_grid]])
{
    iadm_dwt_vert_s123_impl((const device int *)0, (const device int *)0, parent_ref_band,
                            parent_dis_band, dwt_tmp_ref, dwt_tmp_dis, d, c, gid, true);
}

/* Scales 2 and 3: the parent is the int32 band of the scale before. */
kernel void integer_adm_dwt_vert_s123(const device int *parent_ref_band [[buffer(6)]],
                                      const device int *parent_dis_band [[buffer(7)]],
                                      device int *dwt_tmp_ref [[buffer(2)]],
                                      device int *dwt_tmp_dis [[buffer(3)]],
                                      constant IadmDims &d [[buffer(4)]],
                                      constant IadmCsf &c [[buffer(5)]],
                                      uint3 gid [[thread_position_in_grid]])
{
    iadm_dwt_vert_s123_impl(parent_ref_band, parent_dis_band, (const device short *)0,
                            (const device short *)0, dwt_tmp_ref, dwt_tmp_dis, d, c, gid, false);
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
    int r_val = vmaf_mtl_iadm_decouple_s0(o_val, t_val, af, iadm_gain(c));

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
    int oh = iadm_read32(ref, 1, y, x, buf_stride, half_h);
    int ov = iadm_read32(ref, 2, y, x, buf_stride, half_h);
    int od = iadm_read32(ref, 3, y, x, buf_stride, half_h);
    int th = iadm_read32(dis, 1, y, x, buf_stride, half_h);
    int tv = iadm_read32(dis, 2, y, x, buf_stride, half_h);
    int td = iadm_read32(dis, 3, y, x, buf_stride, half_h);

    bool af = iadm_angle_flag_s123(oh, ov, th, tv);
    int o_val = (theta == 0) ? oh : (theta == 1) ? ov : od;
    int t_val = (theta == 0) ? th : (theta == 1) ? tv : td;
    int r_val = vmaf_mtl_iadm_decouple_s123(o_val, t_val, af, iadm_gain(c));

    uint irf = (theta == 0) ? c.i_rfactor_h : (theta == 1) ? c.i_rfactor_v : c.i_rfactor_d;
    int src = use_r ? r_val : (t_val - r_val);
    int csf = vmaf_mtl_iadm_i4_csf(irf, src, c.i4_add_shift_dst, c.i4_shift_dst);
    *out_a = (t_val - r_val);
    return csf;
}

/* ------------------------------------------------------------------ */
/*  Stage 2 — Decouple + CSF: writes csf_a + csf_f.                     */
/*  Scale 0 (int16) variant.                                           */
/* ------------------------------------------------------------------ */
static void iadm_decouple_csf_s0(const device short *ref_band, const device short *dis_band,
                                 device short *csf_a, device short *csf_f, constant IadmDims &d,
                                 constant IadmCsf &c, uint2 gid)
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= d.half_w || gy >= d.half_h) {
        return;
    }
    for (int b = 0; b < IADM_NUM_BANDS; ++b) {
        int a_dummy;
        int csf = iadm_s0_band_vals(ref_band, dis_band, gy, gx, d.buf_stride, d.half_h, b, false, c,
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
    iadm_decouple_csf_s0(ref_band, dis_band, csf_a, csf_f, d, c, gid);
}

/* Scales 1-3 (int32) decouple + CSF. csf_f uses FIX_ONE_BY_30 >> 32. */
static void iadm_decouple_csf_s123(const device int *ref_band, const device int *dis_band,
                                   device int *csf_a, device int *csf_f, constant IadmDims &d,
                                   constant IadmCsf &c, uint2 gid)
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= d.half_w || gy >= d.half_h) {
        return;
    }
    for (int b = 0; b < IADM_NUM_BANDS; ++b) {
        int a_dummy;
        int csf = iadm_i4_band_vals(ref_band, dis_band, gy, gx, d.buf_stride, d.half_h, b, false, c,
                                    &a_dummy);
        iadm_write32(csf_a, b, gy, gx, d.buf_stride, d.half_h, csf);
        int flt = vmaf_mtl_iadm_i4_masking_term(IADM_I4_FIX_ONE_BY_30, csf, c.i4_add_shift_flt,
                                                c.i4_shift_flt);
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
    iadm_decouple_csf_s123(ref_band, dis_band, csf_a, csf_f, d, c, gid);
}

/* ------------------------------------------------------------------ */
/*  Per-(band,row) int64 cube reduction. accum_out stores lo/hi uint     */
/*  pairs (no 64-bit MSL atomics). threadgroup fan-in via 2x uint arrays. */
/* ------------------------------------------------------------------ */
static inline ulong iadm_tg_reduce_u64(ulong v, threadgroup atomic_uint *scratch_lo,
                                       threadgroup atomic_uint *scratch_hi, uint lid)
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

/* MSL twin of adm_cm_excess_s0() in feature/adm_cm_accumulator.h: the scale-0
 * masking excess clamp(|x| - thr * 2^shift, 0, INT32_MAX), formed in 64 bits
 * because thr * 2^shift can leave int (ADR-1402). The centre tap of `thr`
 * stays an int at its call sites; it is never narrowed to short. */
static inline int adm_cm_excess_s0(int x, int thr, int shift)
{
    const long magnitude = (x < 0) ? -(long)x : (long)x;
    const long excess = magnitude - ((long)thr * (1l << shift));
    if (excess <= 0l) {
        return 0;
    }
    return (excess > 2147483647l) ? 2147483647 : (int)excess;
}

/* MSL twin of feature/adm_cm_accumulator.h. Keep the full row total as ulong
 * until this final fold; shifting per lane/threadgroup is not distributive. */
static inline ulong adm_cm_round_row_total(ulong row_total, ulong rounding, uint shift)
{
    return (row_total + rounding) >> shift;
}

/* One 64-bit sum into slot `slot` of threadgroup `wg`, through the layout the
 * host reads with (vmaf_mtl_iadm_accum_word()). */
static inline void iadm_store_slot(device uint *accum_out, uint wg, uint slot, ulong v)
{
    accum_out[vmaf_mtl_iadm_accum_word(wg, slot, 0u)] = (uint)(v & 0xFFFFFFFFul);
    accum_out[vmaf_mtl_iadm_accum_word(wg, slot, 1u)] = (uint)(v >> 32);
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
        int decouple_r = vmaf_mtl_iadm_decouple_s0(o_val, t_val, af, iadm_gain(c));
        const int x = adm_cm_excess_s0((int)(irf * (uint)decouple_r), thr, shift_sub);
        local_cm += iadm_cm_cube((long)x, shift_sq, add_shift_sq, shift_cub, add_shift_cub);
    }

    ulong total_csf = iadm_tg_reduce_u64(local_csf, s_csf_lo, s_csf_hi, lid);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    ulong total_cm = iadm_tg_reduce_u64(local_cm, s_cm_lo, s_cm_hi, lid);

    if (lid == 0) {
        /* inner-accum shift applied per (band,row) to match accum_global. */
        ulong cm_out =
            adm_cm_round_row_total(total_cm, (ulong)c.cm_add_shift_inner, (uint)c.cm_shift_inner);
        ulong csf_out = (total_csf + (ulong)c.den_add_shift_accum) >> (uint)c.den_shift_accum;
        iadm_store_slot(accum_out, wg_id, VMAF_MTL_IADM_SLOT_DEN + band_idx, csf_out);
        iadm_store_slot(accum_out, wg_id, VMAF_MTL_IADM_SLOT_CM + band_idx, cm_out);
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

    /* i4_cube_term(): I4AdmDenCtx's rounding, 2^shift_sq at the square. */
    const uint den_shift_sq = c.den_shift_sq;
    const ulong den_add_shift_sq = (ulong)c.den_add_shift_sq;

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
            const int csf_a_center = iadm_i4_band_vals(ref_band, dis_band, row, col, d.buf_stride,
                                                       d.half_h, theta, false, c, &a_center);
            int sum = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ry = iadm_clampx(row + dy, h);
                for (int dx = -1; dx <= 1; ++dx) {
                    int rx = iadm_clampx(col + dx, w);
                    if (dx == 0 && dy == 0) {
                        sum += vmaf_mtl_iadm_i4_masking_term(IADM_I4_ONE_BY_15, csf_a_center,
                                                             c.i4_add_shift_flt, c.i4_shift_flt);
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

    ulong total_csf = iadm_tg_reduce_u64(local_csf, s_csf_lo, s_csf_hi, lid);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    ulong total_cm = iadm_tg_reduce_u64(local_cm, s_cm_lo, s_cm_hi, lid);

    if (lid == 0) {
        ulong cm_out =
            adm_cm_round_row_total(total_cm, (ulong)c.cm_add_shift_inner, (uint)c.cm_shift_inner);
        ulong csf_out = (total_csf + (ulong)c.den_add_shift_accum) >> (uint)c.den_shift_accum;
        iadm_store_slot(accum_out, wg_id, VMAF_MTL_IADM_SLOT_DEN + band_idx, csf_out);
        iadm_store_slot(accum_out, wg_id, VMAF_MTL_IADM_SLOT_CM + band_idx, cm_out);
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
    int r_val = vmaf_mtl_iadm_decouple_s0(o_val, t_val, af, iadm_gain(c));
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
        const int x = adm_cm_excess_s0((int)(irf * (uint)a_band), thr, shift_sub);
        local_aim += iadm_cm_cube((long)x, shift_sq, add_shift_sq, shift_cub, add_shift_cub);
    }

    ulong total = iadm_tg_reduce_u64(local_aim, s_lo, s_hi, lid);
    if (lid == 0) {
        ulong out =
            adm_cm_round_row_total(total, (ulong)c.cm_add_shift_inner, (uint)c.cm_shift_inner);
        iadm_store_slot(accum_out, wg_id, VMAF_MTL_IADM_SLOT_AIM + band_idx, out);
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
                    const long coeff =
                        (dx == 0 && dy == 0) ? IADM_I4_ONE_BY_15 : IADM_I4_FIX_ONE_BY_30;
                    sum += vmaf_mtl_iadm_i4_masking_term(coeff, csf_r, c.i4_add_shift_flt,
                                                         c.i4_shift_flt);
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

    ulong total = iadm_tg_reduce_u64(local_aim, s_lo, s_hi, lid);
    if (lid == 0) {
        ulong out =
            adm_cm_round_row_total(total, (ulong)c.cm_add_shift_inner, (uint)c.cm_shift_inner);
        iadm_store_slot(accum_out, wg_id, VMAF_MTL_IADM_SLOT_AIM + band_idx, out);
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
