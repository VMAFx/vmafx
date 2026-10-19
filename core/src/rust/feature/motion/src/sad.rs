// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Ported statement by statement from `motion_score_pipeline_8` and
// `motion_score_pipeline_16` of core/src/feature/integer_motion.c and the
// `filter` table of core/src/feature/integer_motion.h.

//! Sum of absolute differences of the blurred frame difference.

#[cfg(test)]
use vmafx_fex::Error;
use vmafx_fex::{PlaneView, Sample};

/// The 5-tap Gaussian of `integer_motion.h`, verbatim.
pub const FILTER: [u16; 5] = [3571, 16004, 26386, 16004, 3571];
/// `filter_width` of `integer_motion.h`.
const FILTER_WIDTH: usize = FILTER.len();
/// `filter_width / 2`.
const RADIUS: isize = (FILTER_WIDTH / 2) as isize;
/// Smallest frame side the reflect-101 padding supports (`radius + 1`).
pub const MIN_DIM: u32 = 3;

/// C `mirror()`: reflect-101 index of `idx`, which lies in
/// `-radius..size + radius` (guaranteed by `MIN_DIM`).
fn mirror(idx: isize, size: isize) -> usize {
    let m = if idx < 0 {
        -idx
    } else if idx >= size {
        2 * size - idx - 2
    } else {
        idx
    };
    m as usize
}

/// The five (mirrored) source row indices that feed output row `i`.
fn tap_rows(i: usize, h: usize) -> [usize; FILTER_WIDTH] {
    let mut rows = [0_usize; FILTER_WIDTH];
    for (k, slot) in rows.iter_mut().enumerate() {
        *slot = mirror(i as isize - RADIUS + k as isize, h as isize);
    }
    rows
}

/// The five rows of `plane` named by `rows`.
fn rows_of<'a, T: Sample>(plane: &PlaneView<'a, T>, rows: [usize; FILTER_WIDTH]) -> [&'a [T]; 5] {
    rows.map(|r| plane.row(r))
}

/// Vertical pass of an 8-bit row, `y_row[j] = (sum + 128) >> 8` in `i32`.
/// Returns the OR of the row, the C's `any_nonzero`.
fn y_pass_8(prev: &PlaneView<'_, u8>, cur: &PlaneView<'_, u8>, i: usize, y_row: &mut [i32]) -> i32 {
    let y_round: i32 = 1 << 7;
    let rows = tap_rows(i, prev.height());
    let (p, c) = (rows_of(prev, rows), rows_of(cur, rows));
    let mut any_nonzero: i32 = 0;
    for (j, out) in y_row.iter_mut().enumerate() {
        let mut accum: i32 = 0;
        for (&f, (pr, cr)) in FILTER.iter().zip(p.iter().zip(&c)) {
            let diff = i32::from(pr[j]) - i32::from(cr[j]);
            accum += i32::from(f) * diff;
        }
        *out = (accum + y_round) >> 8;
        any_nonzero |= *out;
    }
    any_nonzero
}

/// Vertical pass of a 9..16-bit row, `y_row[j] = (sum + 2^(bpc-1)) >> bpc`
/// with an `i64` accumulator. Returns the OR of the row.
fn y_pass_16(
    prev: &PlaneView<'_, u16>,
    cur: &PlaneView<'_, u16>,
    i: usize,
    bpc: u32,
    y_row: &mut [i32],
) -> i32 {
    let y_round: i32 = 1 << (bpc - 1);
    let rows = tap_rows(i, prev.height());
    let (p, c) = (rows_of(prev, rows), rows_of(cur, rows));
    let mut any_nonzero: i32 = 0;
    for (j, out) in y_row.iter_mut().enumerate() {
        let mut accum: i64 = 0;
        for (&f, (pr, cr)) in FILTER.iter().zip(p.iter().zip(&c)) {
            let diff = i32::from(pr[j]) - i32::from(cr[j]);
            accum += i64::from(f) * i64::from(diff);
        }
        *out = ((accum + i64::from(y_round)) >> bpc) as i32;
        any_nonzero |= *out;
    }
    any_nonzero
}

