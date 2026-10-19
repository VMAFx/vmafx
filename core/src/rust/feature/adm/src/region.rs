// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Frame-border bookkeeping of the integer ADM reductions, ported from
// core/src/feature/integer_adm_kernels.h (adm_border(), adm_border_filt(),
// adm_cm_bounds()) and adm_csf_fixed_point.h (adm_half_shift()).

use vmafx_fex::libm;

/// `ADM_BORDER_FACTOR`.
const ADM_BORDER_FACTOR: f64 = 0.1;

/// `AdmBorder`: half-open column range [left, right) and row range
/// [top, bottom).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Border {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

impl Border {
    /// `(bottom - top) * (right - left)`.
    pub const fn area(&self) -> i32 {
        (self.bottom - self.top) * (self.right - self.left)
    }
}

/// C `(int)(x)`: truncation toward zero; `x` is a small finite value here.
#[allow(clippy::cast_possible_truncation)]
fn trunc_int(x: f64) -> i32 {
    x as i32
}

/// `adm_border()`: the region that takes part in the reductions.
pub fn border(w: i32, h: i32) -> Border {
    let left = trunc_int(f64::from(w) * ADM_BORDER_FACTOR - 0.5);
    let top = trunc_int(f64::from(h) * ADM_BORDER_FACTOR - 0.5);
    Border {
        left,
        top,
        right: w - left,
        bottom: h - top,
    }
}

/// `adm_border_filt()`: the region widened by one filter tap (-1 / +2) and
/// clamped to the frame, for the decouple and CSF stages.
pub fn border_filt(w: i32, h: i32) -> Border {
    let mut b = Border {
        left: trunc_int(f64::from(w) * ADM_BORDER_FACTOR - 0.5 - 1.0),
        top: trunc_int(f64::from(h) * ADM_BORDER_FACTOR - 0.5 - 1.0),
        right: 0,
        bottom: 0,
    };
    b.right = w - b.left + 2;
    b.bottom = h - b.top + 2;
    b.left = b.left.max(0);
    b.right = b.right.min(w);
    b.top = b.top.max(0);
    b.bottom = b.bottom.min(h);
    b
}

/// `AdmCmBounds`: the rows and columns a contrast-masking reduction visits.
#[derive(Clone, Copy, Debug)]
pub struct CmBounds {
    pub b: Border,
    pub left_edge: bool,
    pub right_edge: bool,
    pub start_col: i32,
    pub end_col: i32,
    pub start_row: i32,
    pub end_row: i32,
}

/// `adm_cm_bounds()`.
pub fn cm_bounds(w: i32, h: i32) -> CmBounds {
    let b = border(w, h);
    CmBounds {
        b,
        left_edge: b.left <= 0,
        right_edge: b.right > (w - 1),
        start_col: if b.left > 1 { b.left } else { 1 },
        end_col: if b.right < (w - 1) { b.right } else { w - 1 },
        start_row: if b.top > 1 { b.top } else { 1 },
        end_row: if b.bottom < (h - 1) { b.bottom } else { h - 1 },
    }
}

/// `adm_half_shift()`: 2^(shift - 1), or 0 for a zero shift.
pub const fn half_shift(shift: u32) -> u32 {
    if shift > 0 { 1_u32 << (shift - 1) } else { 0 }
}

/// C `(uint32_t)ceil(log2(n) - sub)`: the shift budgets. The argument is at
/// least 9 - 4 > -1, so `ceil` gives a value in [-0, 31] and the conversion is
/// the C one (`-0.0` -> 0).
#[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
pub fn ceil_log2_u32(n: i32, sub: f64) -> u32 {
    (libm::log2(f64::from(n)) - sub).ceil() as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn border_matches_the_c_truncation() {
        assert_eq!(
            border(288, 162),
            Border {
                left: 28,
                top: 15,
                right: 260,
                bottom: 147
            }
        );
        assert_eq!(
            border(9, 9),
            Border {
                left: 0,
                top: 0,
                right: 9,
                bottom: 9
            }
        );
        assert_eq!(
            border_filt(9, 9),
            Border {
                left: 0,
                top: 0,
                right: 9,
                bottom: 9
            }
        );
        assert_eq!(
            border_filt(288, 162),
            Border {
                left: 27,
                top: 14,
                right: 263,
                bottom: 150
            }
        );
    }

    #[test]
    fn half_shift_of_zero_is_zero() {
        assert_eq!(half_shift(0), 0);
        assert_eq!(half_shift(5), 16);
        assert_eq!(ceil_log2_u32(9, 4.0), 0);
        assert_eq!(ceil_log2_u32(288, 4.0), 5);
    }
}
