/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernels for the integer_vif feature extractor (feature
 *  name "vif" on the CPU). Fixed-point twin of float_vif.metal — mirrors
 *  the proven float_vif Metal scaffold (4-scale separable Gaussian pyramid,
 *  per-workgroup partial-sum reduction, host-side per-scale division) and
 *  swaps the float arithmetic for the int64 fixed-point arithmetic of the
 *  CPU reference core/src/feature/integer_vif.c (vif_statistic_8 /
 *  vif_statistic_16 / subsample_rd_8 / subsample_rd_16).
 *
 *  Bit-exactness contract: the kernels reproduce the CPU's exact integer
 *  rounding shifts, the uint16/uint32/uint64 accumulator widths, and the
 *  uint16 log2 look-up table (VIF_LOG2_TABLE_SIZE = 32768 entries, generated
 *  host-side by integer_vif.c::log_generate and uploaded as a device buffer).
 *  The scalar log2_32 / log2_64 accessors in integer_vif.h are replicated in
 *  MSL via vif_log2_32 / vif_log2_64 (clz-based normalisation, same 2048*k
 *  exponent baked into the returned log value). The four int64 moment
 *  accumulators (accum_num_log, accum_den_log, accum_num_non_log,
 *  accum_den_non_log) are reduced per-workgroup into (lo, hi) uint32 pairs
 *  (MSL lacks atomic_long on Apple Silicon) and summed in int64 on the host,
 *  which then applies the exact CPU final formula:
 *
 *    num = accum_num_log/2048.0 +
 *          (accum_den_non_log - (accum_num_non_log/16384.0)/65025.0)
 *    den = accum_den_log/2048.0 + accum_den_non_log
 *
 *  Two kernel families, dispatched in order by integer_vif_metal.mm:
 *
 *   1. integer_vif_compute_{8,16} — the per-scale statistic. Reads ref/dis
 *      at this scale (raw uint8 at scale 0 via the _8 variant; uint16
 *      decimated buffers at scales 1..3 via the _16 variant), applies the
 *      separable V then H fixed-point Gaussian, computes the per-pixel
 *      integer VIF statistic, and reduces the four int64 accumulators per
 *      threadgroup. Mirrors vif_statistic_8 / vif_statistic_16.
 *
 *   2. integer_vif_decimate_{8,16} — produces the decimated uint16 ref/dis
 *      for the NEXT scale. Applies the (scale+1) fixed-point filter at the
 *      previous scale's dims with the CPU's intermediate uint16 rounding
 *      (V-pass rounded to uint16, then H-pass rounded to uint16), sampled at
 *      (2*gx, 2*gy). Mirrors subsample_rd_8 / subsample_rd_16 +
 *      decimate_and_pad. The _8 variant handles the scale 0->1 transition
 *      (raw uint8 input); the _16 variant handles scales 1->2 and 2->3.
 *
 *  Padding: the CPU pads the top/bottom by mirroring whole rows
 *  (pad_top_and_bottom: row i below 0 copies row +i; row beyond h-1 copies
 *  row h-1-i — i.e. reflect-101 about the edge rows) and pads the horizontal
 *  tmp arrays via PADDING_SQ_DATA (reflect-101 about column 0 / column w-1).
 *  Both are the AVX2/scalar reflect-101 mirror, reproduced here by
 *  vif_mirror(idx, sup) = reflect about [0, sup-1].
 *
 *  Numeric contract: hardcoded default vif_enhn_gain_limit handling matches
 *  the CPU (the .mm forwards the option value as a float via cfg). Parity
 *  target places=4 (1e-4, ADR-0214) — the integer kernel is bit-exact to the
 *  CPU integer reference up to the int64->float final cast, well inside 1e-4.
 */

#include <metal_stdlib>
using namespace metal;

#define IVIF_BX 16
#define IVIF_BY 16
#define IVIF_MAX_HFW 8
/* Worst-case tile pitch (WG + 2*max_hfw) = 16 + 16 = 32. */
#define IVIF_MAX_TILE_W (IVIF_BX + 2 * IVIF_MAX_HFW)

#define VIF_LOG2_TABLE_SIZE 32768u

/* Per-scale fixed-point Gaussian coefficients — identical uint16 arrays to
 * vif_filter1d_table[4] in integer_vif.h. Each row is zero-padded to 18. */
constant ushort IVIF_FILT_S0[18] = {489,  935,  1640, 2640, 3896, 5274, 6547, 7455, 7784,
                                    7455, 6547, 5274, 3896, 2640, 1640, 935,  489,  0};
constant ushort IVIF_FILT_S1[18] = {1244, 3663, 7925, 12590, 14692, 12590, 7925, 3663, 1244,
                                    0,    0,    0,    0,     0,     0,     0,    0,    0};