/// One horizontal output of a row, `(sum filter[k] * y_row[col_k] + 2^15) >> 16`
/// with the five source columns `cols`.
fn x_tap(y_row: &[i32], cols: [usize; FILTER_WIDTH]) -> u32 {
    let mut accum: i64 = 0;
    for (&f, col) in FILTER.iter().zip(cols) {
        accum += i64::from(f) * i64::from(y_row[col]);
    }
    let val = ((accum + (1_i64 << 15)) >> 16) as i32;
    val.unsigned_abs()
}

/// Horizontal pass of one row: the sum of `|(filter * y_row + 2^15) >> 16|`
/// in the C's `uint32_t`, which wraps. The interior columns need no mirror;
/// the arithmetic is the same.
fn x_pass(y_row: &[i32]) -> u32 {
    let w = y_row.len();
    let mut row_sad: u32 = 0;
    for j in 0..w {
        let interior = j >= RADIUS as usize && j + (RADIUS as usize) < w;
        let cols = if interior {
            [j - 2, j - 1, j, j + 1, j + 2]
        } else {
            let mut c = [0_usize; FILTER_WIDTH];
            for (k, slot) in c.iter_mut().enumerate() {
                *slot = mirror(j as isize - RADIUS + k as isize, w as isize);
            }
            c
        };
        row_sad = row_sad.wrapping_add(x_tap(y_row, cols));
    }
    row_sad
}

/// `motion_score_pipeline_8` over a plane pair of the init geometry;
/// `y_row` holds `width` entries.
pub fn sad_8(prev: &PlaneView<'_, u8>, cur: &PlaneView<'_, u8>, y_row: &mut [i32]) -> u64 {
    let mut sad: u64 = 0;
    for i in 0..prev.height() {
        if y_pass_8(prev, cur, i, y_row) != 0 {
            sad += u64::from(x_pass(y_row));
        }
    }
    sad
}

