// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Daubechies-2 DWT of the integer ADM pipeline, ported statement by statement
// from core/src/feature/integer_adm.c (dwt2_src_indices_1d(), adm_dwt2_8(),
// adm_dwt2_16(), the `_lo` variants, i16_to_i32(), adm_dwt2_s123_combined())
// and integer_adm_kernels.h (the vertical / horizontal pass kernels).

use vmafx_fex::PlaneView;

use crate::bands::{Bands, IndexTable};
use crate::tables::{DWT_HI, DWT_HI_SUM, DWT_LO, DWT_LO_SUM};

/// `dwt2_src_indices_1d()`: symmetric-extension source indices of the four
/// taps for every output sample of a dimension of `n` samples. `n` >= 3 (the
/// extractor refuses frames below 17, whose scale-3 input is 3 samples).
pub fn src_indices(ind: &mut IndexTable, n: usize) {
    let n_half = n.div_ceil(2);
    ind[0][0] = 1;
    ind[1][0] = 0;
    ind[2][0] = 1;
    ind[3][0] = 2;
    // `i + 2 < n_half`, as the C writes it.
    let mut i = 1;
    while i + 2 < n_half {
        let ind1 = 2 * i;
        ind[0][i] = ind1 - 1;
        ind[1][i] = ind1;
        ind[2][i] = ind1 + 1;
        ind[3][i] = ind1 + 2;
        i += 1;
    }
    let tail = if n_half > 2 { n_half - 2 } else { 1 };
    for i in tail..n_half {
        let ind1 = 2 * i;
        let mirror = |x: usize| if x >= n { 2 * n - x - 1 } else { x };
        set_taps(
            ind,
            i,
            [
                mirror(ind1 - 1),
                mirror(ind1),
                mirror(ind1 + 1),
                mirror(ind1 + 2),
            ],
        );
    }
}

/// Store the four tap indices of output `i`.
fn set_taps(ind: &mut IndexTable, i: usize, taps: [usize; 4]) {
    for (k, t) in taps.into_iter().enumerate() {
        ind[k][i] = t;
    }
}

/// `adm_dwt2_tap4()`: four-tap response accumulated in int32.
fn tap4(filter: &[i16; 4], s: [i32; 4]) -> i32 {
    let mut accum = 0_i32;
    accum += i32::from(filter[0]) * s[0];
    accum += i32::from(filter[1]) * s[1];
    accum += i32::from(filter[2]) * s[2];
    accum += i32::from(filter[3]) * s[3];
    accum
}

/// The four input indices output `i` reads.
fn taps(ind: &IndexTable, i: usize) -> [usize; 4] {
    [ind[0][i], ind[1][i], ind[2][i], ind[3][i]]
}

/// C `(int16_t)x`: the narrowing conversion GCC defines as modulo 2^16.
#[allow(clippy::cast_possible_truncation)]
const fn narrow16(x: i32) -> i16 {
    x as i16
}

/// C `(int32_t)x` of an int64 value: modulo 2^32.
#[allow(clippy::cast_possible_truncation)]
const fn narrow32(x: i64) -> i32 {
    x as i32
}

/// `adm_dwt2_vpass_8()` over columns [0, w): low-pass into `lo` and, unless
/// `hi` is empty, high-pass into `hi`.
fn vpass_u8(src: &PlaneView<'_, u8>, rows: [usize; 4], lo: &mut [i16], hi: &mut [i16]) {
    const SHIFT_VP: i32 = 8;
    const ADD_SHIFT_VP: i32 = 128;
    let r = rows.map(|y| src.row(y));
    for (j, out) in lo.iter_mut().enumerate() {
        let s = [r[0][j], r[1][j], r[2][j], r[3][j]].map(i32::from);
        let mut accum = tap4(&DWT_LO, s);
        accum -= DWT_LO_SUM * ADD_SHIFT_VP;
        *out = narrow16((accum + ADD_SHIFT_VP) >> SHIFT_VP);
    }
    for (j, out) in hi.iter_mut().enumerate() {
        let s = [r[0][j], r[1][j], r[2][j], r[3][j]].map(i32::from);
        let mut accum = tap4(&DWT_HI, s);
        accum -= DWT_HI_SUM * ADD_SHIFT_VP;
        *out = narrow16((accum + ADD_SHIFT_VP) >> SHIFT_VP);
    }
}

/// `adm_dwt2_vpass16_tap4()`: the response in int64 (a bright 16-bit column
/// overflows int32), normalised, rounded, narrowed to int32.
fn vpass16_tap4(filter: &[i16; 4], filter_sum: i32, s: [u16; 4], add: i32, shift: u32) -> i32 {
    let mut accum = i64::from(filter[0]) * i64::from(s[0]);
    accum += i64::from(filter[1]) * i64::from(s[1]);
    accum += i64::from(filter[2]) * i64::from(s[2]);
    accum += i64::from(filter[3]) * i64::from(s[3]);
    accum -= i64::from(filter_sum) * i64::from(add);
    narrow32((accum + i64::from(add)) >> shift)
}