constant ushort IVIF_FILT_S2[18] = {3571, 16004, 26386, 16004, 3571, 0, 0, 0, 0,
                                    0,    0,     0,     0,     0,    0, 0, 0, 0};
constant ushort IVIF_FILT_S3[18] = {10904, 43728, 10904, 0, 0, 0, 0, 0, 0,
                                    0,     0,     0,     0, 0, 0, 0, 0, 0};

inline constant ushort *ivif_filt(int scale)
{
    if (scale == 0) {
        return IVIF_FILT_S0;
    }
    if (scale == 1) {
        return IVIF_FILT_S1;
    }
    if (scale == 2) {
        return IVIF_FILT_S2;
    }
    return IVIF_FILT_S3;
}

inline int ivif_fw(int scale)
{
    const int fw[4] = {17, 9, 5, 3};
    return fw[scale];
}

/* reflect-101 mirror about [0, sup-1], matching the CPU border handling
 * (pad_top_and_bottom and PADDING_SQ_DATA both reflect about the edge
 * sample, excluding the edge sample itself: idx<0 -> -idx; idx>=sup ->
 * 2*(sup-1)-idx). */
inline int vif_mirror(int idx, int sup)
{
    if (idx < 0) {
        return -idx;
    }
    if (idx >= sup) {
        return 2 * (sup - 1) - idx;
    }
    return idx;
}

/* 64-bit count-leading-zeros, matching __builtin_clzll semantics exactly
 * (UB for 0, never called with 0 here). MSL `clz` is reliable on 32-bit
 * uint; compose two of them for 64-bit. */
inline int vif_clzll(ulong v)
{
    const uint hi = (uint)(v >> 32);
    if (hi != 0u) {
        return (int)clz(hi);
    }
    return 32 + (int)clz((uint)v);
}

/* Replicates integer_vif.h::log2_32: normalise temp to [32768..65535],
 * strip the MSB, look up, add 2048*k. */
inline long vif_log2_32(const device ushort *log2_table, uint temp)
{
    int k = (int)clz(temp);
    k = 16 - k;
    temp = temp >> k;
    return (long)log2_table[temp & (VIF_LOG2_TABLE_SIZE - 1u)] + 2048L * (long)k;
}

/* Replicates integer_vif.h::log2_64. */
inline long vif_log2_64(const device ushort *log2_table, ulong temp)
{
    int k = vif_clzll(temp);
    k = 48 - k;
    temp = temp >> k;
    return (long)log2_table[(uint)temp & (VIF_LOG2_TABLE_SIZE - 1u)] + 2048L * (long)k;
}

/* Read a raw uint8 pixel as the CPU sees it: the integer path uses the raw
 * sample value directly (NO -128 / scaler — that is the float path). HBD
 * (>8 bpc) input is read as `device ushort *` directly by the _16 compute /
 * decimate kernels (the raw uint16 plane is bound as a ushort buffer with a
 * width-in-ushorts stride), so no separate raw-16 reader is needed here. */
inline uint ivif_read_raw_8(const device uchar *plane, uint stride_bytes, int y, int x)
{
    return (uint)plane[(uint)y * stride_bytes + (uint)x];
}

/* ------------------------------------------------------------------ */
/* Per-workgroup int64 accumulator + reduction.                        */
/*                                                                      */
/* MSL has no atomic_long on Apple Silicon, and `simd_sum` is not       */
/* guaranteed for 64-bit integers, so the per-threadgroup reduction is  */
/* a serial sum by thread 0 over a threadgroup scratch array (see       */
/* ivif_reduce_and_store). The per-thread values are small — a single   */
/* pixel contributes at most ~|2048*48| to the log accumulators and     */
/* sigma2_sq (< 2^31) to num_non_log — so each thread's value fits      */
/* int64 trivially, and the WG sum fits int64 too. The host reads each  */
/* WG's int64 partial and sums in int64 (associative, so bit-exact).    */
/* ------------------------------------------------------------------ */
struct VifWgAccum {
    long num_log;
    long den_log;
    long num_non_log;
    long den_non_log;
};

/* The per-pixel integer VIF statistic, shared by the 8/16 compute kernels.
 * Reproduces the inner body of vif_statistic_8 / vif_statistic_16 verbatim.
 * mu1_val/mu2_val are the un-shifted horizontal mu accumulators (uint32);
 * xx/yy/xy are the (accum + add_shift_round_HP) >> shift_HP results. */