/// `motion_score_pipeline_16` over a plane pair at `bpc` bits (9..=16).
pub fn sad_16(
    prev: &PlaneView<'_, u16>,
    cur: &PlaneView<'_, u16>,
    bpc: u32,
    y_row: &mut [i32],
) -> u64 {
    let mut sad: u64 = 0;
    for i in 0..prev.height() {
        if y_pass_16(prev, cur, i, bpc, y_row) != 0 {
            sad += u64::from(x_pass(y_row));
        }
    }
    sad
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Generator of the C reference program the table below comes from.
    struct Lcg(u32);
    impl Lcg {
        fn next(&mut self) -> u32 {
            self.0 = self.0.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            self.0 >> 8
        }
    }

    /// `(w, h, bpc, mode, sad)`: `motion_score_pipeline_8/16` of integer_motion.c,
    /// compiled with gcc, on random (0), one-bit-off (1) and sparse-difference
    /// (2) frames, in generation order.
    const C_SAD: &[(usize, usize, u32, u32, u64)] = &[
        (3, 3, 8, 0, 42994),
        (3, 3, 8, 1, 309),
        (3, 3, 8, 2, 145),
        (3, 3, 10, 0, 99025),
        (3, 3, 10, 1, 145),
        (3, 3, 10, 2, 37),
        (3, 3, 12, 0, 71732),
        (3, 3, 12, 1, 18),
        (3, 3, 12, 2, 8),
        (3, 3, 16, 0, 53638),
        (3, 3, 16, 1, 9),
        (3, 3, 16, 2, 0),
        (4, 7, 8, 0, 213733),
        (4, 7, 8, 1, 2364),
        (4, 7, 8, 2, 125),
        (4, 7, 10, 0, 260822),
        (4, 7, 10, 1, 410),
        (4, 7, 10, 2, 30),
        (4, 7, 12, 0, 217020),
        (4, 7, 12, 1, 159),
        (4, 7, 12, 2, 6),
        (4, 7, 16, 0, 330472),
        (4, 7, 16, 1, 2),
        (4, 7, 16, 2, 0),
        (17, 5, 8, 0, 645600),
        (17, 5, 8, 1, 4717),
        (17, 5, 8, 2, 125),
        (17, 5, 10, 0, 422001),
        (17, 5, 10, 1, 1609),
        (17, 5, 10, 2, 30),
        (17, 5, 12, 0, 691593),
        (17, 5, 12, 1, 332),
        (17, 5, 12, 2, 6),
        (17, 5, 16, 0, 726371),
        (17, 5, 16, 1, 15),
        (17, 5, 16, 2, 0),
        (64, 48, 8, 0, 19922560),
        (64, 48, 8, 1, 186641),
        (64, 48, 8, 2, 8108),
        (64, 48, 10, 0, 18912042),
        (64, 48, 10, 1, 46748),
        (64, 48, 10, 2, 2037),
        (64, 48, 12, 0, 19735706),
        (64, 48, 12, 1, 11343),
        (64, 48, 12, 2, 448),
        (64, 48, 16, 0, 18831157),
        (64, 48, 16, 1, 90),
        (64, 48, 16, 2, 0),
        (576, 324, 8, 0, 1145798851),
        (576, 324, 8, 1, 11169885),
        (576, 324, 8, 2, 1007119),
        (576, 324, 10, 0, 1150206905),
        (576, 324, 10, 1, 2797734),
        (576, 324, 10, 2, 250156),
        (576, 324, 12, 0, 1150651139),
        (576, 324, 12, 1, 678816),
        (576, 324, 12, 2, 88655),
        (576, 324, 16, 0, 1155190384),
        (576, 324, 16, 1, 3969),
        (576, 324, 16, 2, 0),
        (33, 129, 8, 0, 26541168),
        (33, 129, 8, 1, 260939),
        (33, 129, 8, 2, 11196),
        (33, 129, 10, 0, 26846013),
        (33, 129, 10, 1, 65529),
        (33, 129, 10, 2, 2800),
        (33, 129, 12, 0, 26805305),
        (33, 129, 12, 1, 16028),
        (33, 129, 12, 2, 616),
        (33, 129, 16, 0, 26385510),
        (33, 129, 16, 1, 470),
        (33, 129, 16, 2, 0),
    ];

    fn frame(rng: &mut Lcg, n: usize, bpc: u32, mode: u32) -> (Vec<u16>, Vec<u16>) {
        let mx = (1_u32 << bpc) - 1;
        let (mut p, mut c) = (vec![0_u16; n], vec![0_u16; n]);
        for i in 0..n {
            let a = rng.next() & mx;
            let mut b = match mode {
                0 => rng.next() & mx,
                1 => (a ^ 1) & mx,
                _ => a,
            };
            if mode == 2 && i % 97 == 0 {
                b = (a + 1) & mx;
            }
            p[i] = a as u16;
            c[i] = b as u16;
        }
        (p, c)
    }

    fn narrow(v: &[u16]) -> Vec<u8> {
        v.iter().map(|&x| x as u8).collect()
    }

    #[test]
    fn sad_matches_the_c_pipelines() -> Result<(), Error> {
        let mut rng = Lcg(12345);
        let mut cases = C_SAD.iter();
        for (w, h) in [(3, 3), (4, 7), (17, 5), (64, 48), (576, 324), (33, 129)] {
            for bpc in [8_u32, 10, 12, 16] {
                for mode in 0..3 {
                    let (p, c) = frame(&mut rng, w * h, bpc, mode);
                    let mut y = vec![0_i32; w];
                    let got = if bpc == 8 {
                        let (p, c) = (narrow(&p), narrow(&c));
                        let pv = PlaneView::from_slice(&p, w, h, w)?;
                        let cv = PlaneView::from_slice(&c, w, h, w)?;
                        sad_8(&pv, &cv, &mut y)
                    } else {
                        let pv = PlaneView::from_slice(&p, w, h, w)?;
                        let cv = PlaneView::from_slice(&c, w, h, w)?;
                        sad_16(&pv, &cv, bpc, &mut y)
                    };
                    assert_eq!(cases.next(), Some(&(w, h, bpc, mode, got)));
                }
            }
        }
        Ok(())
    }

    #[test]
    fn identical_frames_have_no_sad() -> Result<(), Error> {
        let p = vec![200_u8; 5 * 4];
        let pv = PlaneView::from_slice(&p, 5, 4, 5)?;
        assert_eq!(sad_8(&pv, &pv, &mut [0; 5]), 0);
        Ok(())
    }

    #[test]
    fn mirror_is_reflect_101() {
        assert_eq!(mirror(-2, 5), 2);
        assert_eq!(mirror(-1, 5), 1);
        assert_eq!(mirror(0, 5), 0);
        assert_eq!(mirror(5, 5), 3);
        assert_eq!(mirror(6, 5), 2);
    }
}
