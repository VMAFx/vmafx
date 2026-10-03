/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernels for the float_adm feature extractor.
 *  Port of the CUDA / SYCL twins (`cuda/float_adm/float_adm_score.cu`,
 *  `sycl/float_adm_sycl.cpp`, ADR-1420 / ADR-1434) to MSL: the CPU's float ADM
 *  arithmetic operation for operation (adm_tools.c), summed in the CPU's
 *  order, so the scores equal the CPU's bit for bit (ADR-1498).
 *
 *  Five kernel functions, dispatched in order by `float_adm_metal.mm` once
 *  per scale (4 scales total):
 *
 *    Stage 0 -- float_adm_dwt_vert_{8,16}bpc
 *        9/7-tap DWT vertical pass (separable lo/hi). At scale 0 reads the
 *        raw u8/u16 source plane (the only stage that is bpc-specific --
 *        hence the two variants); at scales >0 reads the parent LL band.
 *        Emits lo/hi sub-rows into a packed dwt_tmp scratch.
 *        Grid: ceil(cur_w/16) x ceil(half_h/16), 16x16 threads, 2 z-slices
 *        (z=0 ref, z=1 dis) flattened to a `plane_is_dis` flag.
 *
 *    Stage 1 -- float_adm_dwt_hori
 *        DWT horizontal pass -- reads lo/hi sub-rows, emits 4 sub-bands
 *        (a=LL, h=HL, v=LH, d=HH) packed contiguous in ref_band/dis_band.
 *        Grid: ceil(half_w/16) x ceil(half_h/16), 16x16, 2 z-slices.
 *
 *    Stage 2 -- float_adm_decouple
 *        adm_decouple_s() and both adm_csf_s() calls of compute_adm() for one
 *        sample: the CSF-weighted additive (csf_a) and restored (csf_r)
 *        signals of the three sub-bands, each with its filtered magnitude
 *        (csf_fa, csf_fr). Per-sample arithmetic: metal_float_adm_math.h.
 *
 *    Stage 3 -- float_adm_terms
 *        The terms adm_csf_den_scale_s() and the two adm_cm_s() calls of
 *        compute_adm() accumulate for one sample of the reduced region, one
 *        float per (slot, band) stored at the sample's own position: no
 *        per-block or per-simd sum of the terms exists.
 *
 *    Stage 4 -- float_adm_rows
 *        One work-item per (slot, row) adds the terms of its row left to
 *        right in one fp32 accumulator. The host adds the rows of a slot top
 *        to bottom in fp32 (adm_fold3_s()) and pools with the CPU's own
 *        adm_pool_bands_s().
 *
 *  Numeric design notes:
 *   - Every arithmetic statement of stages 2 and 3 is in
 *     metal_float_adm_math.h; the three fp64 expressions of the reference are
 *     exact fp32 pairs and 64-bit integer replays (no fp64 type on Metal).
 *   - The decouple quotient is the plain fp32 `/` (ADR-1442), never a
 *     reciprocal; the kernels are built with -fno-fast-math
 *     -ffp-contract=off (core/src/metal/meson.build).
 *   - No kernel uses scratch memory arrays or atomics.
 *   - Watson-97 CSF (adm_csf_mode 0) only -- the default CPU path. Other
 *     modes are rejected in the .mm init (matches the CUDA twin).
 */

#include <metal_stdlib>
using namespace metal;

#include "metal_float_adm_math.h"

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

/* Geometry uniform of the DWT stages. Packed to keep the setBytes payload
 * small and stable across stages. */
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

/* DWT stage uniform: the raw-plane conversion. */
struct FadmCsf {
    float scaler;       /* >8bpc raw divisor */
    float pixel_offset; /* -128 */
};

/* Both axes use `2*sup - idx - 1` for the over-range mirror and `-idx`
 * for the negative mirror -- matches dwt2_src_indices_filt_s in
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
/*  Stage 2 -- decouple + CSF of both signals.                         */
/*  Band buffers hold their sub-bands back to back, `buf_stride`       */
/*  floats per row and `half_h` rows per sub-band: (a, h, v, d) for    */
/*  the DWT bands, (h, v, d) for the CSF buffers.                      */
/* ------------------------------------------------------------------ */
kernel void float_adm_decouple(const device float *ref_band [[buffer(0)]],
                               const device float *dis_band [[buffer(1)]],
                               device float *csf_a [[buffer(2)]],
                               device float *csf_fa [[buffer(3)]],
                               device float *csf_r [[buffer(4)]],
                               device float *csf_fr [[buffer(5)]],
                               constant VmafMtlFadmDecoupleArgs &a [[buffer(6)]],
                               uint2 gid [[thread_position_in_grid]])
{
    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= a.half_w || gy >= a.half_h) { return; }

    const int slice = a.buf_stride * a.half_h;
    const int at = gy * a.buf_stride + gx;
    const VmafMtlFadmDecouple c = vmaf_mtl_fadm_decouple_sample(
        a.limit, a.cos_1deg_sq, a.rfactor_h, a.rfactor_v, a.rfactor_d, ref_band[slice + at],
        ref_band[2 * slice + at], ref_band[3 * slice + at], dis_band[slice + at],
        dis_band[2 * slice + at], dis_band[3 * slice + at]);

    csf_a[at] = c.h.csf_a;
    csf_fa[at] = c.h.csf_fa;
    csf_r[at] = c.h.csf_r;
    csf_fr[at] = c.h.csf_fr;
    csf_a[slice + at] = c.v.csf_a;
    csf_fa[slice + at] = c.v.csf_fa;
    csf_r[slice + at] = c.v.csf_r;
    csf_fr[slice + at] = c.v.csf_fr;
    csf_a[2 * slice + at] = c.d.csf_a;
    csf_fa[2 * slice + at] = c.d.csf_fa;
    csf_r[2 * slice + at] = c.d.csf_r;
    csf_fr[2 * slice + at] = c.d.csf_fr;
}