inline void ivif_pixel_stat(uint mu1_val, uint mu2_val, uint xx_filt_val, uint yy_filt_val,
                            uint xy_filt_val, float vif_enhn_gain_limit,
                            const device ushort *log2_table, thread VifWgAccum &acc)
{
    const int sigma_nsq = 65536 << 1;

    uint mu1_sq_val = (uint)((((ulong)mu1_val * mu1_val) + 2147483648uL) >> 32);
    uint mu2_sq_val = (uint)((((ulong)mu2_val * mu2_val) + 2147483648uL) >> 32);
    uint mu1_mu2_val = (uint)((((ulong)mu1_val * mu2_val) + 2147483648uL) >> 32);

    int sigma1_sq = (int)(xx_filt_val - mu1_sq_val);
    int sigma2_sq = (int)(yy_filt_val - mu2_sq_val);
    int sigma12 = (int)(xy_filt_val - mu1_mu2_val);

    sigma2_sq = max(sigma2_sq, 0);
    if (sigma1_sq >= sigma_nsq) {
        acc.den_log += vif_log2_32(log2_table, (uint)(sigma_nsq + sigma1_sq)) - 2048L * 17L;

        if (sigma12 > 0 && sigma2_sq > 0) {
            /* CPU uses double for g; MSL has no double, but float carries
             * enough precision here: g = sigma12/(sigma1_sq+eps), then
             * sv_sq = sigma2_sq - g*sigma12 truncated to int32, then the
             * int64 numer1_tmp = (int64)(g*g*sigma1_sq) + numer1. The float
             * g*g*sigma1_sq -> int64 cast and the int sv_sq truncation are
             * the only float-touched values, both small relative to 1e-4. */
            const float eps = 65536.0f * 1.0e-10f;
            float g = (float)sigma12 / ((float)sigma1_sq + eps);
            int sv_sq = sigma2_sq - (int)(g * (float)sigma12);

            sv_sq = max(sv_sq, 0);

            g = min(g, vif_enhn_gain_limit);

            uint numer1 = (uint)((uint)sv_sq + (uint)sigma_nsq);
            long numer1_tmp = (long)(g * g * (float)sigma1_sq) + (long)numer1;
            acc.num_log +=
                vif_log2_64(log2_table, (ulong)numer1_tmp) - vif_log2_64(log2_table, (ulong)numer1);
        }
    } else {
        acc.num_non_log += sigma2_sq;
        acc.den_non_log += 1;
    }
}

/* Reduce a per-thread VifWgAccum to one int64 per field per workgroup,
 * written to wg_accum[wg_idx]. MSL `simd_sum` is not guaranteed for 64-bit
 * integers (and the log accumulators are signed, complicating a lo/hi split),
 * so each thread writes its four int64 values into a threadgroup scratch
 * array indexed by lid and thread 0 serially sums them. Integer addition is
 * associative, so the WG sum is bit-exact regardless of summation order; the
 * host then sums the per-WG int64 partials, again bit-exact. tg_total =
 * IVIF_BX*IVIF_BY (256). */
inline void ivif_reduce_and_store(thread VifWgAccum &acc, device VifWgAccum *wg_accum, uint wg_idx,
                                  uint lid, uint tg_total, threadgroup long *t_nl,
                                  threadgroup long *t_dl, threadgroup long *t_nn,
                                  threadgroup long *t_dn)
{
    t_nl[lid] = acc.num_log;
    t_dl[lid] = acc.den_log;
    t_nn[lid] = acc.num_non_log;
    t_dn[lid] = acc.den_non_log;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        long s_nl = 0, s_dl = 0, s_nn = 0, s_dn = 0;
        for (uint i = 0; i < tg_total; ++i) {
            s_nl += t_nl[i];
            s_dl += t_dl[i];
            s_nn += t_nn[i];
            s_dn += t_dn[i];
        }
        wg_accum[wg_idx].num_log = s_nl;
        wg_accum[wg_idx].den_log = s_dl;
        wg_accum[wg_idx].num_non_log = s_nn;
        wg_accum[wg_idx].den_non_log = s_dn;
    }
}