/// `adm_dwt2_vpass_16()`: the normalisation shift follows the bit depth.
fn vpass_u16(src: &PlaneView<'_, u16>, rows: [usize; 4], bpc: u32, lo: &mut [i16], hi: &mut [i16]) {
    let shift = bpc;
    let add = 1_i32 << (bpc - 1);
    let r = rows.map(|y| src.row(y));
    for (j, out) in lo.iter_mut().enumerate() {
        let s = [r[0][j], r[1][j], r[2][j], r[3][j]];
        *out = narrow16(vpass16_tap4(&DWT_LO, DWT_LO_SUM, s, add, shift));
    }
    for (j, out) in hi.iter_mut().enumerate() {
        let s = [r[0][j], r[1][j], r[2][j], r[3][j]];
        *out = narrow16(vpass16_tap4(&DWT_HI, DWT_HI_SUM, s, add, shift));
    }
}

/// `adm_dwt2_hpass()` of output row `i`: low-pass of `lo` into band_a and,
/// unless `hi` is empty, the three detail bands.
fn hpass16(
    lo: &[i16],
    hi: &[i16],
    dst: &mut Bands<i16>,
    ind_x: &IndexTable,
    row: usize,
    w_half: usize,
) {
    const SHIFT_HP: i32 = 16;
    const ADD_SHIFT_HP: i32 = 32768;
    let round = |accum: i32| narrow16((accum + ADD_SHIFT_HP) >> SHIFT_HP);
    for j in 0..w_half {
        let jx = taps(ind_x, j);
        let sl = jx.map(|x| i32::from(lo[x]));
        dst.a[row + j] = round(tap4(&DWT_LO, sl));
        if hi.is_empty() {
            continue;
        }
        let sh = jx.map(|x| i32::from(hi[x]));
        dst.hvd[1][row + j] = round(tap4(&DWT_HI, sl));
        dst.hvd[0][row + j] = round(tap4(&DWT_LO, sh));
        dst.hvd[2][row + j] = round(tap4(&DWT_HI, sh));
    }
}