/* ------------------------------------------------------------------ */
/*  Stage 3 -- the terms of the reduced region.                        */
/* ------------------------------------------------------------------ */

/* The taps of one band of adm_cm_thresh3x3_s(): the eight filtered
 * neighbours of (w.x, w.y) in row order and the unfiltered centre. `plane`
 * is the offset of the band's first sample. */
inline VmafMtlFadmBandTaps fadm_taps(const device float *flt, const device float *csf, int plane,
                                     int stride, VmafMtlFadmWindow w)
{
    const int above = plane + w.ym * stride;
    const int here = plane + w.y * stride;
    const int below = plane + w.yp * stride;
    const VmafMtlFadmNeighbours n = {flt[above + w.xm], flt[above + w.x], flt[above + w.xp],
                                     flt[here + w.xm],  flt[here + w.xp],  flt[below + w.xm],
                                     flt[below + w.x],  flt[below + w.xp]};
    const VmafMtlFadmBandTaps taps = {n, csf[here + w.x]};
    return taps;
}

/* adm_cm_thresh3x3_s() of one signal: one sum per band, added in band order. */
inline float fadm_threshold(const device float *flt, const device float *csf, int slice,
                                   int stride, VmafMtlFadmWindow w)
{
    return vmaf_mtl_fadm_threshold(fadm_taps(flt, csf, 0, stride, w),
                                   fadm_taps(flt, csf, slice, stride, w),
                                   fadm_taps(flt, csf, 2 * slice, stride, w));
}

kernel void float_adm_terms(const device float *ref_band [[buffer(0)]],
                            const device float *csf_a [[buffer(1)]],
                            const device float *csf_fa [[buffer(2)]],
                            const device float *csf_r [[buffer(3)]],
                            const device float *csf_fr [[buffer(4)]],
                            device float *terms [[buffer(5)]],
                            constant VmafMtlFadmTermArgs &a [[buffer(6)]],
                            uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= a.region_w || gid.y >= a.region_h) { return; }
    const int x = a.left + (int)gid.x;
    const int y = a.top + (int)gid.y;
    const int slice = a.buf_stride * a.half_h;
    const VmafMtlFadmWindow w = vmaf_mtl_fadm_window(x, y, a.half_w, a.half_h);

    float thr_additive = 0.0f;
    float thr_restored = 0.0f;
    if (a.bypass_cm == 0u) {
        thr_additive = fadm_threshold(csf_fa, csf_a, slice, a.buf_stride, w);
        thr_restored = fadm_threshold(csf_fr, csf_r, slice, a.buf_stride, w);
    }

    const bool is_cube = a.is_cube != 0u;
    const int at = y * a.buf_stride + x;
    const VmafMtlFadmBandTerms h = vmaf_mtl_fadm_band_terms(
        a.rfactor_h, ref_band[slice + at], csf_r[at], csf_a[at], thr_additive, thr_restored,
        is_cube, a.p_norm);
    const VmafMtlFadmBandTerms v = vmaf_mtl_fadm_band_terms(
        a.rfactor_v, ref_band[2 * slice + at], csf_r[slice + at], csf_a[slice + at], thr_additive,
        thr_restored, is_cube, a.p_norm);
    const VmafMtlFadmBandTerms d = vmaf_mtl_fadm_band_terms(
        a.rfactor_d, ref_band[3 * slice + at], csf_r[2 * slice + at], csf_a[2 * slice + at],
        thr_additive, thr_restored, is_cube, a.p_norm);

    const uint rx = gid.x;
    const uint ry = gid.y;
    terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_DEN, rx, ry, a.region_w, a.region_h)] = h.den;
    terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_DEN + 1u, rx, ry, a.region_w, a.region_h)] =
        v.den;
    terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_DEN + 2u, rx, ry, a.region_w, a.region_h)] =
        d.den;
    terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_CM, rx, ry, a.region_w, a.region_h)] = h.cm;
    terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_CM + 1u, rx, ry, a.region_w, a.region_h)] =
        v.cm;
    terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_CM + 2u, rx, ry, a.region_w, a.region_h)] =
        d.cm;
    terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_AIM, rx, ry, a.region_w, a.region_h)] = h.aim;
    terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_AIM + 1u, rx, ry, a.region_w, a.region_h)] =
        v.aim;
    terms[vmaf_mtl_fadm_term_index(VMAF_MTL_FADM_SLOT_AIM + 2u, rx, ry, a.region_w, a.region_h)] =
        d.aim;
}

/* ------------------------------------------------------------------ */
/*  Stage 4 -- one row of one slot per work-item: `region_w` terms,    */
/*  `region_h` floats apart, left to right in one fp32 accumulator.    */
/*  The order is the result; do not split, stride or reduce this loop. */
/* ------------------------------------------------------------------ */
kernel void float_adm_rows(const device float *terms [[buffer(0)]],
                           device float *rows [[buffer(1)]],
                           constant VmafMtlFadmRowArgs &a [[buffer(2)]],
                           uint id [[thread_position_in_grid]])
{
    if (id >= VMAF_MTL_FADM_TERM_SLOTS * a.region_h) { return; }
    const uint slot = id / a.region_h;
    const uint y = id - slot * a.region_h;
    float inner = 0.0f;
    for (uint x = 0u; x < a.region_w; ++x) {
        inner += terms[vmaf_mtl_fadm_term_index(slot, x, y, a.region_w, a.region_h)];
    }
    rows[id] = inner;
}