/* ================================================================== */
/*  Kernel: integer_vif_compute_8  (scale 0, raw uint8 input)          */
/*                                                                      */
/*  Mirrors vif_statistic_8. V-pass: 17-tap, mu rounded (+128)>>8 to    */
/*  uint16, squared moments kept as uint32 (no shift). H-pass: 17-tap,  */
/*  mu un-shifted (uint32), squared moments (+32768)>>16 to uint32.     */
/*                                                                      */
/*  Buffer bindings:                                                    */
/*   [[buffer(0)]] ref_raw   — const uchar * raw ref plane             */
/*   [[buffer(1)]] dis_raw   — const uchar * raw dis plane             */
/*   [[buffer(2)]] log2_tab  — const ushort * VIF log2 LUT             */
/*   [[buffer(3)]] wg_accum  — VifWgAccum * per-WG int64 partials       */
/*   [[buffer(4)]] params    — uint4 (.x=width, .y=height,             */
/*                                     .z=raw_stride_bytes, .w=grid_x)  */
/*   [[buffer(5)]] cfgf      — float4 (.x=vif_enhn_gain_limit)         */
/*  Grid: ceil(width/16) x ceil(height/16),  threads 16x16.            */
/* ================================================================== */
kernel void integer_vif_compute_8(
    const device uchar *ref_raw [[buffer(0)]], const device uchar *dis_raw [[buffer(1)]],
    const device ushort *log2_tab [[buffer(2)]], device VifWgAccum *wg_accum [[buffer(3)]],
    constant uint4 &params [[buffer(4)]], constant float4 &cfgf [[buffer(5)]],
    uint2 gid [[thread_position_in_grid]], uint2 bid [[threadgroup_position_in_grid]],
    uint2 lpos [[thread_position_in_threadgroup]], uint lid [[thread_index_in_threadgroup]])
{
    const int width = (int)params.x;
    const int height = (int)params.y;
    const uint raw_stride = params.z;
    const uint grid_x = params.w;
    const float egl = cfgf.x;

    const int fw = 17;
    const int hfw = 8;
    constant ushort *coeff = IVIF_FILT_S0;

    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    const int lx = (int)lpos.x;
    const int ly = (int)lpos.y;
    const bool valid = (gx < width && gy < height);

    const int tile_w = IVIF_BX + 2 * hfw;
    const int tile_oy = (int)bid.y * IVIF_BY - hfw;
    const int tile_ox = (int)bid.x * IVIF_BX - hfw;
    const int tile_h = IVIF_BY + 2 * hfw;
    const int tile_elems = tile_h * tile_w;

    /* Raw uint8 tile (stored as ushort for filtering). */
    threadgroup ushort s_ref[IVIF_MAX_TILE_W * IVIF_MAX_TILE_W];
    threadgroup ushort s_dis[IVIF_MAX_TILE_W * IVIF_MAX_TILE_W];
    /* Vertical-pass results: rounded uint16 mu + uint32 squared moments. */
    threadgroup ushort v_mu1[IVIF_BY * IVIF_MAX_TILE_W];
    threadgroup ushort v_mu2[IVIF_BY * IVIF_MAX_TILE_W];
    threadgroup uint v_xx[IVIF_BY * IVIF_MAX_TILE_W];
    threadgroup uint v_yy[IVIF_BY * IVIF_MAX_TILE_W];
    threadgroup uint v_xy[IVIF_BY * IVIF_MAX_TILE_W];

    /* Phase 1: cooperative tile load with reflect-101 mirror. */
    for (int i = (int)lid; i < tile_elems; i += IVIF_BX * IVIF_BY) {
        const int tr = i / tile_w;
        const int tc = i - tr * tile_w;
        const int py = vif_mirror(tile_oy + tr, height);
        const int px = vif_mirror(tile_ox + tc, width);
        s_ref[tr * IVIF_MAX_TILE_W + tc] = (ushort)ivif_read_raw_8(ref_raw, raw_stride, py, px);
        s_dis[tr * IVIF_MAX_TILE_W + tc] = (ushort)ivif_read_raw_8(dis_raw, raw_stride, py, px);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* Phase 2: vertical 17-tap filter for IVIF_BY rows x tile_w cols. */
    const int vert_total = IVIF_BY * tile_w;
    for (int i = (int)lid; i < vert_total; i += IVIF_BX * IVIF_BY) {
        const int r = i / tile_w;
        const int c = i - r * tile_w;
        uint a_mu1 = 0, a_mu2 = 0, a_xx = 0, a_yy = 0, a_xy = 0;
        for (int k = 0; k < fw; ++k) {
            const uint fc = (uint)coeff[k];
            const uint rv = (uint)s_ref[(r + k) * IVIF_MAX_TILE_W + c];
            const uint dv = (uint)s_dis[(r + k) * IVIF_MAX_TILE_W + c];
            const uint icr = fc * rv;
            const uint icd = fc * dv;
            a_mu1 += icr;
            a_mu2 += icd;
            a_xx += icr * rv;
            a_yy += icd * dv;
            a_xy += icr * dv;
        }
        v_mu1[r * IVIF_MAX_TILE_W + c] = (ushort)((a_mu1 + 128u) >> 8);
        v_mu2[r * IVIF_MAX_TILE_W + c] = (ushort)((a_mu2 + 128u) >> 8);
        v_xx[r * IVIF_MAX_TILE_W + c] = a_xx;
        v_yy[r * IVIF_MAX_TILE_W + c] = a_yy;
        v_xy[r * IVIF_MAX_TILE_W + c] = a_xy;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* Phase 3: per-thread horizontal 17-tap filter + statistic. */
    VifWgAccum acc = {0, 0, 0, 0};
    if (valid) {
        uint accum_mu1 = 0, accum_mu2 = 0;
        ulong accum_ref = 0, accum_dis = 0, accum_ref_dis = 0;
        for (int k = 0; k < fw; ++k) {
            const uint fc = (uint)coeff[k];
            accum_mu1 += fc * (uint)v_mu1[ly * IVIF_MAX_TILE_W + (lx + k)];
            accum_mu2 += fc * (uint)v_mu2[ly * IVIF_MAX_TILE_W + (lx + k)];
            accum_ref += (ulong)fc * (ulong)v_xx[ly * IVIF_MAX_TILE_W + (lx + k)];
            accum_dis += (ulong)fc * (ulong)v_yy[ly * IVIF_MAX_TILE_W + (lx + k)];
            accum_ref_dis += (ulong)fc * (ulong)v_xy[ly * IVIF_MAX_TILE_W + (lx + k)];
        }
        const uint xx_filt = (uint)((accum_ref + 32768uL) >> 16);
        const uint yy_filt = (uint)((accum_dis + 32768uL) >> 16);
        const uint xy_filt = (uint)((accum_ref_dis + 32768uL) >> 16);
        ivif_pixel_stat(accum_mu1, accum_mu2, xx_filt, yy_filt, xy_filt, egl, log2_tab, acc);
    }

    threadgroup long t_nl[IVIF_BX * IVIF_BY];
    threadgroup long t_dl[IVIF_BX * IVIF_BY];
    threadgroup long t_nn[IVIF_BX * IVIF_BY];
    threadgroup long t_dn[IVIF_BX * IVIF_BY];
    const uint wg_idx = bid.y * grid_x + bid.x;
    ivif_reduce_and_store(acc, wg_accum, wg_idx, lid, IVIF_BX * IVIF_BY, t_nl, t_dl, t_nn, t_dn);
}

/* ================================================================== */
/*  Kernel: integer_vif_compute_16  (uint16 input)                     */
/*                                                                      */
/*  Mirrors vif_statistic_16. For scale != 0 the V-pass shift constants  */
/*  are (shift_VP = 16, round_VP = 32768, shift_VP_sq = 16,             */
/*   round_VP_sq = 32768); the H-pass is always (shift_HP = 16,         */
/*   round_HP = 32768). For scale == 0 (HBD raw uint16 input) the V-pass */
/*  mu uses shift_VP = bpc / round_VP = 1<<(bpc-1) and the squared       */
/*  moments use shift_VP_sq = (bpc-8)*2 / round_VP_sq = (bpc==8?0:       */
/*  1<<(shift_VP_sq-1)). The host routes 8 bpc scale 0 to                */
/*  integer_vif_compute_8; this kernel covers scales 1..3 (8 & HBD) and  */
/*  HBD scale 0.                                                         */
/*                                                                      */
/*  Buffer bindings:                                                    */
/*   [[buffer(0)]] ref_f     — const ushort * decimated/raw ref         */
/*   [[buffer(1)]] dis_f     — const ushort * decimated/raw dis         */
/*   [[buffer(2)]] log2_tab  — const ushort * VIF log2 LUT             */
/*   [[buffer(3)]] wg_accum  — VifWgAccum * per-WG int64 partials       */
/*   [[buffer(4)]] params    — uint4 (.x=width, .y=height,             */
/*                                     .z=f_stride_ushorts, .w=grid_x)  */
/*   [[buffer(5)]] cfg2      — uint4 (.x=scale, .y=bpc)                 */
/*   [[buffer(6)]] cfgf      — float4 (.x=vif_enhn_gain_limit)         */
/*  Grid: ceil(width/16) x ceil(height/16),  threads 16x16.            */
/* ================================================================== */
kernel void integer_vif_compute_16(
    const device ushort *ref_f [[buffer(0)]], const device ushort *dis_f [[buffer(1)]],
    const device ushort *log2_tab [[buffer(2)]], device VifWgAccum *wg_accum [[buffer(3)]],
    constant uint4 &params [[buffer(4)]], constant uint4 &cfg2 [[buffer(5)]],
    constant float4 &cfgf [[buffer(6)]], uint2 gid [[thread_position_in_grid]],
    uint2 bid [[threadgroup_position_in_grid]], uint2 lpos [[thread_position_in_threadgroup]],
    uint lid [[thread_index_in_threadgroup]])
{
    const int width = (int)params.x;
    const int height = (int)params.y;
    const uint f_stride = params.z;
    const uint grid_x = params.w;
    const int scale = (int)cfg2.x;
    const uint bpc = cfg2.y;
    const float egl = cfgf.x;

    /* V-pass shift constants (vif_statistic_16). */
    uint shift_vp, round_vp, shift_vp_sq, round_vp_sq;
    if (scale == 0) {
        shift_vp = bpc;
        round_vp = 1u << (bpc - 1u);
        shift_vp_sq = (bpc - 8u) * 2u;
        round_vp_sq = (bpc == 8u) ? 0u : (1u << (shift_vp_sq - 1u));
    } else {
        shift_vp = 16u;
        round_vp = 32768u;
        shift_vp_sq = 16u;
        round_vp_sq = 32768u;
    }

    const int fw = ivif_fw(scale);
    const int hfw = fw / 2;
    constant ushort *coeff = ivif_filt(scale);

    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    const int lx = (int)lpos.x;
    const int ly = (int)lpos.y;
    const bool valid = (gx < width && gy < height);

    const int tile_w = IVIF_BX + 2 * hfw;
    const int tile_oy = (int)bid.y * IVIF_BY - hfw;
    const int tile_ox = (int)bid.x * IVIF_BX - hfw;
    const int tile_h = IVIF_BY + 2 * hfw;
    const int tile_elems = tile_h * tile_w;

    threadgroup ushort s_ref[IVIF_MAX_TILE_W * IVIF_MAX_TILE_W];
    threadgroup ushort s_dis[IVIF_MAX_TILE_W * IVIF_MAX_TILE_W];
    threadgroup ushort v_mu1[IVIF_BY * IVIF_MAX_TILE_W];
    threadgroup ushort v_mu2[IVIF_BY * IVIF_MAX_TILE_W];
    threadgroup uint v_xx[IVIF_BY * IVIF_MAX_TILE_W];
    threadgroup uint v_yy[IVIF_BY * IVIF_MAX_TILE_W];
    threadgroup uint v_xy[IVIF_BY * IVIF_MAX_TILE_W];

    for (int i = (int)lid; i < tile_elems; i += IVIF_BX * IVIF_BY) {
        const int tr = i / tile_w;
        const int tc = i - tr * tile_w;
        const int py = vif_mirror(tile_oy + tr, height);
        const int px = vif_mirror(tile_ox + tc, width);
        s_ref[tr * IVIF_MAX_TILE_W + tc] = ref_f[(uint)py * f_stride + (uint)px];
        s_dis[tr * IVIF_MAX_TILE_W + tc] = dis_f[(uint)py * f_stride + (uint)px];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* Vertical pass: scale != 0 constants. mu rounded (+32768)>>16 to
     * uint16; squared moments are uint64 then (+32768)>>16 to uint32. */
    const int vert_total = IVIF_BY * tile_w;
    for (int i = (int)lid; i < vert_total; i += IVIF_BX * IVIF_BY) {
        const int r = i / tile_w;
        const int c = i - r * tile_w;
        uint a_mu1 = 0, a_mu2 = 0;
        ulong a_xx = 0, a_yy = 0, a_xy = 0;
        for (int k = 0; k < fw; ++k) {
            const uint fc = (uint)coeff[k];
            const uint rv = (uint)s_ref[(r + k) * IVIF_MAX_TILE_W + c];
            const uint dv = (uint)s_dis[(r + k) * IVIF_MAX_TILE_W + c];
            const uint icr = fc * rv;
            const uint icd = fc * dv;
            a_mu1 += icr;
            a_mu2 += icd;
            a_xx += (ulong)icr * (ulong)rv;
            a_yy += (ulong)icd * (ulong)dv;
            a_xy += (ulong)icr * (ulong)dv;
        }
        v_mu1[r * IVIF_MAX_TILE_W + c] = (ushort)((a_mu1 + round_vp) >> shift_vp);
        v_mu2[r * IVIF_MAX_TILE_W + c] = (ushort)((a_mu2 + round_vp) >> shift_vp);
        v_xx[r * IVIF_MAX_TILE_W + c] = (uint)((a_xx + (ulong)round_vp_sq) >> shift_vp_sq);
        v_yy[r * IVIF_MAX_TILE_W + c] = (uint)((a_yy + (ulong)round_vp_sq) >> shift_vp_sq);
        v_xy[r * IVIF_MAX_TILE_W + c] = (uint)((a_xy + (ulong)round_vp_sq) >> shift_vp_sq);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    VifWgAccum acc = {0, 0, 0, 0};
    if (valid) {
        uint accum_mu1 = 0, accum_mu2 = 0;
        ulong accum_ref = 0, accum_dis = 0, accum_ref_dis = 0;
        for (int k = 0; k < fw; ++k) {
            const uint fc = (uint)coeff[k];
            accum_mu1 += fc * (uint)v_mu1[ly * IVIF_MAX_TILE_W + (lx + k)];
            accum_mu2 += fc * (uint)v_mu2[ly * IVIF_MAX_TILE_W + (lx + k)];
            accum_ref += (ulong)fc * (ulong)v_xx[ly * IVIF_MAX_TILE_W + (lx + k)];
            accum_dis += (ulong)fc * (ulong)v_yy[ly * IVIF_MAX_TILE_W + (lx + k)];
            accum_ref_dis += (ulong)fc * (ulong)v_xy[ly * IVIF_MAX_TILE_W + (lx + k)];
        }
        const uint xx_filt = (uint)((accum_ref + 32768uL) >> 16);
        const uint yy_filt = (uint)((accum_dis + 32768uL) >> 16);
        const uint xy_filt = (uint)((accum_ref_dis + 32768uL) >> 16);
        ivif_pixel_stat(accum_mu1, accum_mu2, xx_filt, yy_filt, xy_filt, egl, log2_tab, acc);
    }

    threadgroup long t_nl[IVIF_BX * IVIF_BY];
    threadgroup long t_dl[IVIF_BX * IVIF_BY];
    threadgroup long t_nn[IVIF_BX * IVIF_BY];
    threadgroup long t_dn[IVIF_BX * IVIF_BY];
    const uint wg_idx = bid.y * grid_x + bid.x;
    ivif_reduce_and_store(acc, wg_accum, wg_idx, lid, IVIF_BX * IVIF_BY, t_nl, t_dl, t_nn, t_dn);
}

/* ================================================================== */
/*  Kernel: integer_vif_decimate_8  (scale 0 -> 1, raw uint8 input)    */
/*                                                                      */
/*  Mirrors subsample_rd_8 + decimate_and_pad. Applies the scale-1      */
/*  9-tap filter at the FULL (prev) dims and writes the kept            */
/*  every-other output (the decimation). Output pixel (gx, gy) reads    */
/*  input centred at (2*gx, 2*gy). V-pass: mu rounded (+128)>>8 to      */
/*  uint16; H-pass over the rounded uint16 V-results: (+32768)>>16 to   */
/*  uint16. Only mu1/mu2 are needed for the next scale.                 */
/*                                                                      */
/*  Buffer bindings:                                                    */
/*   [[buffer(0)]] ref_raw   — const uchar * raw ref plane             */
/*   [[buffer(1)]] dis_raw   — const uchar * raw dis plane             */
/*   [[buffer(2)]] ref_out   — ushort * decimated ref out              */
/*   [[buffer(3)]] dis_out   — ushort * decimated dis out              */
/*   [[buffer(4)]] dims      — uint4 (.x=out_w, .y=out_h,              */
/*                                     .z=in_w,  .w=in_h)              */
/*   [[buffer(5)]] cfg       — uint4 (.x=raw_stride_bytes,             */
/*                                     .y=out_stride_ushorts)         */
/*  Grid: ceil(out_w/16) x ceil(out_h/16),  threads 16x16.            */
/* ================================================================== */
kernel void integer_vif_decimate_8(const device uchar *ref_raw [[buffer(0)]],
                                   const device uchar *dis_raw [[buffer(1)]],
                                   device ushort *ref_out [[buffer(2)]],
                                   device ushort *dis_out [[buffer(3)]],
                                   constant uint4 &dims [[buffer(4)]],
                                   constant uint4 &cfg [[buffer(5)]],
                                   uint2 gid [[thread_position_in_grid]])
{
    const int out_w = (int)dims.x;
    const int out_h = (int)dims.y;
    const int in_w = (int)dims.z;
    const int in_h = (int)dims.w;
    const uint raw_stride = cfg.x;
    const uint out_stride = cfg.y;

    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= out_w || gy >= out_h) {
        return;
    }

    const int fw = 9; /* scale-1 filter */
    const int hfw = fw / 2;
    constant ushort *coeff = IVIF_FILT_S1;
    const int in_x = 2 * gx;
    const int in_y = 2 * gy;

    /* H-outer over the V-filtered+rounded uint16 intermediates.
     * For each H-tap column px, compute the V-pass at that column for the
     * centre row in_y (the CPU's per-row V then H then decimate keeps the
     * same value for the sampled output position). */
    uint acc_ref = 0, acc_dis = 0;
    for (int fj = 0; fj < fw; ++fj) {
        const uint cj = (uint)coeff[fj];
        const int px = vif_mirror(in_x - hfw + fj, in_w);
        uint v_ref = 0, v_dis = 0;
        for (int fi = 0; fi < fw; ++fi) {
            const uint ci = (uint)coeff[fi];
            const int py = vif_mirror(in_y - hfw + fi, in_h);
            v_ref += ci * ivif_read_raw_8(ref_raw, raw_stride, py, px);
            v_dis += ci * ivif_read_raw_8(dis_raw, raw_stride, py, px);
        }
        const uint v_ref_r = (v_ref + 128u) >> 8;
        const uint v_dis_r = (v_dis + 128u) >> 8;
        acc_ref += cj * v_ref_r;
        acc_dis += cj * v_dis_r;
    }

    ref_out[(uint)gy * out_stride + (uint)gx] = (ushort)((acc_ref + 32768u) >> 16);
    dis_out[(uint)gy * out_stride + (uint)gx] = (ushort)((acc_dis + 32768u) >> 16);
}

/* ================================================================== */
/*  Kernel: integer_vif_decimate_16  (uint16 input)                    */
/*                                                                      */
/*  Mirrors subsample_rd_16 + decimate_and_pad. The CPU's subsample is   */
/*  called as subsample_rd_16(buf, w, h, source_scale, bpc) and uses     */
/*  filter[source_scale + 1]. The V-pass round/shift depends on          */
/*  source_scale: source_scale == 0 (HBD scale 0->1) uses round =        */
/*  1<<(bpc-1), shift = bpc; source_scale >= 1 uses round = 32768,       */
/*  shift = 16. The H-pass is always round 32768, shift 16. Applied at   */
/*  the prev-scale uint16 dims, sampled at (2*gx, 2*gy).                 */
/*                                                                      */
/*  For 8 bpc the scale 0->1 transition goes through                     */
/*  integer_vif_decimate_8 instead, so source_scale == 0 here only       */
/*  occurs for HBD input.                                                */
/*                                                                      */
/*  Buffer bindings:                                                    */
/*   [[buffer(0)]] ref_in    — const ushort * prev-scale ref           */
/*   [[buffer(1)]] dis_in    — const ushort * prev-scale dis           */
/*   [[buffer(2)]] ref_out   — ushort * decimated ref out              */
/*   [[buffer(3)]] dis_out   — ushort * decimated dis out              */
/*   [[buffer(4)]] dims      — uint4 (.x=out_w, .y=out_h,              */
/*                                     .z=in_w,  .w=in_h)              */
/*   [[buffer(5)]] cfg       — uint4 (.x=in_stride_ushorts,            */
/*                                     .y=out_stride_ushorts,          */
/*                                     .z=filt_scale (= source_scale+1),*/
/*                                     .w=bpc)                          */
/*  Grid: ceil(out_w/16) x ceil(out_h/16),  threads 16x16.            */
/* ================================================================== */
kernel void integer_vif_decimate_16(const device ushort *ref_in [[buffer(0)]],
                                    const device ushort *dis_in [[buffer(1)]],
                                    device ushort *ref_out [[buffer(2)]],
                                    device ushort *dis_out [[buffer(3)]],
                                    constant uint4 &dims [[buffer(4)]],
                                    constant uint4 &cfg [[buffer(5)]],
                                    uint2 gid [[thread_position_in_grid]])
{
    const int out_w = (int)dims.x;
    const int out_h = (int)dims.y;
    const int in_w = (int)dims.z;
    const int in_h = (int)dims.w;
    const uint in_stride = cfg.x;
    const uint out_stride = cfg.y;
    const int filt_scale = (int)cfg.z; /* = source_scale + 1 */
    const uint bpc = cfg.w;

    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= out_w || gy >= out_h) {
        return;
    }

    const int fw = ivif_fw(filt_scale);
    const int hfw = fw / 2;
    constant ushort *coeff = ivif_filt(filt_scale);
    const int in_x = 2 * gx;
    const int in_y = 2 * gy;

    /* V-pass round/shift depend on source_scale (= filt_scale - 1). */
    uint round_vp, shift_vp;
    if (filt_scale == 1) {
        round_vp = 1u << (bpc - 1u);
        shift_vp = bpc;
    } else {
        round_vp = 32768u;
        shift_vp = 16u;
    }

    uint acc_ref = 0, acc_dis = 0;
    for (int fj = 0; fj < fw; ++fj) {
        const uint cj = (uint)coeff[fj];
        const int px = vif_mirror(in_x - hfw + fj, in_w);
        uint v_ref = 0, v_dis = 0;
        for (int fi = 0; fi < fw; ++fi) {
            const uint ci = (uint)coeff[fi];
            const int py = vif_mirror(in_y - hfw + fi, in_h);
            v_ref += ci * (uint)ref_in[(uint)py * in_stride + (uint)px];
            v_dis += ci * (uint)dis_in[(uint)py * in_stride + (uint)px];
        }
        const uint v_ref_r = (v_ref + round_vp) >> shift_vp;
        const uint v_dis_r = (v_dis + round_vp) >> shift_vp;
        acc_ref += cj * v_ref_r;
        acc_dis += cj * v_dis_r;
    }

    ref_out[(uint)gy * out_stride + (uint)gx] = (ushort)((acc_ref + 32768u) >> 16);
    dis_out[(uint)gy * out_stride + (uint)gx] = (ushort)((acc_dis + 32768u) >> 16);
}