/// The luma plane of a scale-0 DWT.
#[derive(Clone, Copy)]
pub enum Source<'a> {
    U8(PlaneView<'a, u8>),
    U16(PlaneView<'a, u16>, u32),
}

/// Geometry of one DWT call: input `w` x `h`, band element `stride`.
#[derive(Clone, Copy)]
pub struct DwtGeom {
    pub w: usize,
    pub h: usize,
    pub stride: usize,
}

/// `adm_dwt2_8()` / `adm_dwt2_16()` and, with `lo_only`, their `_lo`
/// variants (band_a only, for `adm_skip_scale0`).
pub fn dwt_scale0(
    src: Source<'_>,
    dst: &mut Bands<i16>,
    tmp: &mut [i16],
    ind: (&IndexTable, &IndexTable),
    g: DwtGeom,
    lo_only: bool,
) {
    let (ind_y, ind_x) = ind;
    let (lo, rest) = tmp.split_at_mut(g.w);
    let hi = if lo_only {
        &mut rest[..0]
    } else {
        &mut rest[..g.w]
    };
    for i in 0..g.h.div_ceil(2) {
        let rows = taps(ind_y, i);
        match src {
            Source::U8(p) => vpass_u8(&p, rows, lo, hi),
            Source::U16(p, bpc) => vpass_u16(&p, rows, bpc, lo, hi),
        }
        hpass16(lo, hi, dst, ind_x, i * g.stride, g.w.div_ceil(2));
    }
}

/// `i16_to_i32()`: widen band_a of the scale-0 output for scale 1.
pub fn widen_band_a(src: &[i16], dst: &mut [i32], g: DwtGeom) {
    for i in 0..g.h.div_ceil(2) {
        let off = i * g.stride;
        let w_half = g.w.div_ceil(2);
        for (d, s) in dst[off..off + w_half]
            .iter_mut()
            .zip(&src[off..off + w_half])
        {
            *d = i32::from(*s);
        }
    }
}

/// `I4Dwt2Round` of `i4_dwt2_round()`: (add, shift) of the vertical and the
/// horizontal pass of scale 1..3.
fn round_terms(scale: usize) -> ((i32, u32), (i32, u32)) {
    const ADD_VP: [i32; 3] = [0, 32768, 32768];
    const ADD_HP: [i32; 3] = [16384, 32768, 16384];
    const SHIFT_VP: [u32; 3] = [0, 16, 16];
    const SHIFT_HP: [u32; 3] = [15, 16, 15];
    let slot = scale - 1;
    (
        (ADD_VP[slot], SHIFT_VP[slot]),
        (ADD_HP[slot], SHIFT_HP[slot]),
    )
}

/// `i4_dwt2_tap4()`: int64 accumulation, rounded back to 32 bits.
fn i4_tap4(filter: &[i16; 4], s: [i32; 4], add: i32, shift: u32) -> i32 {
    let mut accum = 0_i64;
    accum += i64::from(filter[0]) * i64::from(s[0]);
    accum += i64::from(filter[1]) * i64::from(s[1]);
    accum += i64::from(filter[2]) * i64::from(s[2]);
    accum += i64::from(filter[3]) * i64::from(s[3]);
    narrow32((accum + i64::from(add)) >> shift)
}

/// `i4_dwt2_vpass()` of one plane: `lo` and `hi` are `w` samples each.
fn i4_vpass(
    src: &[i32],
    rows: [usize; 4],
    stride: usize,
    lo: &mut [i32],
    hi: &mut [i32],
    r: (i32, u32),
) {
    let base = rows.map(|y| y * stride);
    for (j, (l, h)) in lo.iter_mut().zip(hi.iter_mut()).enumerate() {
        let s = base.map(|b| src[b + j]);
        *l = i4_tap4(&DWT_LO, s, r.0, r.1);
        *h = i4_tap4(&DWT_HI, s, r.0, r.1);
    }
}

/// `i4_dwt2_hpass_bands()` over the output columns of row offset `row`.
fn i4_hpass(
    lo: &[i32],
    hi: &[i32],
    dst: &mut Bands<i32>,
    ind_x: &IndexTable,
    row: usize,
    w_half: usize,
    r: (i32, u32),
) {
    for j in 0..w_half {
        let jx = taps(ind_x, j);
        let sl = jx.map(|x| lo[x]);
        dst.a[row + j] = i4_tap4(&DWT_LO, sl, r.0, r.1);
        dst.hvd[1][row + j] = i4_tap4(&DWT_HI, sl, r.0, r.1);
        let sh = jx.map(|x| hi[x]);
        dst.hvd[0][row + j] = i4_tap4(&DWT_LO, sh, r.0, r.1);
        dst.hvd[2][row + j] = i4_tap4(&DWT_HI, sh, r.0, r.1);
    }
}

/// `adm_dwt2_s123_combined()`: the 32-bit DWT of band_a of both planes. Each
/// output row is formed from rows of band_a that no later output row reads
/// once it is overwritten, so the transform runs in place as in C.
pub fn dwt_s123(
    ref_dwt: &mut Bands<i32>,
    dis_dwt: &mut Bands<i32>,
    tmp: &mut [i32],
    ind: (&IndexTable, &IndexTable),
    g: DwtGeom,
    scale: usize,
) {
    let (ind_y, ind_x) = ind;
    let (vp, hp) = round_terms(scale);
    let (lo_ref, rest) = tmp.split_at_mut(g.w);
    let (hi_ref, rest) = rest.split_at_mut(g.w);
    let (lo_dis, rest) = rest.split_at_mut(g.w);
    let hi_dis = &mut rest[..g.w];
    for i in 0..g.h.div_ceil(2) {
        let rows = taps(ind_y, i);
        i4_vpass(&ref_dwt.a, rows, g.stride, lo_ref, hi_ref, vp);
        i4_vpass(&dis_dwt.a, rows, g.stride, lo_dis, hi_dis, vp);
        let row = i * g.stride;
        i4_hpass(lo_ref, hi_ref, ref_dwt, ind_x, row, g.w.div_ceil(2), hp);
        i4_hpass(lo_dis, hi_dis, dis_dwt, ind_x, row, g.w.div_ceil(2), hp);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn table(n: usize) -> IndexTable {
        let half = n.div_ceil(2);
        let mut t: IndexTable = [vec![0; half], vec![0; half], vec![0; half], vec![0; half]];
        src_indices(&mut t, n);
        t
    }

    #[test]
    fn three_sample_dimension_mirrors_inside() {
        let t = table(3);
        assert_eq!([t[0][0], t[1][0], t[2][0], t[3][0]], [1, 0, 1, 2]);
        assert_eq!([t[0][1], t[1][1], t[2][1], t[3][1]], [1, 2, 2, 1]);
    }

    #[test]
    fn interior_and_tail_follow_the_c_bounds() {
        let t = table(10);
        assert_eq!([t[0][2], t[1][2], t[2][2], t[3][2]], [3, 4, 5, 6]);
        assert_eq!([t[0][4], t[1][4], t[2][4], t[3][4]], [7, 8, 9, 9]);
    }
}
